// Wings cover-art module: the art service (workers, caches, backoff, DNS choice).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/art_service.hpp"

#include "wings/art/doh.hpp"

#include "fsutil.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace wings::art
{

const char *name(DnsMode m)
{
    switch (m)
    {
    case DnsMode::off: return "off";
    case DnsMode::system: return "system";
    case DnsMode::doh: return "doh";
    case DnsMode::custom: return "custom";
    }
    return "?";
}

namespace
{

const char *mime_of(ImageFormat f)
{
    switch (f)
    {
    case ImageFormat::webp: return "image/webp";
    case ImageFormat::png: return "image/png";
    case ImageFormat::jpeg: return "image/jpeg";
    default: return "application/octet-stream";
    }
}

std::string host_key(const std::string &url)
{
    tls::ParsedUrl u;
    std::string why;
    if (!tls::parse_https_url(url, &u, &why))
        return std::string();
    return u.host + ":" + std::to_string(u.port);
}

struct Failure
{
    bool permanent = false;
    bool host_fault = false; // counts towards the per-host breaker
    bool dns_fault = false;  // the resolver, not the host, is unreachable
    std::string reason;
};

Failure classify(const tls::Response &r)
{
    Failure f;
    f.reason = std::string(tls::name(r.error)) + (r.detail.empty() ? "" : ": " + r.detail);
    switch (r.error)
    {
    case tls::Error::bad_url:
    case tls::Error::blocked:
    case tls::Error::redirect:
    case tls::Error::too_large:
    case tls::Error::bad_type:
        f.permanent = true;
        break;
    case tls::Error::http_status:
        if (r.status == 408 || r.status == 429 || r.status >= 500 || r.status < 400)
        {
            f.host_fault = r.status >= 500 || r.status == 429;
        }
        else
            f.permanent = true; // 404, 410, 403, 401 ...
        break;
    case tls::Error::handshake:
        // A bad certificate is permanent; "expired / not yet valid" is far more
        // likely the console clock than the server, so it is only retried.
        f.permanent = r.verify_error != 0 && !r.time_error;
        f.host_fault = !f.permanent;
        break;
    case tls::Error::dns:
        f.dns_fault = !r.nxdomain;
        f.host_fault = r.nxdomain;
        break;
    default: // connect, io, protocol
        f.host_fault = true;
        break;
    }
    return f;
}

} // namespace

// =========================================================================
struct ArtService::Impl
{
    ArtConfig cfg;
    const Clock *clock;
    DiskCache *disk = nullptr;

    mutable Mutex mu;
    Cond cv;
    std::atomic<bool> stopping{false};
    bool started = false;
    bool disk_ok = false;
    Phase fatal = Phase::stopped; // ca_error / entropy_error after start()
    std::string fatal_detail;
    std::string ca_source;

    std::shared_ptr<tls::Shared> shared;
    std::shared_ptr<DnsProvider> dns_active;
    DnsMode mode = DnsMode::off;
    bool mode_available = false;
    std::string mode_detail;
    std::vector<std::string> doh_endpoints;

    struct Job
    {
        std::string url;
        Kind kind = Kind::cover;
        Lane lane = Lane::visible;
        std::uint64_t seq = 0;
        bool running = false;
        bool refresh = false;
    };
    std::map<std::string, Job> jobs;
    std::uint64_t seq = 0;
    int running = 0, running_idle = 0;

    struct FailRec
    {
        int attempts = 0;
        std::int64_t next_ms = 0;
        std::string error;
        int status = 0;
    };
    std::map<std::string, FailRec> fails;
    struct HostRec
    {
        int consecutive = 0;
        std::int64_t open_until_ms = 0;
    };
    std::map<std::string, HostRec> hosts;
    std::int64_t offline_until_ms = 0;
    int dns_streak = 0;
    std::set<std::string> time_error_hosts;

    struct Handoff
    {
        std::string bytes;
        std::uint64_t seq = 0;
    };
    std::map<std::string, Handoff> handoff; // only when the disk cache cannot hold a cover
    std::size_t handoff_bytes = 0;

    bool heap_paused = false;
    ArtStats stats;
    std::vector<std::unique_ptr<Thread>> threads;

    explicit Impl(ArtConfig c) : cfg(std::move(c)), clock(cfg.clock ? cfg.clock : &system_clock())
    {
        cfg.workers = std::max(1, std::min(cfg.workers, 6));
        cfg.idle_fill_workers = std::max(0, std::min(cfg.idle_fill_workers, cfg.workers));
        if (cfg.retry_schedule_s.empty())
            cfg.retry_schedule_s = {2, 8, 30, 120, 600};
#ifndef WINGS_ART_TESTING
        cfg.tls_limits.allow_private = false; // loopback fixtures exist for tests only
#endif
        mode = cfg.dns_mode;
        doh_endpoints = cfg.doh_endpoints;
    }

    void log(int level, const std::string &line) const
    {
        if (cfg.log)
            cfg.log(level, "[WINGS-ART] " + line);
    }

    std::int64_t now_ms() const { return clock->mono_ms(); }

    // ---------------------------------------------------------- DNS wiring
    void apply_dns_mode()
    {
        std::shared_ptr<DnsProvider> inner;
        std::string detail;
        DnsMode m;
        std::vector<std::string> endpoints;
        {
            Lock l(mu);
            m = mode;
            endpoints = doh_endpoints;
        }
        switch (m)
        {
        case DnsMode::off:
            detail = "Cover art is off. Turn on DNS-over-HTTPS in Settings to download covers "
                     "(the host names of the art servers are then sent to 1.1.1.1 or 8.8.8.8). "
                     "Covers that are already downloaded still show.";
            break;
        case DnsMode::system:
#ifdef WINGS_ART_SCENET
        {
            auto sys = std::make_shared<SystemDns>();
            if (sys->ready())
                inner = sys;
            else
                detail = "The console name resolver could not be started. Use DNS-over-HTTPS in Settings.";
        }
#else
            detail = "The console name resolver is not available in this build. Use DNS-over-HTTPS in Settings.";
#endif
        break;
        case DnsMode::doh:
        {
            tls::Limits l = DohProvider::default_limits();
            l.allow_private = cfg.tls_limits.allow_private;
            if (shared)
            {
                auto doh = std::make_shared<DohProvider>(shared, endpoints, l);
                if (doh->rejected_endpoints().size() == endpoints.size())
                    detail = "No valid DNS-over-HTTPS server is configured (use an https:// address with an IP number).";
                else
                    inner = doh;
            }
            break;
        }
        case DnsMode::custom:
            if (cfg.dns_override)
                inner = cfg.dns_override;
            else
                detail = "No custom resolver was supplied.";
            break;
        }
        std::shared_ptr<DnsProvider> active;
        if (inner)
            active = std::make_shared<CachingDns>(inner, clock);
        {
            Lock l(mu);
            dns_active = active;
            mode_available = active != nullptr;
            mode_detail = detail;
            hosts.clear();
            fails.clear();
            offline_until_ms = 0;
            dns_streak = 0;
        }
        if (shared)
            shared->set_dns(active);
        log(2, std::string("DNS mode ") + name(m) + (active ? " active" : " unavailable"));
    }

    // ---------------------------------------------------------- state
    Phase phase_locked(std::string *detail) const
    {
        if (!started || stopping.load())
        {
            *detail = "Cover art is stopped.";
            return Phase::stopped;
        }
        if (fatal != Phase::stopped)
        {
            *detail = fatal_detail;
            return fatal;
        }
        if (!mode_available)
        {
            *detail = mode_detail;
            return Phase::disabled_dns;
        }
        if (now_ms() < offline_until_ms)
        {
            *detail = "Cannot reach the name server. Cover art will retry shortly.";
            return Phase::offline;
        }
        if (heap_paused)
        {
            *detail = "Cover art is paused while the app is short of memory.";
            return Phase::paused_heap;
        }
        *detail = "Cover art is on.";
        return Phase::ready;
    }

    bool update_heap_pause()
    {
        if (!cfg.heap_used)
            return false;
        const std::uint64_t used = cfg.heap_used(); // app callback, called without our lock
        Lock l(mu);
        if (!heap_paused && used > cfg.heap_pause_above)
        {
            heap_paused = true;
            ++stats.heap_pauses;
        }
        else if (heap_paused && used < cfg.heap_resume_below)
            heap_paused = false;
        return heap_paused;
    }

    // ---------------------------------------------------------- queue
    // Mutex held.
    void enqueue_locked(const std::string &url, Kind kind, Lane lane, bool refresh)
    {
        auto it = jobs.find(url);
        if (it != jobs.end())
        {
            Job &j = it->second;
            ++stats.deduped;
            if (j.running)
                return;
            if (static_cast<int>(lane) < static_cast<int>(j.lane))
                j.lane = lane;
            if (kind == Kind::hero)
                j.kind = Kind::hero;
            if (lane <= Lane::near)
                j.seq = ++seq; // asked for again: it is on screen right now
            j.refresh = j.refresh && refresh;
            cv.notify_all();
            return;
        }
        const bool foreground = lane <= Lane::near;
        std::size_t fg = 0, bg = 0;
        for (const auto &kv : jobs)
            if (!kv.second.running)
                (kv.second.lane <= Lane::near ? fg : bg)++;
        if (foreground)
        {
            while (fg >= cfg.max_queued_visible)
            {
                // The oldest foreground request was scrolled past long ago.
                auto oldest = jobs.end();
                for (auto j = jobs.begin(); j != jobs.end(); ++j)
                    if (!j->second.running && j->second.lane <= Lane::near &&
                        (oldest == jobs.end() || j->second.seq < oldest->second.seq))
                        oldest = j;
                if (oldest == jobs.end())
                    break;
                jobs.erase(oldest);
                --fg;
            }
        }
        else if (bg >= cfg.max_queued_prefetch)
            return; // prefetch is best effort
        Job j;
        j.url = url;
        j.kind = kind;
        j.lane = lane;
        j.seq = ++seq;
        j.refresh = refresh;
        jobs[url] = std::move(j);
        cv.notify_all();
    }

    // Mutex held. Picks the next runnable job; false when none.
    bool pick_locked(Job *out)
    {
        bool foreground_waiting = false;
        for (const auto &kv : jobs)
            if (!kv.second.running && kv.second.lane <= Lane::near)
                foreground_waiting = true;
        const Job *best = nullptr;
        for (const auto &kv : jobs)
        {
            const Job &j = kv.second;
            if (j.running)
                continue;
            if (j.lane == Lane::idle_fill && (foreground_waiting || running_idle >= cfg.idle_fill_workers))
                continue;
            if (!best)
            {
                best = &j;
                continue;
            }
            if (j.lane != best->lane)
            {
                if (static_cast<int>(j.lane) < static_cast<int>(best->lane))
                    best = &j;
                continue;
            }
            const bool newest_first = j.lane <= Lane::near;
            if (newest_first ? j.seq > best->seq : j.seq < best->seq)
                best = &j;
        }
        if (!best)
            return false;
        Job &mine = jobs[best->url];
        mine.running = true;
        ++running;
        if (mine.lane == Lane::idle_fill)
            ++running_idle;
        *out = mine;
        return true;
    }

    bool next_job(Job *out)
    {
        for (;;)
        {
            if (stopping.load())
                return false;
            const bool paused = update_heap_pause();
            Lock l(mu);
            if (stopping.load())
                return false;
            if (!paused && pick_locked(out))
                return true;
            cv.wait(mu, paused ? 250 : 500);
        }
    }

    void finish(const Job &job)
    {
        Lock l(mu);
        jobs.erase(job.url);
        --running;
        if (job.lane == Lane::idle_fill)
            --running_idle;
        cv.notify_all();
    }

    // ---------------------------------------------------------- failure bookkeeping
    // Mutex held.
    bool host_blocked_locked(const std::string &host, std::int64_t *until) const
    {
        auto it = hosts.find(host);
        if (it == hosts.end() || now_ms() >= it->second.open_until_ms)
            return false;
        *until = it->second.open_until_ms;
        return true;
    }

    void note_success(const std::string &host, const std::string &url)
    {
        Lock l(mu);
        auto h = hosts.find(host);
        if (h != hosts.end())
            h->second.consecutive = 0;
        dns_streak = 0;
        fails.erase(url);
    }

    void note_transient(const std::string &url, const std::string &host, const Failure &f,
                        int status, int retry_after_s)
    {
        Lock l(mu);
        ++stats.fail_transient;
        if (fails.size() > 4096)
            fails.clear();
        FailRec &fr = fails[url];
        ++fr.attempts;
        const std::size_t idx = static_cast<std::size_t>(fr.attempts - 1);
        int delay = idx < cfg.retry_schedule_s.size() ? cfg.retry_schedule_s[idx] : cfg.park_s;
        if (retry_after_s > 0)
            delay = std::max(delay, std::min(retry_after_s, cfg.park_s));
        fr.next_ms = now_ms() + static_cast<std::int64_t>(delay) * 1000;
        fr.error = f.reason;
        fr.status = status;
        if (f.host_fault && !host.empty())
        {
            if (hosts.size() > 256)
                hosts.clear();
            HostRec &h = hosts[host];
            if (++h.consecutive >= cfg.host_fail_threshold && now_ms() >= h.open_until_ms)
            {
                h.open_until_ms = now_ms() + static_cast<std::int64_t>(cfg.host_backoff_s) * 1000;
                h.consecutive = cfg.host_fail_threshold - 1; // half-open: one more failure re-opens
                ++stats.host_pauses;
                log(1, "host " + host + " paused for " + std::to_string(cfg.host_backoff_s) + " s");
            }
        }
        if (f.dns_fault && ++dns_streak >= 2)
        {
            offline_until_ms = now_ms() + static_cast<std::int64_t>(cfg.offline_probe_s) * 1000;
            dns_streak = 1; // after the probe period a single failure trips it again
            log(1, "name server unreachable; probing again in " + std::to_string(cfg.offline_probe_s) + " s");
        }
    }

    void note_permanent(const std::string &url, const std::string &reason)
    {
        disk->put_negative(url, reason, cfg.negative_ttl_s);
        Lock l(mu);
        ++stats.fail_permanent;
        fails.erase(url);
    }

    void keep_in_memory(const std::string &url, std::string bytes)
    {
        Lock l(mu);
        while (!handoff.empty() && handoff_bytes + bytes.size() > (8u << 20))
        {
            auto oldest = handoff.begin();
            for (auto it = handoff.begin(); it != handoff.end(); ++it)
                if (it->second.seq < oldest->second.seq)
                    oldest = it;
            handoff_bytes -= oldest->second.bytes.size();
            handoff.erase(oldest);
        }
        auto old = handoff.find(url);
        if (old != handoff.end())
            handoff_bytes -= old->second.bytes.size();
        handoff_bytes += bytes.size();
        Handoff h;
        h.bytes = std::move(bytes);
        h.seq = ++seq;
        handoff[url] = std::move(h);
    }

    // ---------------------------------------------------------- worker
    static bool is_stopping(void *self) { return static_cast<Impl *>(self)->stopping.load(); }

    void run_job(tls::Client &client, const Job &job)
    {
        const std::string host = host_key(job.url);
        {
            Lock l(mu);
            std::int64_t until = 0;
            if (host_blocked_locked(host, &until) || now_ms() < offline_until_ms)
                return; // request() reports it; nothing to do now
        }
        if (!job.refresh && disk->contains(job.url))
            return; // someone else fetched it meanwhile
        tls::GetOptions go;
        go.max_body = job.kind == Kind::hero ? cfg.hero_max_bytes : cfg.cover_max_bytes;
        go.cancelled = &Impl::is_stopping;
        go.cancel_ctx = this;
        tls::Response r = client.get(job.url, go);
        if (r.error == tls::Error::cancelled || stopping.load())
            return;

        if (!r.ok())
        {
            Failure f = classify(r);
            if (r.time_error)
            {
                Lock l(mu);
                time_error_hosts.insert(host);
            }
            if (r.status != 0 && !f.permanent && !f.host_fault)
            {
                // The server answered (e.g. 408): the host is alive.
                Lock l(mu);
                auto h = hosts.find(host);
                if (h != hosts.end())
                    h->second.consecutive = 0;
            }
            if (f.permanent)
            {
                if (r.status != 0)
                    note_success(host, job.url); // an answer, even 404, proves the host is up
                note_permanent(job.url, f.reason);
                log(2, "permanent failure " + job.url + " -> " + f.reason);
            }
            else
            {
                note_transient(job.url, host, f, r.status, r.retry_after_s);
                log(2, "transient failure " + job.url + " -> " + f.reason);
            }
            return;
        }

        note_success(host, job.url);
        ImageInfo info;
        const Verdict v = check_image(r.body.data(), r.body.size(), cfg.guard, &info);
        {
            Lock l(mu);
            ++stats.net_ok;
            stats.net_bytes += r.body.size();
            stats.connections_reused += r.timing.reused ? 1 : 0;
            stats.tls_resumed += r.timing.resumed ? 1 : 0;
        }
        if (v != Verdict::ok)
        {
            {
                Lock l(mu);
                ++stats.refused_by_guard;
            }
            note_permanent(job.url, std::string("image refused: ") + name(v) + " (" + std::to_string(info.width) +
                                         "x" + std::to_string(info.height) + ")");
            log(1, "image refused " + job.url + ": " + name(v));
            return;
        }
        if (!disk_ok || !disk->put(job.url, r.body.data(), r.body.size()))
            keep_in_memory(job.url, std::string(reinterpret_cast<const char *>(r.body.data()), r.body.size()));
    }

    void worker_main()
    {
        tls::Limits l = cfg.tls_limits;
        tls::Client client(shared, l);
        Job job;
        while (next_job(&job))
        {
            run_job(client, job);
            finish(job);
        }
        client.close_all();
    }
};

// =========================================================================

ArtService::ArtService(ArtConfig config) : impl_(new Impl(std::move(config)))
{
    DiskCacheConfig dc;
    dc.root = impl_->cfg.cache_root;
    dc.budget_bytes = impl_->cfg.cache_budget_bytes;
    dc.clock = impl_->cfg.clock;
    disk_.reset(new DiskCache(dc));
    impl_->disk = disk_.get();
}

ArtService::~ArtService()
{
    stop();
}

bool ArtService::start(std::string *problem)
{
    Impl &s = *impl_;
    std::string dummy;
    std::string &prob = problem ? *problem : dummy;
    prob.clear();
    {
        Lock l(s.mu);
        if (s.started)
            return true;
    }
    std::string err;
    s.disk_ok = disk_->open(&err);
    if (!s.disk_ok)
    {
        prob = err;
        s.log(1, "disk cache unavailable: " + err + " (covers are kept in memory only)");
    }

    // Trust store: first usable bundle wins; never "no verification".
    std::string info;
    std::vector<std::string> tried;
    for (const std::string &path : s.cfg.ca_bundle_paths)
    {
        std::string pem;
        if (!fs::read_file(path, &pem, 4u << 20))
        {
            tried.push_back(path + " (unreadable)");
            continue;
        }
        s.shared = tls::Shared::create(pem, &info);
        if (s.shared)
        {
            s.ca_source = path;
            break;
        }
        tried.push_back(path + " (" + info + ")");
        if (info.find("entropy") != std::string::npos)
            break;
    }
    if (!s.shared && !s.cfg.ca_bundle_pem.empty() && info.find("entropy") == std::string::npos)
    {
        s.shared = tls::Shared::create(s.cfg.ca_bundle_pem, &info);
        if (s.shared)
            s.ca_source = "(built in)";
        else
            tried.push_back("(built in) (" + info + ")");
    }
    if (s.shared)
        s.log(2, "trust store " + s.ca_source + ": " + info);
    else
    {
        s.fatal = info.find("entropy") != std::string::npos ? Phase::entropy_error : Phase::ca_error;
        std::string list;
        for (const std::string &t : tried)
            list += (list.empty() ? "" : "; ") + t;
        s.fatal_detail = s.fatal == Phase::entropy_error
                             ? "Cover art is off: this system has no random number source for secure connections."
                             : "Cover art is off: no usable certificate bundle was found, and the app will not "
                               "download pictures without checking certificates.";
        prob = s.fatal_detail + (list.empty() ? "" : " Tried: " + list);
        s.log(0, prob);
    }

    s.apply_dns_mode();
    {
        Lock l(s.mu);
        s.started = true;
    }
    if (s.fatal == Phase::stopped)
    {
        for (int i = 0; i < s.cfg.workers; ++i)
        {
            std::unique_ptr<Thread> t(new Thread);
            t->start([&s] { s.worker_main(); }, 1u << 20);
            s.threads.push_back(std::move(t));
        }
    }
    return s.fatal == Phase::stopped;
}

void ArtService::stop()
{
    Impl &s = *impl_;
    s.stopping.store(true);
    {
        Lock l(s.mu);
        s.cv.notify_all();
    }
    for (auto &t : s.threads)
        t->join();
    s.threads.clear();
    if (disk_)
        disk_->flush();
}

ArtResult ArtService::request(const std::string &url, Kind kind, Lane lane)
{
    Impl &s = *impl_;
    ArtResult r;
    {
        Lock l(s.mu);
        ++s.stats.requests;
    }
    tls::ParsedUrl pu;
    std::string why;
    if (!tls::parse_https_url(url, &pu, &why))
    {
        r.state = Fetch::failed;
        r.permanent = true;
        r.error = "bad url: " + why;
        return r;
    }
    const std::string host = pu.host + ":" + std::to_string(pu.port);

    bool paused;
    {
        Lock l(s.mu);
        paused = s.heap_paused;
        // 1. memory handoff (only used when the disk cache is unavailable)
        auto h = s.handoff.find(url);
        if (h != s.handoff.end())
        {
            h->second.seq = ++s.seq;
            r.bytes = h->second.bytes;
            ++s.stats.handoff_hits;
        }
    }
    auto fill_ready = [&](std::string bytes) {
        const ImageFormat f = sniff_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
        r.state = Fetch::ready;
        r.content_type = mime_of(f);
        r.bytes = std::move(bytes);
    };
    if (!r.bytes.empty())
    {
        fill_ready(std::move(r.bytes));
        return r;
    }

    // 2. disk cache. While the heap is short, only on-screen requests read.
    if (s.disk_ok && (!paused || lane == Lane::visible))
    {
        std::string bytes;
        std::int64_t fetched = 0;
        if (disk_->get(url, &bytes, &fetched) == CacheGet::hit)
        {
            const bool stale = s.clock->unix_seconds() - fetched > s.cfg.stale_after_s;
            {
                Lock l(s.mu);
                ++s.stats.disk_hits;
                if (stale)
                {
                    ++s.stats.stale_served;
                    if (s.mode_available && s.fatal == Phase::stopped)
                        s.enqueue_locked(url, kind, Lane::idle_fill, true);
                }
            }
            fill_ready(std::move(bytes));
            return r;
        }
    }

    // 3. is the network allowed at all?
    std::string detail;
    Phase phase;
    {
        Lock l(s.mu);
        phase = s.phase_locked(&detail);
    }
    if (phase == Phase::stopped || phase == Phase::ca_error || phase == Phase::entropy_error ||
        phase == Phase::disabled_dns)
    {
        r.state = Fetch::disabled;
        r.error = detail;
        return r;
    }

    // 4. negative cache (permanent failures, 24 h)
    std::string reason;
    if (disk_->get_negative(url, &reason))
    {
        {
            Lock l(s.mu);
            ++s.stats.negative_hits;
        }
        r.state = Fetch::failed;
        r.permanent = true;
        r.error = reason;
        return r;
    }

    // 5. retry schedule, host breaker, offline state
    {
        Lock l(s.mu);
        const std::int64_t now = s.now_ms();
        auto f = s.fails.find(url);
        if (f != s.fails.end() && now < f->second.next_ms)
        {
            ++s.stats.backoff_blocked;
            r.state = Fetch::failed;
            r.error = f->second.error;
            r.http_status = f->second.status;
            r.retry_after_s = static_cast<int>((f->second.next_ms - now) / 1000) + 1;
            return r;
        }
        std::int64_t until = 0;
        if (s.host_blocked_locked(host, &until))
        {
            ++s.stats.backoff_blocked;
            r.state = Fetch::failed;
            r.error = "host " + host + " is paused after repeated failures";
            r.retry_after_s = static_cast<int>((until - now) / 1000) + 1;
            return r;
        }
        if (now < s.offline_until_ms)
        {
            ++s.stats.backoff_blocked;
            r.state = Fetch::failed;
            r.error = "name server unreachable";
            r.retry_after_s = static_cast<int>((s.offline_until_ms - now) / 1000) + 1;
            return r;
        }
        s.enqueue_locked(url, kind, lane, false);
    }
    r.state = Fetch::pending;
    return r;
}

std::size_t ArtService::prefetch(const std::vector<std::string> &urls, Kind kind, Lane lane)
{
    Impl &s = *impl_;
    if (lane < Lane::near)
        lane = Lane::near;
    std::string detail;
    {
        Lock l(s.mu);
        const Phase p = s.phase_locked(&detail);
        if (p == Phase::stopped || p == Phase::ca_error || p == Phase::entropy_error || p == Phase::disabled_dns)
            return 0;
    }
    std::size_t queued = 0;
    for (const std::string &url : urls)
    {
        const std::string host = host_key(url);
        if (host.empty() || disk_->contains(url) || disk_->get_negative(url, nullptr))
            continue;
        Lock l(s.mu);
        auto f = s.fails.find(url);
        std::int64_t until = 0;
        if ((f != s.fails.end() && s.now_ms() < f->second.next_ms) || s.host_blocked_locked(host, &until) ||
            s.now_ms() < s.offline_until_ms || s.handoff.count(url))
            continue;
        const std::size_t before = s.jobs.size();
        s.enqueue_locked(url, kind, lane, false);
        if (s.jobs.size() > before)
            ++queued;
    }
    return queued;
}

void ArtService::cancel_background()
{
    Impl &s = *impl_;
    Lock l(s.mu);
    for (auto it = s.jobs.begin(); it != s.jobs.end();)
    {
        if (!it->second.running && it->second.lane >= Lane::prefetch)
            it = s.jobs.erase(it);
        else
            ++it;
    }
}

ArtResult ArtService::fetch_blocking(const std::string &url, Kind kind, int timeout_ms)
{
    // Bounded by real time, so a fake test clock cannot make this spin forever.
    const std::int64_t deadline = system_clock().mono_ms() + timeout_ms;
    for (;;)
    {
        ArtResult r = request(url, kind, Lane::visible);
        if (r.state != Fetch::pending)
            return r;
        if (system_clock().mono_ms() > deadline)
        {
            r.state = Fetch::failed;
            r.error = "timeout waiting for the fetch";
            return r;
        }
        sleep_ms(5);
    }
}

void ArtService::set_dns_mode(DnsMode mode)
{
    {
        Lock l(impl_->mu);
        impl_->mode = mode;
    }
    impl_->apply_dns_mode();
    Lock l(impl_->mu);
    impl_->cv.notify_all();
}

void ArtService::set_doh_endpoints(std::vector<std::string> endpoints)
{
    bool doh;
    {
        Lock l(impl_->mu);
        impl_->doh_endpoints = std::move(endpoints);
        doh = impl_->mode == DnsMode::doh;
    }
    if (doh)
        impl_->apply_dns_mode();
}

DnsMode ArtService::dns_mode() const
{
    Lock l(impl_->mu);
    return impl_->mode;
}

ArtService::Document ArtService::fetch_document(const std::string &url, std::size_t max_bytes)
{
    Document doc;
    const ServiceState st = state();
    if (st.phase != Phase::ready)
    {
        doc.error = st.detail.empty() ? "the network is not ready" : st.detail;
        return doc;
    }
    tls::Limits limits = impl_->cfg.tls_limits;
    limits.max_body = max_bytes;
    tls::Client client(impl_->shared, limits);
    tls::GetOptions options;
    options.accept = "application/json,*/*;q=0.1";
    options.any_content_type = true;
    options.max_body = max_bytes;
    const tls::Response r = client.get(url, options);
    doc.status = r.status;
    doc.time_error = r.time_error;
    if (!r.ok() && r.error != tls::Error::http_status)
        doc.error = std::string(tls::name(r.error)) + (r.detail.empty() ? "" : " (" + r.detail + ")");
    else if (r.ok())
        doc.body.assign(reinterpret_cast<const char *>(r.body.data()), r.body.size());
    return doc;
}

namespace
{
struct DownloadCtx
{
    const ArtService::DownloadHooks *hooks;
};
bool download_redirect_ok(void *ctx, const std::string &url)
{
    const auto &h = static_cast<DownloadCtx *>(ctx)->hooks->redirect_ok;
    return h ? h(url) : true;
}
void download_progress(void *ctx, std::uint64_t bytes)
{
    const auto &h = static_cast<DownloadCtx *>(ctx)->hooks->progress;
    if (h)
        h(bytes);
}
bool download_cancelled(void *ctx)
{
    const auto &h = static_cast<DownloadCtx *>(ctx)->hooks->cancel;
    return h && h();
}
} // namespace

ArtService::Download ArtService::fetch_download(const std::string &url, std::size_t max_bytes,
                                                const DownloadHooks &hooks)
{
    Download out;
    const ServiceState st = state();
    if (st.phase != Phase::ready)
    {
        out.error = st.detail.empty() ? "the network is not ready" : st.detail;
        return out;
    }
    tls::Limits limits = impl_->cfg.tls_limits;
    limits.max_body = max_bytes;
    limits.total_timeout_ms = std::max(30, hooks.total_timeout_s) * 1000;
    tls::Client client(impl_->shared, limits);
    DownloadCtx ctx{&hooks};
    tls::GetOptions options;
    options.accept = "application/octet-stream,*/*;q=0.1";
    options.any_content_type = true;
    options.max_body = max_bytes;
    options.cancelled = &download_cancelled;
    options.cancel_ctx = &ctx;
    options.redirect_ok = &download_redirect_ok;
    options.progress = &download_progress;
    options.hook_ctx = &ctx;
    tls::Response r = client.get(url, options);
    out.status = r.status;
    out.time_error = r.time_error;
    if (!r.ok() && r.error != tls::Error::http_status)
        out.error = std::string(tls::name(r.error)) + (r.detail.empty() ? "" : " (" + r.detail + ")");
    else if (r.ok())
        out.body = std::move(r.body);
    return out;
}

ServiceState ArtService::state() const
{
    const Impl &s = *impl_;
    ServiceState st;
    Lock l(s.mu);
    st.phase = s.phase_locked(&st.detail);
    st.dns_mode = s.mode;
    st.system_dns_compiled = system_dns_compiled();
    st.ca_certs = s.shared ? s.shared->ca_count() : 0;
    st.ca_source = s.ca_source;
    st.disk_ok = s.disk_ok;
    st.heap_paused = s.heap_paused;
    st.clock_suspect = s.time_error_hosts.size() >= 2;
    if (st.clock_suspect && st.phase == Phase::ready)
        st.detail = "Cover art is on, but secure connections fail with 'certificate expired / not yet valid'. "
                    "Check the console's date and time.";
    int queued = 0;
    for (const auto &kv : s.jobs)
        if (!kv.second.running)
            ++queued;
    st.queued = queued;
    st.running = s.running;
    return st;
}

ArtStats ArtService::stats() const
{
    ArtStats out;
    {
        Lock l(impl_->mu);
        out = impl_->stats;
    }
    out.disk = disk_->stats();
    return out;
}

bool ArtService::idle() const
{
    Lock l(impl_->mu);
    return impl_->jobs.empty();
}

void ArtService::clear_cache()
{
    disk_->clear();
    Lock l(impl_->mu);
    impl_->handoff.clear();
    impl_->handoff_bytes = 0;
    impl_->fails.clear();
    impl_->hosts.clear();
    impl_->offline_until_ms = 0;
    impl_->dns_streak = 0;
}

void ArtService::retry_failed()
{
    disk_->clear_negatives();
    Lock l(impl_->mu);
    impl_->fails.clear();
    impl_->hosts.clear();
    impl_->offline_until_ms = 0;
    impl_->dns_streak = 0;
    impl_->time_error_hosts.clear();
}

} // namespace wings::art
