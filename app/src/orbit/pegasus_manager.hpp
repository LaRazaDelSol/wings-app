// Wings TV app - "Pegasus DL on this console": status, an opt-in download, a hand-off.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three safe layers, none of them automatic beyond the first:
//   1. Status: the official release list is read (at most every 6 hours, or when the player presses
//      Check) and compared with the running version and the range Wings was tested with.
//   2. Download: only when the player confirms. One file, from the exact official URL pattern, over
//      https, redirects limited to GitHub's own hosts, a 64 MiB cap, SHA-256 against the digest the
//      release API publishes, written to /data/wings/pegasus/<tag>/ only after it matches.
//   3. Hand-off: the path is shown; an experimental button can send the verified file to the console's
//      payload loader on 127.0.0.1:9021, and only while Pegasus DL itself is not answering.
// Wings never bundles, mirrors or starts Pegasus DL. The project publishes no licence or redistribution
// terms, so the download is the player's own request, fetched from the author's own release page.
//
// The pure parts (release JSON, URL rules, digest, version order, cache time, advice) are free
// functions; the service is the one background job at a time that uses them.
#pragma once

#include "orbit/popularity.hpp" // DocFetcher
#include "orbit/thread.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

inline constexpr const char *kPegasusReleasesUrl =
    "https://api.github.com/repos/pegasus-ps5/pegasus-dl/releases?per_page=10";
inline constexpr const char *kPegasusAssetName = "pegasus_dl.elf";
inline constexpr std::size_t kPegasusDownloadCap = 64u << 20;
inline constexpr std::size_t kPegasusReleasesCap = 1u << 20;
inline constexpr int kPegasusCheckTtlS = 6 * 3600;
inline constexpr std::uint16_t kPegasusLoaderPort = 9021;

struct PegasusRelease
{
    std::string tag;          // "v1.11.0"
    std::string published_at; // as the API gives it
    std::uint64_t size = 0;   // bytes of the asset
    std::string sha256;       // 64 lower-case hex, from the API's "sha256:<hex>" digest
    bool prerelease = false;
    // The asset is there, its URL is the official pattern for this tag, the digest is readable and
    // the size is within the cap. Only such a release can be downloaded.
    bool downloadable = false;
};

// ---- pure rules ----
// Reads the releases array. Drafts are skipped. False (with *error) when it is not a JSON array.
bool parse_pegasus_releases(std::string_view json, std::vector<PegasusRelease> *out, std::string *error);
// The newest non-prerelease by version number; null when there is none.
const PegasusRelease *pick_latest_pegasus_release(const std::vector<PegasusRelease> &releases);
// -1, 0, 1; false in *ok when either text is not a version.
int compare_pegasus_versions(std::string_view a, std::string_view b, bool *ok);

// "v1.11.0" only: digits and dots, three parts, nothing else (it becomes part of a path and a URL).
bool pegasus_tag_ok(std::string_view tag);
// https://github.com/pegasus-ps5/pegasus-dl/releases/download/<tag>/pegasus_dl.elf, "" for a bad tag.
std::string pegasus_download_url(std::string_view tag);
enum class PegasusUrlRole : std::uint8_t
{
    first,   // must be exactly the official URL of `tag`
    redirect // https on github.com, objects.githubusercontent.com or release-assets.githubusercontent.com
};
bool pegasus_url_allowed(std::string_view url, PegasusUrlRole role, std::string_view tag = {});

// "sha256:<hex>" (any case) -> lower-case 64 hex.
bool parse_sha256_digest(std::string_view digest, std::string *hex);

// SHA-256 (FIPS 180-4), portable: the same code runs on the console and in the host tests.
class Sha256
{
  public:
    Sha256();
    void update(const void *data, std::size_t size);
    std::string finish_hex(); // lower-case; the object is spent afterwards
  private:
    void block(const std::uint8_t *chunk);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t fill_ = 0;
    std::uint64_t total_ = 0;
};
std::string sha256_hex(std::string_view data);

bool looks_like_elf(const std::uint8_t *data, std::size_t size);

// Release list cache: true when the list should be asked for again (never checked, older than the
// TTL, or the clock went backwards), or when `force` (the player pressed Check).
bool pegasus_check_due(std::int64_t checked_at, std::int64_t now, bool force);

enum class PegasusAdvice : std::uint8_t
{
    not_running,    // Pegasus DL is not answering
    not_checked,    // running, but the release list has not been read
    unreadable,     // running, but its version text is not a version
    up_to_date,
    newer_tested,   // a newer release exists and is inside the range Wings was tested with
    newer_untested  // a newer release exists, outside the tested range
};
// running_version is only looked at when `running`; latest_tag "" means not checked.
PegasusAdvice pegasus_advice(bool running, std::string_view running_version, std::string_view latest_tag);
std::string pegasus_advice_text(PegasusAdvice advice); // translated

// The saved copy of the list (payload text, inside the checked save container on disk).
std::string encode_pegasus_cache(std::int64_t checked_at, const std::vector<PegasusRelease> &releases);
bool decode_pegasus_cache(std::string_view payload, std::int64_t *checked_at, std::vector<PegasusRelease> *out);

// ---- what the service needs from the console ----
struct PegasusDownloadHooks
{
    std::function<bool(const std::string &)> redirect_ok;
    std::function<void(std::uint64_t)> progress;
    std::function<bool()> cancel;
};
struct PegasusDownloadResult
{
    int status = 0;
    std::vector<std::uint8_t> body;
    std::string error;
};
class PegasusDownloader
{
  public:
    virtual ~PegasusDownloader() = default;
    // The same network gate as the cover art (its service is ready). False with a sentence.
    virtual bool ready(std::string *why) = 0;
    virtual PegasusDownloadResult get(const std::string &url, std::size_t max_bytes,
                                      const PegasusDownloadHooks &hooks) = 0; // worker; blocks
};

// Plain TCP, IPv4 loopback only: streams the file at `path` to host:port and closes. False with *error.
bool send_file_to_loopback(const std::string &host, std::uint16_t port, const std::string &path,
                           const std::function<void(std::uint64_t)> &progress, std::string *error);

// ---- the service ----
enum class PegasusPhase : std::uint8_t
{
    idle,
    checking,
    downloading,
    verifying,
    sending
};

// Why the last send job stopped (the first-run flow reads it; "none" for every other outcome).
enum class PegasusFail : std::uint8_t
{
    none,
    verify,    // the file on disk did not match the published SHA-256 / size / ELF header
    no_loader, // 127.0.0.1:<port> refused the connection
    send       // any other send failure
};

struct PegasusStatus
{
    bool attached = false;      // false in the preview and the unit tests: the section says so
    bool net_ok = false;        // the network gate is open
    std::string net_note;       // why not, when it is closed
    PegasusPhase phase = PegasusPhase::idle;
    std::int64_t checked_at = 0;
    bool have_latest = false;
    PegasusRelease latest;
    std::string check_error;
    std::uint64_t got_bytes = 0;
    bool downloaded = false;    // a verified file for `latest` is on disk
    std::string path;           // where
    std::string message;        // last outcome of a download or send, "" when none
    bool message_is_error = false;
    bool sent = false;          // the last send finished
    PegasusFail fail = PegasusFail::none;
    bool busy() const
    {
        return phase != PegasusPhase::idle;
    }
};

struct PegasusManagerConfig
{
    DocFetcher *docs = nullptr;             // the releases list (the same fetcher as popularity)
    PegasusDownloader *downloader = nullptr;
    std::string root = "/data/wings/pegasus";
    std::vector<std::string> cache_paths;   // the saved release list; first writable wins
    std::string loader_host = "127.0.0.1";
    std::uint16_t loader_port = kPegasusLoaderPort;
    std::function<std::int64_t()> now;      // unix seconds (default: the system clock)
    // Experimental, off by default everywhere: before a send, try the patches in
    // pegasus_patch.hpp against the verified stock file. A patch only ever applies to the exact
    // source SHA-256 it was derived from; anything else (a different Pegasus build, a pattern that
    // moved) sends the stock file unchanged. docs/research/pegasus-dl-patch.md has the detail.
    bool experimental_patch_pegasus = false;
};

class PegasusManager
{
  public:
    PegasusManager() = default;
    ~PegasusManager();
    PegasusManager(const PegasusManager &) = delete;
    PegasusManager &operator=(const PegasusManager &) = delete;

    // Frame thread. Loads the saved list and looks for an already downloaded file.
    void attach(PegasusManagerConfig config);
    PegasusStatus status() const; // any thread; a copy

    // Frame thread; each starts the one background job unless it is busy or not allowed.
    void auto_check();          // only when due (6 h) and the gate is open; no message when skipped
    bool check_now();           // the player pressed Check
    bool download_latest();     // the player confirmed; only a downloadable `latest`
    // pegasus_online: Pegasus DL answers on 6970 right now; then the send is refused.
    bool send_to_loader(bool pegasus_online);
    void cancel();              // stops a download between reads
    // First-run flow (pegasus_offer.cpp): any listed release, and a send that first re-checks the file
    // on disk against the published SHA-256. Both refuse when a job runs; the send refuses while
    // Pegasus answers.
    std::vector<PegasusRelease> releases() const; // the list as last read (or loaded from the saved copy)
    bool download_release(const std::string &tag);
    bool verify_and_send(const std::string &tag, bool pegasus_online);
    // Frame thread, every frame: Pegasus DL answers GET /api/health right now. A verify+send job that
    // sees it just before the send ends quietly without sending (a slow Pegasus DL won the race).
    void note_pegasus_online(bool online);
    void wait_idle();           // tests

    // The path a verified download of `tag` lives at.
    std::string file_path(const std::string &tag) const;

  private:
    void run_check();
    void run_download(PegasusRelease release);
    void run_send(std::string path, std::string note = {});
    void run_verify_send(PegasusRelease release);
    // Experimental (config_.experimental_patch_pegasus only): a patched sibling of the verified
    // `stock_path`, or `stock_path` itself unchanged when nothing applied or anything went wrong.
    // Never fails the send; *note describes what happened, for the caller to report.
    std::string apply_experimental_patch(const std::string &stock_path, std::string *note) const;
    void finish(const std::string &message, bool is_error);
    bool gate(std::string *why) const;
    void refresh_downloaded_locked();
    void save_cache_locked();
    bool start_job(PegasusPhase phase, std::function<void()> body);

    PegasusManagerConfig config_;
    mutable Mutex mutex_;
    PegasusStatus status_;
    std::vector<PegasusRelease> releases_;
    bool cancel_ = false;
    bool online_now_ = false;     // the last note_pegasus_online()
    std::int64_t attempt_at_ = 0; // the last background check, so a failure is not retried every frame
    Thread thread_;
};
PegasusManager &pegasus_manager();

} // namespace orbit
