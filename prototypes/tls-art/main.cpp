// Wings prototype: fetch cover art over HTTPS, decode it with the app's own
// image code, cache it on disk, and report numbers.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   tls_art_test --ca ca-bundle.crt --library library.json [--sample 120]
//                [--workers 4] [--cache /tmp/wings-art] [--budget-mb 64]
//   tls_art_test --ca ca-bundle.crt --negative
//
// Decoding uses orbit::image::decode from the vendored app sources
// (src/orbit/image.cpp + stb_image + libwebp), compiled unmodified.

#include "orbit/image.hpp"
#include "doh.hpp"
#include "tls_client.hpp"

#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace wings;

namespace
{

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string slurp(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string host_of(const std::string &url)
{
    const std::size_t a = url.find("://");
    const std::size_t b = url.find('/', a + 3);
    return url.substr(a + 3, b == std::string::npos ? std::string::npos : b - a - 3);
}

// Pulls every "posterUrl":"..." string out of the catalogue without a JSON
// library (the app would use its streaming parser).
std::vector<std::string> poster_urls(const std::string &json)
{
    std::vector<std::string> out;
    const std::string needle = "\"posterUrl\"";
    for (std::size_t pos = json.find(needle); pos != std::string::npos;
         pos = json.find(needle, pos + 1))
    {
        std::size_t i = json.find(':', pos + needle.size());
        if (i == std::string::npos)
            break;
        while (++i < json.size() && (json[i] == ' ' || json[i] == '\n'))
        {
        }
        if (i >= json.size() || json[i] != '"')
            continue;
        std::string s;
        for (++i; i < json.size() && json[i] != '"'; ++i)
        {
            if (json[i] == '\\' && i + 1 < json.size())
            {
                ++i;
                if (json[i] == 'u' && i + 4 < json.size())
                {
                    const unsigned cp = static_cast<unsigned>(std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
                    if (cp < 0x80)
                        s.push_back(static_cast<char>(cp));
                    else if (cp < 0x800)
                    {
                        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        s.push_back(static_cast<char>(0x80 | (cp & 63)));
                    }
                    else
                    {
                        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                        s.push_back(static_cast<char>(0x80 | (cp & 63)));
                    }
                    i += 4;
                }
                else
                    s.push_back(json[i] == 'n' ? '\n' : json[i]);
            }
            else
                s.push_back(json[i]);
        }
        if (s.compare(0, 8, "https://") == 0)
            out.push_back(s);
    }
    return out;
}

std::uint64_t fnv1a(const std::string &s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

// ------------------------------------------------------------ disk cache
// <dir>/<xx>/<16 hex>.art holds the ORIGINAL compressed bytes. Eviction is
// least-recently-used by mtime (touched on every hit) down to a byte budget.
struct DiskCache
{
    std::string dir;
    std::string path_for(const std::string &url) const
    {
        char name[40];
        const std::uint64_t h = fnv1a(url);
        std::snprintf(name, sizeof name, "%02x/%016llx.art", static_cast<unsigned>(h >> 56),
                      static_cast<unsigned long long>(h));
        return dir + "/" + name;
    }
    bool read(const std::string &url, std::string *out) const
    {
        const std::string p = path_for(url);
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        *out = ss.str();
        utimes(p.c_str(), nullptr); // LRU touch
        return !out->empty();
    }
    void write(const std::string &url, const std::uint8_t *data, std::size_t n) const
    {
        const std::string p = path_for(url);
        mkdir(dir.c_str(), 0755);
        mkdir(p.substr(0, p.rfind('/')).c_str(), 0755);
        const std::string tmp = p + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(n));
        }
        rename(tmp.c_str(), p.c_str()); // atomic publish; half files never appear as entries
    }
    struct Entry
    {
        std::string path;
        long long size;
        long mtime;
    };
    std::vector<Entry> scan() const
    {
        std::vector<Entry> v;
        DIR *d = opendir(dir.c_str());
        if (!d)
            return v;
        while (dirent *e = readdir(d))
        {
            if (e->d_name[0] == '.')
                continue;
            const std::string sub = dir + "/" + e->d_name;
            DIR *s = opendir(sub.c_str());
            if (!s)
                continue;
            while (dirent *f = readdir(s))
            {
                const std::string fp = sub + "/" + f->d_name;
                struct stat st;
                if (stat(fp.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                {
                    if (fp.size() > 4 && fp.compare(fp.size() - 4, 4, ".tmp") == 0)
                        unlink(fp.c_str()); // crash leftovers
                    else
                        v.push_back({fp, st.st_size, static_cast<long>(st.st_mtime)});
                }
            }
            closedir(s);
        }
        closedir(d);
        return v;
    }
    // Returns (files removed, bytes before, bytes after).
    std::tuple<int, long long, long long> evict(long long budget) const
    {
        auto v = scan();
        long long total = 0;
        for (auto &e : v)
            total += e.size;
        const long long before = total;
        std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) { return a.mtime < b.mtime; });
        int removed = 0;
        for (auto &e : v)
        {
            if (total <= budget)
                break;
            if (unlink(e.path.c_str()) == 0)
            {
                total -= e.size;
                ++removed;
            }
        }
        return {removed, before, total};
    }
};

// ------------------------------------------------------------ job queue
// Lower priority number first (0 = visible tile, 1 = near, 2 = prefetch);
// within a priority the NEWEST request goes first (what the player is looking
// at now), mirroring ArtCache's newest-first rule.
struct Job
{
    std::string url;
    int priority = 2;
    std::uint64_t seq = 0;
};

struct Result
{
    std::string url;
    int priority = 0;
    bool ok = false;
    bool from_disk = false;
    std::string error;
    std::size_t bytes = 0;
    int w = 0, h = 0;
    double net_ms = 0, decode_ms = 0, queue_ms = 0;
    tls::Timing timing;
    int order = 0; // completion rank
    std::string type;
};

class Pool
{
  public:
    Pool(std::shared_ptr<tls::Shared> shared, tls::Limits limits, DiskCache cache, int workers,
         int cover)
        : shared_(std::move(shared)), limits_(limits), cache_(std::move(cache)), cover_(cover)
    {
        for (int i = 0; i < workers; ++i)
            threads_.emplace_back([this] { run(); });
    }
    ~Pool()
    {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_)
            t.join();
    }
    void submit(const std::string &url, int priority)
    {
        std::lock_guard<std::mutex> l(m_);
        queue_.push_back({url, priority, ++seq_});
        enqueued_[url] = Clock::now();
        cv_.notify_one();
    }
    void wait_idle()
    {
        std::unique_lock<std::mutex> l(m_);
        idle_cv_.wait(l, [this] { return queue_.empty() && busy_ == 0; });
    }
    std::vector<Result> results()
    {
        std::lock_guard<std::mutex> l(m_);
        return results_;
    }
    std::size_t peak_inflight_bytes() const { return peak_bytes_; }

  private:
    void run()
    {
        tls::Client client(shared_, limits_);
        for (;;)
        {
            Job job;
            Clock::time_point queued;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty())
                    return;
                auto best = queue_.begin();
                for (auto it = queue_.begin(); it != queue_.end(); ++it)
                    if (it->priority < best->priority ||
                        (it->priority == best->priority && it->seq > best->seq))
                        best = it;
                job = *best;
                queue_.erase(best);
                queued = enqueued_[job.url];
                ++busy_;
            }
            Result r = process(client, job);
            r.queue_ms = std::chrono::duration<double, std::milli>(Clock::now() - queued).count();
            {
                std::lock_guard<std::mutex> l(m_);
                r.order = static_cast<int>(results_.size());
                results_.push_back(std::move(r));
                --busy_;
                if (queue_.empty() && busy_ == 0)
                    idle_cv_.notify_all();
            }
        }
    }

    Result process(tls::Client &client, const Job &job)
    {
        Result r;
        r.url = job.url;
        r.priority = job.priority;
        std::string bytes;
        const auto t0 = Clock::now();
        if (cache_.read(job.url, &bytes))
        {
            r.from_disk = true;
        }
        else
        {
            tls::Response resp = client.get(job.url);
            r.timing = resp.timing;
            r.type = resp.content_type;
            if (!resp.ok())
            {
                r.error = std::string(tls::name(resp.error)) + ": " + resp.detail;
                r.net_ms = ms_since(t0);
                return r;
            }
            bytes.assign(resp.body.begin(), resp.body.end());
            {
                std::lock_guard<std::mutex> l(m_);
                inflight_ += bytes.size();
                peak_bytes_ = std::max(peak_bytes_, inflight_);
            }
            // Sniff before caching: never store something we cannot decode.
            if (orbit::image::sniff(bytes) == orbit::image::Format::unknown)
            {
                r.error = "undecodable: unknown image format (" + resp.content_type + ")";
                r.net_ms = ms_since(t0);
                std::lock_guard<std::mutex> l(m_);
                inflight_ -= bytes.size();
                return r;
            }
            cache_.write(job.url, reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
            std::lock_guard<std::mutex> l(m_);
            inflight_ -= bytes.size();
        }
        r.net_ms = ms_since(t0);
        r.bytes = bytes.size();
        const auto t1 = Clock::now();
        orbit::image::Pixels px;
        std::string err;
        if (!orbit::image::decode(bytes, cover_, cover_, &px, &err))
        {
            r.error = "decode: " + err;
            return r;
        }
        r.decode_ms = ms_since(t1);
        r.w = px.width;
        r.h = px.height;
        r.ok = true;
        return r;
    }

    std::shared_ptr<tls::Shared> shared_;
    tls::Limits limits_;
    DiskCache cache_;
    int cover_;
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_, idle_cv_;
    std::vector<Job> queue_;
    std::map<std::string, Clock::time_point> enqueued_;
    std::vector<Result> results_;
    std::uint64_t seq_ = 0;
    int busy_ = 0;
    bool stop_ = false;
    std::size_t inflight_ = 0, peak_bytes_ = 0;
};

double pct(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

long rss_kb()
{
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss;
}

int negative_tests(std::shared_ptr<tls::Shared> shared)
{
    struct Case
    {
        const char *what;
        std::string url;
        tls::Error expect;
        std::size_t cap;
    };
    const std::vector<Case> cases = {
        {"plain http refused", "http://example.com/a.png", tls::Error::bad_url, 4u << 20},
        {"credentials in URL refused", "https://user:pw@example.com/a.png", tls::Error::bad_url, 4u << 20},
        {"expired certificate", "https://expired.badssl.com/", tls::Error::handshake, 4u << 20},
        {"wrong host name", "https://wrong.host.badssl.com/", tls::Error::handshake, 4u << 20},
        {"self-signed certificate", "https://self-signed.badssl.com/", tls::Error::handshake, 4u << 20},
        {"untrusted root", "https://untrusted-root.badssl.com/", tls::Error::handshake, 4u << 20},
        {"unresolvable host", "https://no-such-host.invalid/x.png", tls::Error::dns, 4u << 20},
        {"loopback literal refused (Pegasus)", "https://127.0.0.1:6970/a.png", tls::Error::dns, 4u << 20},
        {"RFC1918 literal refused", "https://192.168.1.1/a.png", tls::Error::dns, 4u << 20},
        {"redirect to http refused", "https://httpbin.org/redirect-to?url=http%3A%2F%2Fexample.com%2F", tls::Error::redirect, 4u << 20},
        {"too many redirects", "https://httpbin.org/redirect/6", tls::Error::redirect, 4u << 20},
        {"HTML is not an image", "https://example.com/", tls::Error::bad_type, 4u << 20},
        {"size cap (cap 64 KiB)", "https://cdn.jsdelivr.net/npm/jquery@3.7.1/dist/jquery.min.js", tls::Error::bad_type, 64u << 10},
        {"size cap on real image (cap 4 KiB)", "https://image.api.playstation.com/vulcan/ap/rnd/202211/2218/9UflSkyn6ZUATU2VJNcMQ49b.png?w=260", tls::Error::too_large, 4u << 10},
        {"HTTP 404", "https://cdn.jsdelivr.net/gh/nexgen999/evoX-CoreOS@main/assets/icon/does-not-exist.png", tls::Error::http_status, 4u << 20},
    };
    int bad = 0;
    for (const auto &c : cases)
    {
        tls::Limits lim;
        lim.max_body = c.cap;
        lim.total_timeout_ms = 20000;
        tls::Client client(shared, lim);
        const auto t0 = Clock::now();
        tls::Response r = client.get(c.url);
        const bool pass = r.error == c.expect;
        std::printf("  [%s] %-36s -> %-11s %s (%.0f ms)\n", pass ? "PASS" : "FAIL", c.what,
                    tls::name(r.error), r.detail.c_str(), ms_since(t0));
        bad += pass ? 0 : 1;
    }
    return bad;
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    std::string ca, library, cache = "/tmp/wings-art-cache";
    int sample = 120, workers = 4, budget_mb = 64, cover = 400;
    bool mem_test = false, negative = false, priority_demo = true, use_doh = false, dns_test = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--ca") ca = next();
        else if (a == "--library") library = next();
        else if (a == "--sample") sample = std::atoi(next().c_str());
        else if (a == "--workers") workers = std::atoi(next().c_str());
        else if (a == "--cache") cache = next();
        else if (a == "--budget-mb") budget_mb = std::atoi(next().c_str());
        else if (a == "--negative") negative = true;
        else if (a == "--doh") use_doh = true;
        else if (a == "--mem") mem_test = true;
        else if (a == "--dns-test") dns_test = true;
        else if (a == "--no-priority-demo") priority_demo = false;
    }
    const std::string pem = slurp(ca);
    std::string info;
    const std::size_t heap_before_ctx = mallinfo2().uordblks;
    auto shared = tls::Shared::create(pem, &info);
    if (!shared)
    {
        std::fprintf(stderr, "CA bundle: %s\n", info.c_str());
        return 2;
    }
    std::printf("trust store: %s (%zu bytes of PEM from %s)\n", info.c_str(), pem.size(), ca.c_str());

    std::printf("  heap for SSL_CTX + parsed trust store: %.1f KiB\n",
                static_cast<double>(mallinfo2().uordblks - heap_before_ctx) / 1024.0);

    std::unique_ptr<tls::DohResolver> doh;
    if (use_doh || dns_test)
    {
        doh.reset(new tls::DohResolver(shared, {"https://1.1.1.1/dns-query", "https://8.8.8.8/dns-query"}));
        if (use_doh)
            doh->install();
        std::printf("name resolution: %s\n", use_doh ? "DNS-over-HTTPS (1.1.1.1, fallback 8.8.8.8)" : "getaddrinfo");
    }
    if (dns_test)
    {
        const char *hosts[] = {"image.api.playstation.com", "cdn.jsdelivr.net", "raw.githubusercontent.com",
                               "cdn.prosperopatches.com", "dlpsgame.com", "no-such-host.invalid"};
        for (const char *h : hosts)
        {
            auto show = [](const std::vector<std::uint32_t> &v)
            {
                std::string s;
                for (std::uint32_t ip : v)
                {
                    char b[32];
                    inet_ntop(AF_INET, &ip, b, sizeof b);
                    s += std::string(s.empty() ? "" : ",") + b;
                }
                return s;
            };
            std::vector<std::uint32_t> a, b;
            int ttl = 0;
            std::string why1, why2;
            auto t1 = Clock::now();
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *res = nullptr;
            const bool ok1 = getaddrinfo(h, nullptr, &hints, &res) == 0 && res;
            for (addrinfo *p = res; ok1 && p; p = p->ai_next)
                a.push_back(reinterpret_cast<sockaddr_in *>(p->ai_addr)->sin_addr.s_addr);
            if (res)
                freeaddrinfo(res);
            const double d1 = ms_since(t1);
            auto t2 = Clock::now();
            const bool ok2 = doh->resolve(h, &b, &ttl, &why2);
            const double d2 = ms_since(t2);
            std::printf("  %-28s getaddrinfo %5.1f ms [%s]   DoH %6.1f ms ttl %d [%s%s]\n", h, d1, ok1 ? show(a).c_str() : "fail",
                        d2, ttl, ok2 ? show(b).c_str() : "fail ", ok2 ? "" : why2.c_str());
        }
        std::vector<std::uint32_t> ips;
        int ttl = 0;
        std::string why;
        const auto q = tls::DohResolver::build_query("cdn.jsdelivr.net");
        std::printf("  query wire size for cdn.jsdelivr.net: %zu bytes; truncated reply parse -> %s\n", q.size(),
                    tls::DohResolver::parse_response(q.data(), q.size(), &ips, &ttl, &why) ? "accepted?!" : why.c_str());
        return 0;
    }

    if (mem_test)
    {
        // Live heap per idle HTTPS connection (OpenSSL objects + buffers), measured
        // with glibc mallinfo2 (the app's mspace would track within a few percent).
        const char *url = "https://cdn.jsdelivr.net/gh/nexgen999/evoX-CoreOS@main/assets/icon/FPKGi.png";
        { tls::Client warm(shared); warm.get(url); } // loads ciphers/tables once
        const std::size_t base = mallinfo2().uordblks;
        std::vector<std::unique_ptr<tls::Client>> clients;
        for (int i = 1; i <= 8; ++i)
        {
            clients.emplace_back(new tls::Client(shared));
            tls::Response r = clients.back()->get(url);
            std::printf("  %d idle connection(s): live heap +%.1f KiB (%.1f KiB each)%s\n", i,
                        static_cast<double>(mallinfo2().uordblks - base) / 1024.0,
                        static_cast<double>(mallinfo2().uordblks - base) / 1024.0 / i, r.ok() ? "" : " [request failed]");
        }
        return 0;
    }

    if (negative)
    {
        std::printf("negative / safety tests:\n");
        return negative_tests(shared) == 0 ? 0 : 1;
    }

    std::vector<std::string> all = poster_urls(slurp(library));
    std::set<std::string> uniq(all.begin(), all.end());
    std::printf("library: %zu posterUrls, %zu unique\n", all.size(), uniq.size());
    std::map<std::string, int> per_host;
    for (auto &u : uniq)
        per_host[host_of(u)]++;
    for (auto &kv : per_host)
        std::printf("  %-52s %d\n", kv.first.c_str(), kv.second);

    // Sample: up to 6 from each non-main host, rest spread evenly over the main host.
    std::vector<std::string> urls, main_host;
    std::map<std::string, int> taken;
    for (auto &u : uniq)
    {
        const std::string h = host_of(u);
        if (per_host[h] > 500)
            main_host.push_back(u);
        else if (taken[h]++ < 6)
            urls.push_back(u);
    }
    const int want = std::max(0, sample - static_cast<int>(urls.size()));
    for (int i = 0; i < want && !main_host.empty(); ++i)
        urls.push_back(main_host[static_cast<std::size_t>(i) * main_host.size() / static_cast<std::size_t>(want)]);
    std::printf("sample: %zu urls, %d workers, cover %d px, cache %s\n", urls.size(), workers, cover,
                cache.c_str());

    tls::Limits limits;
    DiskCache dc{cache};
    Pool pool(shared, limits, dc, workers, cover);

    const auto t0 = Clock::now();
    // Everything starts as low-priority prefetch; the last 12 are then
    // re-requested as "visible" after the pool is busy, to show they jump the queue.
    std::vector<std::string> visible;
    if (priority_demo && urls.size() > 40)
        for (std::size_t i = 0; i < 12; ++i)
            visible.push_back(urls[urls.size() - 1 - i]);
    for (auto &u : urls)
        if (std::find(visible.begin(), visible.end(), u) == visible.end())
            pool.submit(u, 2);
    if (!visible.empty())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (auto &u : visible)
            pool.submit(u, 0);
    }
    pool.wait_idle();
    const double wall = ms_since(t0);

    auto res = pool.results();
    int ok = 0, disk = 0, resumed = 0, reused = 0, cached_dns = 0, nets = 0;
    long long bytes = 0;
    std::vector<double> net, dec, ttfb;
    std::map<std::string, int> fails;
    double hs = 0;
    int hs_n = 0;
    for (auto &r : res)
    {
        if (r.ok)
        {
            ++ok;
            bytes += static_cast<long long>(r.bytes);
            dec.push_back(r.decode_ms);
        }
        else
            fails[r.error.substr(0, 90)]++;
        if (r.from_disk)
            ++disk;
        else if (r.error.empty() || r.timing.total_ms > 0)
        {
            ++nets;
            net.push_back(r.net_ms);
            if (r.timing.reused) ++reused;
            if (r.timing.resumed) ++resumed;
            if (r.timing.dns_cached) ++cached_dns;
            if (!r.timing.reused && r.timing.tls_ms > 0)
            {
                hs += r.timing.dns_ms + r.timing.connect_ms + r.timing.tls_ms;
                ++hs_n;
            }
            ttfb.push_back(r.timing.first_byte_ms);
        }
    }
    std::printf("\nRESULT: %d/%zu ok in %.2f s wall (%.1f img/s)\n", ok, res.size(), wall / 1000.0,
                static_cast<double>(res.size()) / (wall / 1000.0));
    std::printf("  from disk cache: %d, network fetches: %d\n", disk, nets);
    std::printf("  compressed bytes: %.1f KiB total, %.1f KiB avg\n", static_cast<double>(bytes) / 1024.0,
                ok ? static_cast<double>(bytes) / 1024.0 / ok : 0.0);
    std::printf("  network ms per image: p50 %.0f  p95 %.0f  max %.0f\n", pct(net, 0.5), pct(net, 0.95), pct(net, 1));
    std::printf("  time to first byte ms: p50 %.0f  p95 %.0f\n", pct(ttfb, 0.5), pct(ttfb, 0.95));
    std::printf("  new-connection setup (dns+tcp+tls) avg: %.0f ms over %d connections\n", hs_n ? hs / hs_n : 0.0, hs_n);
    std::printf("  keep-alive reuse: %d/%d requests; TLS resumption: %d; DNS cache hits: %d\n", reused, nets, resumed, cached_dns);
    std::printf("  decode ms (webp/png/jpg -> %d px RGBA): p50 %.1f  p95 %.1f\n", cover, pct(dec, 0.5), pct(dec, 0.95));
    std::printf("  peak in-flight compressed bytes: %.1f KiB; process peak RSS: %.1f MiB\n",
                static_cast<double>(pool.peak_inflight_bytes()) / 1024.0, static_cast<double>(rss_kb()) / 1024.0);
    for (auto &kv : fails)
        std::printf("  FAIL x%d: %s\n", kv.second, kv.first.c_str());

    if (!visible.empty())
    {
        double sum = 0;
        int n = 0;
        int worst = 0;
        for (auto &r : res)
            if (r.priority == 0)
            {
                sum += r.order;
                ++n;
                worst = std::max(worst, r.order);
            }
        std::printf("  priority demo: %zu prefetch jobs queued first, then 12 visible jobs after 300 ms;\n"
                    "    visible jobs finished at completion ranks avg %.1f, worst %d of %zu\n",
                    urls.size() - visible.size(), n ? sum / n : 0.0, worst, res.size());
    }

    std::map<std::string, std::pair<int, int>> byhost; // ok, total
    for (auto &r : res)
    {
        auto &p = byhost[host_of(r.url)];
        p.second++;
        p.first += r.ok ? 1 : 0;
    }
    for (auto &kv : byhost)
        std::printf("  host %-52s %d/%d ok\n", kv.first.c_str(), kv.second.first, kv.second.second);

    auto [removed, before, after] = dc.evict(static_cast<long long>(budget_mb) << 20);
    std::printf("  disk cache: %.2f MiB before eviction; budget %d MiB -> removed %d files, %.2f MiB left\n",
                static_cast<double>(before) / 1048576.0, budget_mb, removed, static_cast<double>(after) / 1048576.0);
    return ok > 0 ? 0 : 1;
}
