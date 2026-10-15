// Wings cover-art module: the art service (workers, caches, backoff, DNS choice).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ArtService turns "give me the picture behind this https URL" into bytes:
//
//   request() -> memory handoff -> disk cache -> negative cache -> retry/host
//   backoff -> queue (4 workers, lanes visible > near > prefetch > idle fill)
//   -> TLS client -> magic-byte sniff + size guard -> disk cache.
//
// request() never blocks on the network. It answers `ready` with the encoded
// bytes (WebP/PNG/JPEG, original compressed form), `pending` while a worker
// fetches (ask again shortly; asking again also keeps the job at the front of
// its lane), or `failed` with a transient/permanent class. Decoding is left to
// the caller (the app's ArtCache), which keeps the single decode gate.
//
// DNS: the module never calls getaddrinfo. DnsMode::off is the default: no
// network at all (disk-cached covers are still served) until the app enables
// System DNS (when verified on a console) or DNS-over-HTTPS (opt-in, the host
// names of the art CDNs are sent to the DoH operator).
#pragma once

#include "wings/art/disk_cache.hpp"
#include "wings/art/dns.hpp"
#include "wings/art/image_probe.hpp"
#include "wings/art/platform.hpp"
#include "wings/art/tls_client.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wings::art
{

enum class DnsMode : std::uint8_t
{
    off,    // default: no network. State reports "disabled until the user enables DoH".
    system, // console resolver (needs -DWINGS_ART_SCENET, unverified on hardware)
    doh,    // DNS-over-HTTPS to ArtConfig::doh_endpoints (opt-in)
    custom, // ArtConfig::dns_override (tests, or an app-supplied resolver)
};
const char *name(DnsMode m);

enum class Kind : std::uint8_t
{
    cover,
    hero,
};

// Lower number is served first. visible and near are newest-request-first (what
// is on screen now), prefetch and idle fill are first-in first-out.
enum class Lane : std::uint8_t
{
    visible = 0,
    near = 1,
    prefetch = 2,
    idle_fill = 3,
};

struct ArtConfig
{
    // --- storage
    std::string cache_root;                     // e.g. "/download0/wings/art"
    std::uint64_t cache_budget_bytes = 96ull << 20;
    int stale_after_s = 30 * 24 * 3600;         // older entries are refreshed in the background

    // --- trust. Tried in order; the first file holding at least one certificate wins.
    //     Typical: {"/download0/wings/ca-bundle.pem" (user override), "/app0/assets/ca-bundle.crt"}.
    std::vector<std::string> ca_bundle_paths;
    std::string ca_bundle_pem;                  // in-memory fallback (tests)

    // --- DNS
    DnsMode dns_mode = DnsMode::off;
    std::vector<std::string> doh_endpoints = {"https://1.1.1.1/dns-query", "https://8.8.8.8/dns-query"};
    std::shared_ptr<DnsProvider> dns_override;  // for DnsMode::custom

    // --- concurrency (clamped to 1..6 / 0..workers)
    int workers = 4;
    int idle_fill_workers = 2;                  // at most this many workers run idle-fill jobs
    std::size_t max_queued_visible = 96;        // visible+near jobs; the oldest is dropped
    std::size_t max_queued_prefetch = 512;      // prefetch+idle jobs; new ones are ignored when full

    // --- limits
    std::size_t cover_max_bytes = 2u << 20;
    std::size_t hero_max_bytes = 6u << 20;
    ImageGuard guard;                           // 4 Mpx / 4096 px before anything is cached

    // --- heap awareness: when heap_used() exceeds pause_above no NEW fetch starts;
    //     work resumes under resume_below. In-flight fetches finish.
    std::function<std::uint64_t()> heap_used;
    std::uint64_t heap_pause_above = 100ull << 20;
    std::uint64_t heap_resume_below = 92ull << 20;

    // --- failure handling
    int negative_ttl_s = 24 * 3600;             // permanent failures (404, bad type, certificate ...)
    int host_fail_threshold = 5;                // consecutive transient failures before a pause
    int host_backoff_s = 60;
    std::vector<int> retry_schedule_s = {2, 8, 30, 120, 600}; // then park_s
    int park_s = 3600;
    int offline_probe_s = 30;                   // while DNS is unreachable, one probe per period

    // --- misc
    tls::Limits tls_limits;                     // timeouts etc. (max_body is set per kind)
    const Clock *clock = nullptr;
    std::function<void(int level, const std::string &line)> log; // 0 error, 1 warn, 2 info
};

enum class Phase : std::uint8_t
{
    stopped,
    disabled_dns, // DnsMode::off (or the chosen mode is unavailable): the user must choose
    ca_error,     // no usable CA bundle: HTTPS is refused rather than unverified
    entropy_error,
    offline,      // DNS resolver unreachable; probing
    paused_heap,  // fetching paused: heap use above the limit
    ready,
};

struct ServiceState
{
    Phase phase = Phase::stopped;
    std::string detail;      // one user-facing sentence for Settings / the grid placeholder
    DnsMode dns_mode = DnsMode::off;
    bool system_dns_compiled = false;
    int ca_certs = 0;
    std::string ca_source;   // which file the bundle came from
    bool disk_ok = false;    // cache directory usable
    bool clock_suspect = false; // certificates look "expired / not yet valid" on several hosts
    bool heap_paused = false;
    int queued = 0, running = 0;
};

struct ArtStats
{
    std::uint64_t requests = 0, disk_hits = 0, handoff_hits = 0, negative_hits = 0;
    std::uint64_t net_ok = 0, net_bytes = 0, connections_reused = 0, tls_resumed = 0;
    std::uint64_t fail_transient = 0, fail_permanent = 0, backoff_blocked = 0, host_pauses = 0;
    std::uint64_t refused_by_guard = 0, heap_pauses = 0, deduped = 0, stale_served = 0;
    DiskCache::Stats disk;
};

enum class Fetch : std::uint8_t
{
    ready,    // bytes are set
    pending,  // a worker is on it (or it is queued / paused): ask again shortly
    failed,   // see permanent / error; do not ask again before retry_after_s
    disabled, // no network is allowed right now (see ArtService::state())
};

struct ArtResult
{
    Fetch state = Fetch::pending;
    std::string bytes;        // ready: the encoded image
    std::string content_type; // ready: sniffed, "image/webp" | "image/png" | "image/jpeg"
    bool permanent = false;   // failed: cached negatively, will not be retried soon
    int http_status = 0;      // failed: the HTTP status when there was one
    int retry_after_s = 0;    // failed (transient): earliest useful retry
    std::string error;
};

class ArtService
{
  public:
    explicit ArtService(ArtConfig config);
    ~ArtService();
    ArtService(const ArtService &) = delete;
    ArtService &operator=(const ArtService &) = delete;

    // Opens the cache, loads the CA bundle, starts the workers. Always returns:
    // problems are reported through state() (and the returned text). True when
    // the service can fetch once a DNS mode is enabled.
    bool start(std::string *problem = nullptr);
    void stop();

    // Non-blocking; see the file comment. Any thread.
    ArtResult request(const std::string &url, Kind kind = Kind::cover, Lane lane = Lane::visible);
    // Queue background fetches for URLs that are not cached yet. Returns how many were queued.
    std::size_t prefetch(const std::vector<std::string> &urls, Kind kind = Kind::cover,
                         Lane lane = Lane::prefetch);
    // True when the bytes for this URL are already in the disk cache. Cheap, any thread.
    bool is_cached(const std::string &url);
    // Drops queued (not running) prefetch and idle-fill jobs, e.g. when the grid changes.
    void cancel_background();
    // Test/utility: request() until it is not pending, up to timeout_ms.
    ArtResult fetch_blocking(const std::string &url, Kind kind = Kind::cover, int timeout_ms = 30000);

    // Settings. Changing the mode forgets in-memory failure state (backoff, offline).
    void set_dns_mode(DnsMode mode);
    void set_doh_endpoints(std::vector<std::string> endpoints);
    DnsMode dns_mode() const;

    ServiceState state() const;
    ArtStats stats() const;
    bool idle() const; // nothing queued or running
    // One small non-image document (the popularity data), fetched on the CALLING thread through
    // the same TLS client, CA bundle and DNS choice as the covers. https only; the body is capped
    // at max_bytes. No cache here: the caller keeps its own copy. Fails with a sentence in `error`
    // (also when the art service is not ready, e.g. DNS-over-HTTPS is off).
    struct Document
    {
        int status = 0;
        std::string body;
        std::string error;
        bool time_error = false; // the console clock is probably wrong
    };
    Document fetch_document(const std::string &url, std::size_t max_bytes);
    // One large file (the Pegasus DL release), fetched on the CALLING thread like fetch_document, but
    // kept as bytes, with a long total timeout, a progress report and a redirect-target check. `cancel`
    // is polled between reads. Not cached; the caller owns the bytes. Same network gate as the art.
    struct DownloadHooks
    {
        std::function<bool(const std::string &)> redirect_ok; // false refuses that redirect target
        std::function<void(std::uint64_t)> progress;          // body bytes so far
        std::function<bool()> cancel;                         // true: stop
        // Streaming: when set the body is NOT kept in Download::body; it arrives here in order, in
        // pieces of at most 16 KiB, only for the final 2xx answer. false aborts (error "sink").
        std::function<bool(const std::uint8_t *, std::size_t)> sink;
        int total_timeout_s = 900;
    };
    struct Download
    {
        int status = 0;
        std::vector<std::uint8_t> body;
        std::string error;
        bool time_error = false;
    };
    Download fetch_download(const std::string &url, std::size_t max_bytes, const DownloadHooks &hooks);
    // Settings > Clear art cache. Also forgets failure and backoff state.
    void clear_cache();
    // Forget negative records and backoff only ("retry failed covers").
    void retry_failed();

    DiskCache &disk() { return *disk_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<DiskCache> disk_;
};

} // namespace wings::art
