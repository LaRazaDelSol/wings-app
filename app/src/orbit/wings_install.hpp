// Wings TV app - fetching and placing a newer Wings package, only on the player's confirmed presses.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The flow (Settings > Wings > Updates), each step doing what its name says:
//   Check     : the release and its published checksum (it must agree with the API digest).
//   Download  : (Select) make sure the Wings update helper runs (wings_helper_client.hpp); it downloads the
//               package to its own folder and checks size, SHA-256 and the image magic. Nothing is installed:
//               "downloaded and checked" waits for the player. A download the helper already holds is reused.
//   Install   : (a confirmed press) the helper checks the downloaded file again and puts it in /data/homebrew,
//               keeping the old file as PPSA99178.ffpkg.old. No download. The app follows its status.
//               When the helper cannot be started, the exact manual steps are shown.
//   Restart   : (a second confirmed press, in the screen) quits Wings; opening it again runs the new version.
// Never: install beside a folder copy (/data/homebrew/PPSA99178), replace a copy the player placed
// themselves without being asked, or install a release that is not newer than the running one.
//
// Everything here is free of the network and of the console: the checksum downloader and the helper's
// transport are interfaces (tests use fakes; main.cpp wires the console).
#pragma once

#include "orbit/stream_download.hpp"
#include "orbit/thread.hpp"
#include "orbit/wings_update.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orbit
{

inline constexpr const char *kWingsHomebrewDir = "/data/homebrew";
inline constexpr const char *kWingsFolderName = "PPSA99178"; // the unpacked-folder form of the package
inline constexpr const char *kWingsNoteDir = "/download0/wings"; // placed.v1

// ---- pure rules ----
enum class WingsTarget : std::uint8_t
{
    unknown,    // not looked at yet
    none,       // no PPSA99178.ffpkg in /data/homebrew (Wings runs from somewhere else)
    wings_copy, // the file Wings placed itself last time (the helper's record matches)
    own_copy,   // a file the player placed: replacing it needs a question first
    folder,     // /data/homebrew/PPSA99178 is a folder: Wings never installs beside it
    blocked     // something else is where the file should be
};

// Download speed and time left from progress samples (seconds on a steady clock, bytes so far). Pure.
inline constexpr double kWingsRateWindowS = 5.0; // speed over the last few seconds
class WingsRate
{
  public:
    void reset();
    void sample(double t_s, std::uint64_t bytes); // bytes going backwards (or time) starts again
    double bytes_per_s() const;                   // 0 while unknown (less than half a second of samples)
    std::int64_t eta_s(std::uint64_t total) const; // -1 while unknown; 0 when done

  private:
    struct Sample
    {
        double t;
        std::uint64_t bytes;
    };
    std::vector<Sample> samples_;
};

// ---- the state machine ----
enum class WingsInstallPhase : std::uint8_t
{
    idle,        // nothing fetched
    checking,    // reading the published checksum, starting the helper
    downloading, // the helper streams the package (got_bytes == total_bytes: it checks it)
    verified,    // checksums agree and the helper is ready: waiting for the player's confirm
    placing,     // the helper moves it into /data/homebrew
    placed,      // done: close Wings and open it again
    manual,      // the helper cannot do it: the steps are for the player
    blocked,     // nothing was changed and nothing can be: say why (folder copy, other thing)
    failed       // something went wrong; the old file is as it was (or restored); retry is allowed
};

enum class WingsPlacedBy : std::uint8_t
{
    none,
    direct,  // the console's file system (earlier Wings)
    pegasus, // Pegasus DL's file API (earlier Wings)
    helper   // the Wings update helper
};

// The note Wings keeps after it placed a package (written whether or not the file can be looked at from Wings'
// sandbox), so "Installed, restart to finish" survives until the new version runs. Removed once the version
// that runs is that release or newer.
struct WingsPlacedNote
{
    std::string tag;
    WingsPlacedBy by = WingsPlacedBy::none;
};
std::string encode_wings_placed(const WingsPlacedNote &note);
bool decode_wings_placed(const std::string &text, WingsPlacedNote *out);

struct WingsInstallStatus
{
    WingsInstallPhase phase = WingsInstallPhase::idle;
    std::string tag;                  // the release this is about
    std::uint64_t got_bytes = 0;      // downloading
    std::uint64_t total_bytes = 0;
    WingsTarget target = WingsTarget::unknown; // known once verified
    WingsPlacedBy placed_by = WingsPlacedBy::none;
    bool needs_replace_confirm = false; // verified and the target is a copy the player placed
    std::string message;              // one translated sentence for the row (what happened / what is next)
    std::string package_url;          // the official package address (the manual steps name it)
    bool install_tried = false;       // the helper was asked to install (a failure is on the install step)
    bool old_kept = false;            // placed: the old file is at PPSA99178.ffpkg.old
    double speed_bps = 0.0;           // downloading: bytes per second, 0 while unknown
    std::int64_t eta_s = -1;          // downloading: seconds left, -1 while unknown
    bool busy() const
    {
        return phase == WingsInstallPhase::checking || phase == WingsInstallPhase::downloading ||
               phase == WingsInstallPhase::placing;
    }
};

class WingsHelperClient; // wings_helper_client.hpp
struct WingsHelperStatus;

// The helper keeps a release's version without the tag's "v" (0.1.2 for v0.1.2): true when they name one release.
bool wings_same_release(const std::string &helper_version, const std::string &tag);

// What a helper status means for installing release `tag`. Each has its own handling (wings_install.cpp).
enum class WingsHelperSituation : std::uint8_t
{
    ready,          // free, nothing in the way: the install can be asked for
    following,      // already downloading/placing this release (Wings was reopened): follow it
    other_job,      // working on another release: wait for it (with its progress), then look again
    already_placed, // this release is the file the helper placed: nothing to install, restart to finish
    old_waiting,    // PPSA99178.ffpkg.old from an earlier update is still there: tidy it first
    downloaded      // this release is downloaded and checked: Install places it (no new download)
};
WingsHelperSituation wings_helper_situation(const WingsHelperStatus &s, const std::string &tag);

struct WingsInstallConfig
{
    StreamingDownloader *downloader = nullptr; // the published .sha256 (a small text file)
    WingsHelperClient *helper = nullptr;       // null: the manual steps only
    std::string note_dir = kWingsNoteDir;      // where placed.v1 lives
    std::string running; // this app's contentVersion, to tidy the .old file after a successful update
    std::function<void(const std::string &)> log;
    std::function<double()> clock;        // seconds on a steady clock, for the download speed (default: steady_clock)
    std::function<void(int ms)> sleep_ms; // between status reads (default: usleep)
    int poll_ms = 250;                    // status reads while the helper works
    int lost_polls = 40;                  // unanswered reads in a row before the helper counts as gone
};

class WingsInstaller
{
  public:
    WingsInstaller() = default;
    ~WingsInstaller();
    WingsInstaller(const WingsInstaller &) = delete;
    WingsInstaller &operator=(const WingsInstaller &) = delete;

    // Frame thread. Restores "installed" from placed.v1, or (once the new version runs) asks the helper to tidy
    // the .old file in the background.
    void attach(WingsInstallConfig config);
    bool attached() const;
    WingsInstallStatus status() const; // any thread; a copy

    // Frame thread. Remembers the release the screen shows (nothing is fetched or staged in the app).
    void note_release(const WingsRelease &release);

    // Each starts the one background job unless busy or not allowed; false says nothing started.
    bool start_download(const WingsRelease &release); // Download update
    bool install(bool replace_own_confirmed);         // Install update (after the player's confirm)
    void cancel();                                    // asks the helper to stop a download between chunks
    void wait_idle();

    // The same jobs, synchronously (tests; the threads call these).
    void run_download(const WingsRelease &release);
    void run_install(bool replace_own_confirmed);

    void run_tidy();

  private:
    void set_phase(WingsInstallPhase phase, std::string message);
    void say(const std::string &line) const;
    void follow(const WingsRelease &release, bool installing);
    bool ask_helper(const WingsRelease &release, bool installing, bool replace, WingsHelperStatus *hs);
    void record_placed(const std::string &tag);
    void log_helper(const char *when, const WingsHelperStatus &s) const;
    // Resolves the helper's situation for the release until it is `ready` (true, *hs current). False: the
    // phase and message are set (following, placed, failed, cancelled ...) and the job is over.
    bool settle_helper(const WingsRelease &release, WingsHelperStatus *hs);
    bool wait_for_helper(WingsHelperStatus *hs); // polls while another job runs, showing its progress
    void already_placed(const WingsRelease &release, const WingsHelperStatus &hs);
    void restore_placed_locked(bool *tidy);
    bool start(std::function<void()> job);

    WingsInstallConfig config_;
    mutable Mutex mutex_;
    WingsInstallStatus status_;
    WingsRelease release_;
    std::string noted_tag_;
    std::atomic<bool> cancel_{false};
    bool attached_ = false;
    WingsRate rate_; // guarded by mutex_
    Thread thread_;
};
WingsInstaller &wings_installer();

// ---- words ----
struct WingsInstallStep
{
    std::string label;
    std::string detail;
};
// The manual steps (translated): what the `manual` and `blocked` outcomes show.
std::vector<WingsInstallStep> wings_install_manual_steps(const WingsInstallStatus &status);
// The text of the install confirm dialog (translated); wording depends on the target.
std::string wings_install_confirm_text(const WingsInstallStatus &status, const std::string &version_text);
// The right-hand value and detail of the row for the status (translated).
struct WingsInstallText
{
    std::string value;
    std::string detail;
};
WingsInstallText wings_install_text(const WingsInstallStatus &status);

// ---- the update flow as the screen draws it ----
// One state for the whole flow (the checker's answer plus the installer's phase), drawn as four steps.
enum class WingsFlowState : std::uint8_t
{
    idle,             // nothing newer known (or the Updates switch is off)
    available,        // a newer release with a package Wings can fetch
    downloading,      // reading the published checksum, then streaming the package
    checking,         // the whole package is in: SHA-256 and image check
    ready_to_install, // verified, waiting for the player's install confirm
    installing,       // placing it in /data/homebrew
    installed,        // in place: close Wings and open it again to finish
    manual,           // Wings will not place it: the exact steps are shown
    failed            // something went wrong; reason says what, can_retry whether Retry applies
};
enum class WingsFlowStep : std::uint8_t
{
    check,    // find the update and its published checksum
    download, // the helper downloads and checks the package
    install,  // the helper puts the downloaded package in place
    restart   // close Wings and open it again
};
inline constexpr int kWingsFlowSteps = 4;

// Which buttons apply now (the screen shows only these).
struct WingsFlowButtons
{
    bool download = false; // available: start the download
    bool later = false;    // available: snooze this release (WingsUpdate::snooze_latest)
    bool cancel = false;   // downloading: stop between chunks
    bool install = false;  // ready_to_install: opens the install confirm (wings_install_confirm_text)
    bool quit = false;     // installed: the quit confirm; Wings never restarts the chain itself
    bool steps = false;    // manual / failed: show the manual steps
    bool retry = false;    // failed and can_retry: download again
};

struct WingsFlowSnapshot
{
    WingsFlowState state = WingsFlowState::idle;
    WingsFlowStep step = WingsFlowStep::download; // the current step
    int step_index = 0;                           // 0..3, same as `step`
    bool step_done = false;                       // installed: every step up to restart is done
    std::string tag;                              // the release this is about
    std::string version_text;                     // "0.1.2"
    std::string running_text;                     // "0.1.1"
    std::string title;                            // one short line (translated)
    std::string text;                             // what is happening / what is next (translated)
    float progress = 0.0f;                        // 0..1; negative: working, amount unknown
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    double speed_bps = 0.0;                       // 0 while unknown
    std::int64_t eta_s = -1;                      // -1 while unknown
    std::string speed_text;                       // "2.4 MB/s", "" while unknown
    std::string eta_text;                         // "About 12 s left", "" while unknown
    std::string reason;                           // failed: why
    bool can_retry = false;
    bool needs_replace_confirm = false;           // ready_to_install: the confirm must name the player's copy
    std::vector<WingsInstallStep> manual_steps;   // manual (and failed, for the Steps button)
    WingsFlowButtons buttons;
};
// Pure: the same inputs always give the same snapshot. A placed package stays `installed` whatever the
// checker says, until Wings runs that version (WingsInstaller::attach then clears it).
WingsFlowSnapshot wings_flow_snapshot(const WingsUpdateStatus &update, const WingsInstallStatus &install);
// "2.4 MB/s" (translated), "" for 0.
std::string wings_speed_text(double bytes_per_s);
// "About 12 s left" / "About 3 min left" (translated), "" for unknown.
std::string wings_eta_text(std::int64_t seconds);

} // namespace orbit
