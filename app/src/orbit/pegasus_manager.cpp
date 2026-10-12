// Wings TV app - "Pegasus DL on this console" (see pegasus_manager.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_manager.hpp"

#include "core/save_file.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"
#include "orbit/pegasus_load_flow.hpp" // sha256_file
#include "orbit/pegasus_version.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace orbit
{

namespace
{

constexpr const char *kRepoPath = "/pegasus-ps5/pegasus-dl/releases/download/";
constexpr std::size_t kMaxReleases = 10;

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::int64_t system_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Creates every missing directory of an absolute path.
bool make_dirs(const std::string &path)
{
    for (std::size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
        {
            const std::string part = path.substr(0, i);
            struct stat st;
            if (stat(part.c_str(), &st) == 0)
                continue;
            if (!hui::save::ensure_directory(part))
                return false;
        }
    return true;
}

std::string clean_stamp(std::string_view text)
{
    std::string out;
    for (char c : text.substr(0, 40))
        if ((c >= '0' && c <= '9') || c == 'T' || c == 'Z' || c == ':' || c == '-' || c == '+' || c == '.')
            out.push_back(c);
    return out;
}

} // namespace

// ---- pure rules ----

bool pegasus_tag_ok(std::string_view tag)
{
    if (tag.size() < 6 || tag.size() > 24 || tag[0] != 'v')
        return false;
    int dots = 0;
    bool digit_before = false;
    for (std::size_t i = 1; i < tag.size(); ++i)
    {
        const char c = tag[i];
        if (c >= '0' && c <= '9')
            digit_before = true;
        else if (c == '.' && digit_before && i + 1 < tag.size())
        {
            ++dots;
            digit_before = false;
        }
        else
            return false;
    }
    return dots == 2 && digit_before;
}

std::string pegasus_download_url(std::string_view tag)
{
    if (!pegasus_tag_ok(tag))
        return std::string();
    return "https://github.com" + std::string(kRepoPath) + std::string(tag) + "/" + kPegasusAssetName;
}

bool pegasus_url_allowed(std::string_view url, PegasusUrlRole role, std::string_view tag)
{
    if (role == PegasusUrlRole::first)
    {
        const std::string want = pegasus_download_url(tag);
        return !want.empty() && url == want;
    }
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size() || url.size() > 2048 || lower(url.substr(0, scheme.size())) != scheme)
        return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f || c == '\\')
            return false;
    const std::string_view rest = url.substr(scheme.size());
    const std::size_t end = rest.find_first_of("/?#");
    const std::string_view authority = end == std::string_view::npos ? rest : rest.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        return false;
    std::string host(authority);
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos)
    {
        if (host.substr(colon + 1) != "443")
            return false;
        host.resize(colon);
    }
    host = lower(host);
    return host == "github.com" || host == "objects.githubusercontent.com" ||
           host == "release-assets.githubusercontent.com";
}

bool parse_sha256_digest(std::string_view digest, std::string *hex)
{
    constexpr std::string_view prefix = "sha256:";
    if (digest.size() != prefix.size() + 64 || lower(digest.substr(0, prefix.size())) != prefix)
        return false;
    std::string h = lower(digest.substr(prefix.size()));
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    if (hex != nullptr)
        *hex = std::move(h);
    return true;
}

int compare_pegasus_versions(std::string_view a, std::string_view b, bool *ok)
{
    int x[3], y[3];
    const bool good = parse_pegasus_version(a, x) && parse_pegasus_version(b, y);
    if (ok != nullptr)
        *ok = good;
    if (!good)
        return 0;
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool parse_pegasus_releases(std::string_view text, std::vector<PegasusRelease> *out, std::string *error)
{
    out->clear();
    json::Value root;
    std::string why;
    if (!json::parse(text, &root, &why) || !root.is_array())
    {
        if (error != nullptr)
            *error = why.empty() ? std::string("not a list of releases") : why;
        return false;
    }
    for (const json::Value &item : root.items())
    {
        if (!item.is_object() || item.flag("draft"))
            continue;
        PegasusRelease r;
        r.tag = item.text("tag_name");
        if (!pegasus_tag_ok(r.tag))
            continue;
        r.published_at = clean_stamp(item.text("published_at"));
        r.prerelease = item.flag("prerelease");
        for (const json::Value &asset : item["assets"].items())
        {
            if (!asset.is_object() || asset.text("name") != kPegasusAssetName)
                continue;
            const double size = asset.number("size");
            std::string hex;
            if (size >= 1.0 && size <= static_cast<double>(kPegasusDownloadCap) &&
                parse_sha256_digest(asset.text("digest"), &hex) &&
                asset.text("browser_download_url") == pegasus_download_url(r.tag))
            {
                r.size = static_cast<std::uint64_t>(size);
                r.sha256 = hex;
                r.downloadable = true;
            }
            break;
        }
        out->push_back(std::move(r));
        if (out->size() >= kMaxReleases)
            break;
    }
    return true;
}

const PegasusRelease *pick_latest_pegasus_release(const std::vector<PegasusRelease> &releases)
{
    const PegasusRelease *best = nullptr;
    for (const PegasusRelease &r : releases)
    {
        if (r.prerelease)
            continue;
        bool ok = false;
        if (best == nullptr || compare_pegasus_versions(r.tag, best->tag, &ok) > 0)
            best = &r;
    }
    return best;
}

bool looks_like_elf(const std::uint8_t *data, std::size_t size)
{
    return size >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
}

bool pegasus_check_due(std::int64_t checked_at, std::int64_t now, bool force)
{
    if (force || checked_at <= 0 || now < checked_at)
        return true;
    return now - checked_at >= kPegasusCheckTtlS;
}

PegasusAdvice pegasus_advice(bool running, std::string_view running_version, std::string_view latest_tag)
{
    if (!running)
        return PegasusAdvice::not_running;
    if (latest_tag.empty())
        return PegasusAdvice::not_checked;
    bool ok = false;
    const int order = compare_pegasus_versions(running_version, latest_tag, &ok);
    if (!ok)
        return PegasusAdvice::unreadable;
    if (order >= 0)
        return PegasusAdvice::up_to_date;
    return pegasus_version_fit(latest_tag) == PegasusVersionFit::tested ? PegasusAdvice::newer_tested
                                                                        : PegasusAdvice::newer_untested;
}

std::string pegasus_advice_text(PegasusAdvice advice)
{
    switch (advice)
    {
    case PegasusAdvice::not_running:
        return tr("Not running");
    case PegasusAdvice::not_checked:
        return tr("Latest release not checked yet");
    case PegasusAdvice::unreadable:
        return tr("Running; its version could not be compared");
    case PegasusAdvice::up_to_date:
        return tr("Up to date");
    case PegasusAdvice::newer_tested:
        return tr("Newer release available");
    case PegasusAdvice::newer_untested:
        return tr("Newer release available, untested with Wings");
    }
    return std::string();
}

std::string encode_pegasus_cache(std::int64_t checked_at, const std::vector<PegasusRelease> &releases)
{
    std::string out = "checked=" + std::to_string(checked_at) + "\n";
    for (const PegasusRelease &r : releases)
    {
        if (!r.downloadable && r.sha256.empty() && r.size == 0)
            out += "r=" + r.tag + "|" + r.published_at + "|0||" + (r.prerelease ? "1" : "0") + "\n";
        else
            out += "r=" + r.tag + "|" + r.published_at + "|" + std::to_string(r.size) + "|" + r.sha256 + "|" +
                   (r.prerelease ? "1" : "0") + "\n";
    }
    return out;
}

bool decode_pegasus_cache(std::string_view payload, std::int64_t *checked_at, std::vector<PegasusRelease> *out)
{
    out->clear();
    *checked_at = 0;
    bool have_time = false;
    std::size_t pos = 0;
    while (pos < payload.size())
    {
        std::size_t nl = payload.find('\n', pos);
        if (nl == std::string_view::npos)
            nl = payload.size();
        const std::string_view line = payload.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.substr(0, 8) == "checked=")
        {
            const std::string num(line.substr(8));
            if (num.empty() || num.size() > 12 || num.find_first_not_of("0123456789") != std::string::npos)
                return false;
            *checked_at = std::atoll(num.c_str());
            have_time = true;
        }
        else if (line.substr(0, 2) == "r=" && out->size() < kMaxReleases)
        {
            std::vector<std::string> f;
            std::size_t from = 2;
            for (;;)
            {
                const std::size_t bar = line.find('|', from);
                if (bar == std::string_view::npos)
                {
                    f.emplace_back(line.substr(from));
                    break;
                }
                f.emplace_back(line.substr(from, bar - from));
                from = bar + 1;
            }
            if (f.size() != 5 || !pegasus_tag_ok(f[0]))
                continue;
            PegasusRelease r;
            r.tag = f[0];
            r.published_at = clean_stamp(f[1]);
            r.prerelease = f[4] == "1";
            const bool numeric = !f[2].empty() && f[2].size() <= 10 &&
                                 f[2].find_first_not_of("0123456789") == std::string::npos;
            const std::uint64_t size = numeric ? std::strtoull(f[2].c_str(), nullptr, 10) : 0;
            std::string hex;
            if (size >= 1 && size <= kPegasusDownloadCap && parse_sha256_digest("sha256:" + f[3], &hex))
            {
                r.size = size;
                r.sha256 = hex;
                r.downloadable = true;
            }
            out->push_back(std::move(r));
        }
    }
    return have_time;
}

// ---- SHA-256 ----

namespace
{
const std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline std::uint32_t rotr(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}
} // namespace

Sha256::Sha256()
{
    static const std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
}

void Sha256::block(const std::uint8_t *p)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
               (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void *data, std::size_t size)
{
    const auto *p = static_cast<const std::uint8_t *>(data);
    total_ += size;
    while (size > 0)
    {
        if (fill_ == 0 && size >= 64)
        {
            block(p);
            p += 64;
            size -= 64;
            continue;
        }
        const std::size_t k = std::min(size, std::size_t(64) - fill_);
        std::memcpy(buf_ + fill_, p, k);
        fill_ += k;
        p += k;
        size -= k;
        if (fill_ == 64)
        {
            block(buf_);
            fill_ = 0;
        }
    }
}

std::string Sha256::finish_hex()
{
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    const std::uint8_t zero = 0;
    while (fill_ != 56)
        update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i)
        for (int s = 28; s >= 0; s -= 4)
            out.push_back(digits[(h_[i] >> s) & 0xf]);
    return out;
}

std::string sha256_hex(std::string_view data)
{
    Sha256 h;
    h.update(data.data(), data.size());
    return h.finish_hex();
}

// ---- TCP hand-off ----

bool send_file_to_loopback(const std::string &host, std::uint16_t port, const std::string &path,
                           const std::function<void(std::uint64_t)> &progress, std::string *error)
{
    const auto fail = [&](const std::string &why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (host != "127.0.0.1")
        return fail("only 127.0.0.1 is allowed");
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return fail("the downloaded file could not be opened");
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::fclose(file);
        return fail("socket failed");
    }
    timeval timeout{};
    timeout.tv_sec = 15;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    peer.sin_addr.s_addr = htonl(0x7f000001u);
    if (connect(fd, reinterpret_cast<const sockaddr *>(&peer), sizeof peer) != 0)
    {
        const bool refused = errno == ECONNREFUSED;
        close(fd);
        std::fclose(file);
        return fail(refused ? "connection refused" : "connect failed");
    }
    std::uint64_t sent = 0;
    char chunk[64 * 1024];
    bool ok = true;
    for (;;)
    {
        const std::size_t n = std::fread(chunk, 1, sizeof chunk, file);
        if (n == 0)
        {
            if (std::ferror(file))
            {
                ok = false;
                if (error != nullptr)
                    *error = "reading the file failed";
            }
            break;
        }
        std::size_t off = 0;
        while (off < n)
        {
#ifdef MSG_NOSIGNAL
            const ssize_t w = ::send(fd, chunk + off, n - off, MSG_NOSIGNAL);
#else
            const ssize_t w = ::send(fd, chunk + off, n - off, 0);
#endif
            if (w <= 0)
            {
                if (w < 0 && errno == EINTR)
                    continue;
                ok = false;
                if (error != nullptr)
                    *error = "the loader stopped accepting data";
                break;
            }
            off += static_cast<std::size_t>(w);
            sent += static_cast<std::uint64_t>(w);
        }
        if (!ok)
            break;
        if (progress)
            progress(sent);
    }
    shutdown(fd, SHUT_WR);
    close(fd);
    std::fclose(file);
    return ok;
}

// ---- the service ----

PegasusManager &pegasus_manager()
{
    static PegasusManager manager;
    return manager;
}

PegasusManager::~PegasusManager()
{
    {
        Lock l(mutex_);
        cancel_ = true;
    }
    thread_.join();
}

std::string PegasusManager::file_path(const std::string &tag) const
{
    return config_.root + "/" + tag + "/" + kPegasusAssetName;
}

bool PegasusManager::gate(std::string *why) const
{
    if (config_.downloader == nullptr)
    {
        *why = tr("Not available in this build");
        return false;
    }
    return config_.downloader->ready(why);
}

void PegasusManager::attach(PegasusManagerConfig config)
{
    thread_.join();
    Lock l(mutex_);
    config_ = std::move(config);
    if (!config_.now)
        config_.now = system_now;
    status_ = PegasusStatus();
    status_.attached = true;
    releases_.clear();
    for (const std::string &path : config_.cache_paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 64u << 10))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        std::int64_t at = 0;
        std::vector<PegasusRelease> list;
        if (d.ok && decode_pegasus_cache(d.payload, &at, &list))
        {
            releases_ = std::move(list);
            status_.checked_at = at;
            break;
        }
    }
    if (const PegasusRelease *latest = pick_latest_pegasus_release(releases_))
    {
        status_.have_latest = true;
        status_.latest = *latest;
    }
    refresh_downloaded_locked();
}

void PegasusManager::refresh_downloaded_locked()
{
    status_.downloaded = false;
    status_.path.clear();
    if (!status_.have_latest || !status_.latest.downloadable)
        return;
    const std::string path = file_path(status_.latest.tag);
    struct stat st;
    // Only a rename after a verified digest ever creates this name, so presence plus size is enough.
    if (stat(path.c_str(), &st) == 0 && static_cast<std::uint64_t>(st.st_size) == status_.latest.size)
    {
        status_.downloaded = true;
        status_.path = path;
    }
}

void PegasusManager::save_cache_locked()
{
    const std::string data =
        hui::save::encode(hui::save::Kind::stats, 1, encode_pegasus_cache(status_.checked_at, releases_));
    for (const std::string &path : config_.cache_paths)
        if (hui::save::write_atomic(path, data).empty())
            return;
}

PegasusStatus PegasusManager::status() const
{
    PegasusStatus out;
    {
        Lock l(mutex_);
        out = status_;
    }
    if (out.attached)
    {
        std::string why;
        out.net_ok = gate(&why);
        out.net_note = why;
    }
    return out;
}

bool PegasusManager::start_job(PegasusPhase phase, std::function<void()> body)
{
    {
        Lock l(mutex_);
        if (!status_.attached || status_.phase != PegasusPhase::idle)
            return false;
        status_.phase = phase;
        status_.message.clear();
        status_.message_is_error = false;
        status_.got_bytes = 0;
        status_.sent = false;
        status_.fail = PegasusFail::none;
        cancel_ = false;
    }
    thread_.join();
    if (!thread_.start(std::move(body), 512u << 10))
    {
        Lock l(mutex_);
        status_.phase = PegasusPhase::idle;
        status_.message = tr("Could not start the background job");
        status_.message_is_error = true;
        return false;
    }
    return true;
}

void PegasusManager::finish(const std::string &message, bool is_error)
{
    Lock l(mutex_);
    status_.phase = PegasusPhase::idle;
    status_.message = message;
    status_.message_is_error = is_error;
}

void PegasusManager::note_pegasus_online(bool online)
{
    Lock l(mutex_);
    online_now_ = online;
}

void PegasusManager::wait_idle()
{
    thread_.join();
}

void PegasusManager::cancel()
{
    Lock l(mutex_);
    cancel_ = true;
}

void PegasusManager::auto_check()
{
    std::string why;
    std::int64_t now = 0;
    {
        Lock l(mutex_);
        if (!status_.attached || status_.phase != PegasusPhase::idle)
            return;
        now = config_.now();
        if (!pegasus_check_due(status_.checked_at, now, false))
            return;
        // A failed attempt is not repeated every frame.
        if (attempt_at_ != 0 && now >= attempt_at_ && now - attempt_at_ < 600)
            return;
    }
    if (!gate(&why))
        return;
    {
        Lock l(mutex_);
        attempt_at_ = now;
    }
    start_job(PegasusPhase::checking, [this] { run_check(); });
}

bool PegasusManager::check_now()
{
    std::string why;
    if (!gate(&why))
    {
        Lock l(mutex_);
        status_.check_error = why;
        return false;
    }
    {
        Lock l(mutex_);
        attempt_at_ = config_.now();
    }
    return start_job(PegasusPhase::checking, [this] { run_check(); });
}

void PegasusManager::run_check()
{
    DocResult r;
    if (config_.docs != nullptr)
        r = config_.docs->get(kPegasusReleasesUrl, kPegasusReleasesCap);
    else
        r.error = "no network client";
    std::vector<PegasusRelease> list;
    std::string error;
    if (!r.error.empty())
        error = r.error;
    else if (r.status == 403 || r.status == 429)
        error = tr("GitHub is limiting requests from this address. Try again later.");
    else if (r.status != 200)
        error = tr("GitHub answered HTTP {status}", {{"status", std::to_string(r.status)}});
    else
    {
        std::string why;
        if (!parse_pegasus_releases(r.body, &list, &why))
            error = tr("The release list could not be read");
    }
    Lock l(mutex_);
    status_.phase = PegasusPhase::idle;
    status_.check_error = error;
    if (!error.empty())
        return;
    releases_ = std::move(list);
    status_.checked_at = config_.now();
    status_.have_latest = false;
    status_.latest = PegasusRelease();
    if (const PegasusRelease *latest = pick_latest_pegasus_release(releases_))
    {
        status_.have_latest = true;
        status_.latest = *latest;
    }
    refresh_downloaded_locked();
    save_cache_locked();
}

bool PegasusManager::download_latest()
{
    PegasusRelease release;
    {
        Lock l(mutex_);
        if (!status_.have_latest || !status_.latest.downloadable)
            return false;
        release = status_.latest;
    }
    std::string why;
    if (!gate(&why))
    {
        Lock l(mutex_);
        status_.message = why;
        status_.message_is_error = true;
        return false;
    }
    return start_job(PegasusPhase::downloading, [this, release] { run_download(release); });
}

void PegasusManager::run_download(PegasusRelease release)
{
    const std::string url = pegasus_download_url(release.tag);
    if (!pegasus_url_allowed(url, PegasusUrlRole::first, release.tag) || !release.downloadable)
    {
        finish(tr("That release has no download Wings can verify."), true);
        return;
    }
    PegasusDownloadHooks hooks;
    hooks.redirect_ok = [](const std::string &target)
    { return pegasus_url_allowed(target, PegasusUrlRole::redirect); };
    hooks.progress = [this](std::uint64_t bytes)
    {
        Lock l(mutex_);
        status_.got_bytes = std::max(status_.got_bytes, bytes);
    };
    hooks.cancel = [this]
    {
        Lock l(mutex_);
        return cancel_;
    };
    PegasusDownloadResult got = config_.downloader->get(url, kPegasusDownloadCap, hooks);
    {
        Lock l(mutex_);
        if (cancel_)
        {
            status_.phase = PegasusPhase::idle;
            status_.message = tr("Download cancelled");
            status_.message_is_error = true;
            return;
        }
        status_.phase = PegasusPhase::verifying;
    }
    if (!got.error.empty())
    {
        finish(tr("Download failed: {reason}", {{"reason", got.error}}), true);
        return;
    }
    if (got.status != 200)
    {
        finish(tr("Download failed: HTTP {status}", {{"status", std::to_string(got.status)}}), true);
        return;
    }
    if (got.body.size() != release.size)
    {
        finish(tr("The file is {got} bytes but GitHub lists {want}. Nothing was saved.",
                  {{"got", std::to_string(got.body.size())}, {"want", std::to_string(release.size)}}),
               true);
        return;
    }
    if (!looks_like_elf(got.body.data(), got.body.size()))
    {
        finish(tr("The download is not an ELF file. Nothing was saved."), true);
        return;
    }
    Sha256 hash;
    hash.update(got.body.data(), got.body.size());
    if (hash.finish_hex() != release.sha256)
    {
        finish(tr("The file did not match the checksum GitHub publishes for it. Nothing was saved."), true);
        return;
    }
    // Verified: only now does it touch the disk (a .part file first, then the final name).
    const std::string dir = config_.root + "/" + release.tag;
    const std::string final_path = file_path(release.tag);
    const std::string part = final_path + ".part";
    bool wrote = make_dirs(dir);
    if (wrote)
    {
        FILE *f = std::fopen(part.c_str(), "wb");
        wrote = f != nullptr;
        if (f != nullptr)
        {
            std::size_t off = 0;
            while (wrote && off < got.body.size())
            {
                const std::size_t k = std::min<std::size_t>(256u << 10, got.body.size() - off);
                wrote = std::fwrite(got.body.data() + off, 1, k, f) == k;
                off += k;
            }
            wrote = (std::fflush(f) == 0) && wrote;
            wrote = (std::fclose(f) == 0) && wrote;
        }
    }
    if (wrote)
    {
        std::remove(final_path.c_str());
        wrote = std::rename(part.c_str(), final_path.c_str()) == 0;
    }
    if (!wrote)
    {
        std::remove(part.c_str());
        finish(tr("The verified file could not be saved to {path}", {{"path", dir}}), true);
        return;
    }
    Lock l(mutex_);
    status_.phase = PegasusPhase::idle;
    status_.downloaded = true;
    status_.path = final_path;
    status_.message = tr("Downloaded and verified: {path}", {{"path", final_path}});
    status_.message_is_error = false;
}

std::vector<PegasusRelease> PegasusManager::releases() const
{
    Lock l(mutex_);
    return releases_;
}

bool PegasusManager::download_release(const std::string &tag)
{
    PegasusRelease release;
    bool found = false;
    {
        Lock l(mutex_);
        for (const PegasusRelease &r : releases_)
            if (r.tag == tag && r.downloadable)
            {
                release = r;
                found = true;
            }
    }
    std::string why;
    if (!found || !gate(&why))
        return false;
    return start_job(PegasusPhase::downloading, [this, release] { run_download(release); });
}

bool PegasusManager::verify_and_send(const std::string &tag, bool pegasus_online)
{
    PegasusRelease release;
    bool found = false;
    {
        Lock l(mutex_);
        for (const PegasusRelease &r : releases_)
            if (r.tag == tag && r.downloadable)
            {
                release = r;
                found = true;
            }
    }
    if (!found || pegasus_online)
        return false;
    return start_job(PegasusPhase::verifying, [this, release] { run_verify_send(release); });
}

// Re-checks the file on disk (SHA-256, size, ELF header) every time before it is sent. The file is
// read in chunks twice (hash, then send); it is never held in memory.
void PegasusManager::run_verify_send(PegasusRelease release)
{
    const std::string path = file_path(release.tag);
    std::string hex;
    std::uint64_t size = 0;
    bool elf = false;
    const bool read = sha256_file(path, &hex, &size, &elf);
    if (!read || !elf || size != release.size || hex != release.sha256)
    {
        std::remove(path.c_str()); // never keep a file that does not match; the flow downloads it again
        {
            Lock l(mutex_);
            status_.fail = PegasusFail::verify;
            status_.downloaded = false;
        }
        finish(read ? tr("The saved file did not match the published checksum, so it was not sent.")
                    : tr("The saved file could not be read."),
               true);
        return;
    }
    bool answered = false;
    {
        Lock l(mutex_);
        answered = online_now_; // the health re-check, immediately before the send
        if (!answered)
        {
            status_.phase = PegasusPhase::sending;
            status_.got_bytes = 0;
            status_.path = path;
        }
    }
    if (answered)
    {
        finish(tr("Pegasus DL answered, so nothing was sent."), false);
        return;
    }
    run_send(path);
}

bool PegasusManager::send_to_loader(bool pegasus_online)
{
    std::string path;
    {
        Lock l(mutex_);
        if (pegasus_online || !status_.downloaded || status_.path.empty())
            return false;
        path = status_.path;
    }
    return start_job(PegasusPhase::sending, [this, path] { run_send(path); });
}

void PegasusManager::run_send(std::string path)
{
    std::string error;
    const std::string where = config_.loader_host + ":" + std::to_string(config_.loader_port);
    const bool ok = send_file_to_loopback(
        config_.loader_host, config_.loader_port, path,
        [this](std::uint64_t bytes)
        {
            Lock l(mutex_);
            status_.got_bytes = bytes;
        },
        &error);
    if (!ok)
    {
        {
            Lock l(mutex_);
            status_.fail = error == "connection refused" ? PegasusFail::no_loader : PegasusFail::send;
        }
        finish(tr("Could not send to the payload loader at {where} ({reason}). Send the file with your usual "
                  "payload sender instead.",
                  {{"where", where}, {"reason", error}}),
               true);
        return;
    }
    Lock l(mutex_);
    status_.phase = PegasusPhase::idle;
    status_.sent = true;
    status_.message = tr("Sent to the payload loader at {where}. Wait for Wings to say Pegasus DL is running.",
                         {{"where", where}});
    status_.message_is_error = false;
}

} // namespace orbit
