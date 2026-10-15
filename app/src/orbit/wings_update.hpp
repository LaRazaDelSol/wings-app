// Wings TV app - "Is there a newer Wings?": a read-only look at the project's latest release.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Looking is read-only. Fetching and placing the package is wings_install.hpp, and only on the player's
// confirmed presses (a mounted package cannot be replaced while it runs, so the last step is by hand too:
// docs/guides/updating.md). This tells them there is something to fetch:
//   * once the network is ready, and then at most every 6 hours, it asks GitHub for the latest release of
//     the Wings repository (one small JSON, no identifier sent beyond the fixed User-Agent) and remembers the tag;
//   * the answer is compared with this app's own contentVersion (00.001.001 is release v0.1.1);
//   * Settings > Wings > Updates shows the result and the steps, and a newer release is announced once per start.
// It is on by default and the player can turn it off (Settings); it uses the same network gate as cover art, so
// with cover downloads off Wings makes no connection at all.
//
// The pure parts (release JSON, tag, advice, cache time, saved copy) are free functions; the service is the one
// background job at a time that uses them.
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

inline constexpr const char *kWingsLatestReleaseUrl =
    "https://api.github.com/repos/LaRazaDelSol/wings-app/releases/latest";
// Shown to the player (never fetched from the answer: nothing in the reply is trusted as an address).
inline constexpr const char *kWingsReleasePage = "github.com/LaRazaDelSol/wings-app/releases/latest";
inline constexpr std::size_t kWingsReleaseCap = 256u << 10;
inline constexpr int kWingsCheckTtlS = 6 * 3600;
inline constexpr int kWingsRetryS = 600; // a failed attempt is not repeated sooner

inline constexpr const char *kWingsAssetName = "PPSA99178.ffpkg";
inline constexpr const char *kWingsDigestAssetName = "PPSA99178.ffpkg.sha256";

// Why a release has no package Wings will fetch.
enum class WingsAssetIssue : std::uint8_t
{
    none,
    missing,        // no PPSA99178.ffpkg in the release
    bad_size,       // size absent, zero, too small to be an image or larger than the limit
    no_digest,      // the API gives no readable sha256 digest
    bad_url,        // the download address is not the official one for this tag
    no_checksum,    // no PPSA99178.ffpkg.sha256 asset (or its address is not the official one)
    digest_differs  // the API digest and the published .sha256 text disagree (set after reading the text)
};

// The package of one release: everything the installer needs, taken from the release JSON only.
struct WingsAsset
{
    bool downloadable = false;
    WingsAssetIssue issue = WingsAssetIssue::missing;
    std::uint64_t size = 0;
    std::string sha256;     // 64 lower-case hex, from the API digest
    std::string url;        // the official address for this tag (never taken from the reply)
    std::string digest_url; // the .sha256 asset's official address
};
struct WingsRelease
{
    std::string tag;
    WingsAsset asset;
};

// ---- pure rules ----
// The latest release: the tag plus its package asset (size, digest, official addresses). Same refusals as
// parse_wings_latest; an asset problem does not refuse the release, it leaves asset.downloadable false.
bool parse_wings_release(std::string_view json, WingsRelease *out, std::string *error);
// "https://github.com/LaRazaDelSol/wings-app/releases/download/<tag>/<asset>", "" for a bad tag.
std::string wings_asset_url(std::string_view tag);
std::string wings_digest_url(std::string_view tag);
// The first address must be exactly this tag's; a redirect only to GitHub's own hosts.
bool wings_url_allowed(std::string_view url, bool redirect, std::string_view tag);
// The .sha256 text ("<hex>  PPSA99178.ffpkg" on one line, or just "<hex>"): the hex when the line names our
// file or no file.
bool parse_wings_checksum_text(std::string_view text, std::string *hex);
// Words for an issue (translated).
std::string wings_asset_issue_text(WingsAssetIssue issue);

// The tag of the latest published release from the "releases/latest" reply. False (with *error) when the
// reply is not a release object, is a draft or a pre-release, or its tag is not v<major>.<minor>.<patch>.
bool parse_wings_latest(std::string_view json, std::string *tag, std::string *error);

// True when the latest release should be asked for again (never checked, older than the TTL, or the clock
// went backwards), or when `force` (the player pressed Check).
bool wings_check_due(std::int64_t checked_at, std::int64_t now, bool force);

enum class WingsAdvice : std::uint8_t
{
    not_checked, // no release is known
    unreadable,  // a release is known but this app's own version cannot be read
    up_to_date,
    newer,       // a newer release exists
    ahead        // this build is newer than the latest release (a test build)
};
// running = this app's contentVersion ("00.001.001"); latest_tag "" means not checked.
WingsAdvice wings_advice(std::string_view running, std::string_view latest_tag);

// The saved answer (payload text, inside the checked save container on disk).
std::string encode_wings_cache(std::int64_t checked_at, std::string_view tag);
std::string encode_wings_cache(std::int64_t checked_at, std::string_view tag, const WingsAsset &asset);
bool decode_wings_cache(std::string_view payload, std::int64_t *checked_at, std::string *tag);
// Also the package size and digest of that release (asset stays "missing" when the copy has none).
bool decode_wings_cache(std::string_view payload, std::int64_t *checked_at, std::string *tag, WingsAsset *asset);

// ---- the startup prompt ----
// A newer release is offered in a dialog once per start; "Later" leaves that version alone for this long,
// then it is offered again. A newer tag than the snoozed one is offered at once.
inline constexpr std::int64_t kWingsSnoozeS = 3 * 24 * 3600;

// The saved "Later": which release, and until when (unix seconds).
struct WingsSnooze
{
    std::string tag;
    std::int64_t until = 0;
};
// True when the dialog may be shown for latest_tag: a tag is known, and it is not the snoozed one or the
// snooze has run out. A snooze reaching further than kWingsSnoozeS ahead (the clock went backwards) is over.
bool wings_should_prompt(std::int64_t now, std::string_view latest_tag, const WingsSnooze &snooze);
// "Later" pressed at `now` for `tag`.
WingsSnooze wings_snooze_for(std::string_view tag, std::int64_t now);
// The saved record (payload text, inside the checked save container on disk).
std::string encode_wings_snooze(const WingsSnooze &snooze);
bool decode_wings_snooze(std::string_view payload, WingsSnooze *out);

// ---- the service ----
struct WingsUpdateConfig
{
    DocFetcher *docs = nullptr;                          // the same fetcher as popularity
    std::function<bool(std::string *)> ready;            // the network gate; false with a sentence
    std::vector<std::string> cache_paths;                // the saved answer; first writable wins
    // The saved "Later"; first writable wins. Empty: "wings_update_snooze.v1" beside each cache path.
    std::vector<std::string> snooze_paths;
    std::string running;                                 // this app's contentVersion
    std::function<std::int64_t()> now;                   // unix seconds (default: the system clock)
};

struct WingsUpdateStatus
{
    bool attached = false;       // false in the preview and in tests that do not attach
    bool enabled = true;         // the player's switch
    bool busy = false;
    std::int64_t checked_at = 0; // unix time of the last successful check, 0 never
    bool have_latest = false;
    std::string latest_tag;      // "v0.1.2"
    WingsAsset asset;            // the package of latest_tag
    std::string latest_text;     // "0.1.2"
    std::string running_text;    // "0.1.1"
    WingsAdvice advice = WingsAdvice::not_checked;
    std::string error;           // why the last check failed, "" when it did not
    WingsSnooze snooze;          // the saved "Later" (tag "" when none)
};

class WingsUpdate
{
  public:
    WingsUpdate() = default;
    ~WingsUpdate();
    WingsUpdate(const WingsUpdate &) = delete;
    WingsUpdate &operator=(const WingsUpdate &) = delete;

    // Frame thread. Loads the saved answer.
    void attach(WingsUpdateConfig config);
    void set_enabled(bool on);
    WingsUpdateStatus status() const; // any thread; a copy

    // Frame thread; each starts the one background job unless it is busy or not allowed.
    void auto_check(); // only when enabled, due (6 h) and the network is ready; nothing is reported when skipped
    bool check_now();  // the player pressed Check (works while the switch is off too: it was asked for)
    void wait_idle();  // tests and shutdown

    // Frame thread. True once per start per release when the startup dialog should show now: attached, the
    // switch on, a newer release known and wings_should_prompt agrees. Marks it shown. The screen asks only
    // when the install flow is at "available" (wings_flow_snapshot), and asks this last.
    bool take_prompt();
    // Frame thread. "Later": the latest release is not offered again for kWingsSnoozeS; saved on disk.
    void snooze_latest();

  private:
    void run_check();
    bool start_job();
    bool gate(std::string *why) const;
    void refresh_advice_locked();
    std::vector<std::string> snooze_paths() const;

    WingsUpdateConfig config_;
    mutable Mutex mutex_;
    WingsUpdateStatus status_;
    std::string prompted_tag_;    // the release the dialog was shown for in this start
    std::int64_t attempt_at_ = 0; // the last background check, so a failure is not retried every frame
    Thread thread_;
};
WingsUpdate &wings_update();

// The words for Settings: the row's right-hand value and its detail line (translated).
struct WingsUpdateText
{
    std::string value;
    std::string detail;
};
WingsUpdateText wings_update_text(const WingsUpdateStatus &status);
// How to install a newer release by hand (translated): a title, a line under it and four steps. The dialog is
// a scrolling list (a confirm dialog would cut the text off). It is also the manual fallback when the
// in-app install cannot place the file.
struct WingsUpdateStep
{
    std::string label;
    std::string detail;
};
struct WingsUpdateGuide
{
    std::string title;
    std::string subtitle;
    std::vector<WingsUpdateStep> steps;
};
WingsUpdateGuide wings_update_guide(const WingsUpdateStatus &status);
// The one-line announcement shown once per start when a newer release is known (translated).
std::string wings_update_toast(const WingsUpdateStatus &status);

} // namespace orbit
