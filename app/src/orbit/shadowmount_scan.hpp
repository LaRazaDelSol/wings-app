// Wings TV app - After a download finishes: ask ShadowMount to scan, and say what happened.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ShadowMount registers a game only when it scans, and it will not scan while a game runtime
// (Wings itself, mounted by ShadowMount) is active. So after a download completes Wings:
//   1. waits 5 s (several completions become one request),
//   2. probes /api/v1/version (2 s) and posts /api/v1/scan once,
//   3. shows what the answer means: "scanning now" (then polls /api/v1/games every 5 s for up
//      to 60 s until the title id appears), "waits while Wings is open", or "API not found".
// Wings never closes itself. This class holds no sockets and no UI: it is driven one frame at a
// time (update), runs its blocking calls through ShadowMountEnv::run, and the Downloads tab
// draws its banner().

#pragma once

#include "orbit/after_download_mode.hpp"
#include "orbit/model.hpp"
#include "orbit/shadowmount_client.hpp"
#include "orbit/thread.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace orbit
{

constexpr double kScanDebounceS = 5.0;
constexpr double kScanPollEveryS = 5.0;
constexpr double kScanPollForS = 60.0;

enum class ScanPhase : std::uint8_t
{
    idle,        // nothing to say
    waiting,     // a completion is being debounced
    asking,      // the request is out
    deferred,    // queued, but ShadowMount waits while Wings is open
    scanning,    // ShadowMount is scanning now
    registered,  // the title id of a finished job is in ShadowMount's game list
    unreachable, // no API, API off, or it refused
    timed_out,   // scanning, but the title id did not appear within 60 s
};

struct ScanBanner
{
    ScanPhase phase = ScanPhase::idle;
    std::string reason;                  // scan_deferred_reason when deferred
    std::vector<std::string> registered; // names of games seen in ShadowMount's list
    // The banner is on screen (and can take focus) in these phases.
    bool visible() const
    {
        return phase != ScanPhase::idle && phase != ScanPhase::waiting && phase != ScanPhase::asking;
    }
    // "Close Wings now" / "Not now" make sense: ShadowMount cannot finish while Wings runs.
    bool offers_close() const
    {
        return phase == ScanPhase::deferred || phase == ScanPhase::unreachable ||
               phase == ScanPhase::timed_out;
    }
};

class AfterInstall
{
  public:
    explicit AfterInstall(ShadowMountEnv env) : client_(std::move(env)), shared_(std::make_shared<Shared>())
    {
    }

    void set_mode(AfterDownload mode)
    {
        mode_ = mode;
    }
    AfterDownload mode() const
    {
        return mode_;
    }
    // Frame thread, every frame, whichever tab shows. now: monotonic seconds.
    void update(double now, const std::vector<Job> &jobs, bool have_queue);
    // The Downloads Actions menu: scan now, whatever the setting says.
    void scan_now(double now);
    // "Not now": hide the banner and stop polling.
    void dismiss();

    const ScanBanner &banner() const
    {
        return banner_;
    }
    bool busy() const
    {
        return in_flight_ || due_ >= 0.0;
    }
    // Number of /api/v1/scan requests sent (tests).
    int scans_sent() const
    {
        return scans_sent_;
    }
    // Tests and previews: show a banner state without any request.
    void debug_show(const ScanBanner &banner)
    {
        banner_ = banner;
    }

  private:
    struct Shared
    {
        Mutex mutex;
        bool scan_ready = false;
        bool games_ready = false;
        VersionReply version;
        ScanReply scan;
        GamesReply games;
    };
    struct Watched
    {
        std::string title_id;
        std::string name;
    };

    void observe(double now, const std::vector<Job> &jobs);
    void start_scan(double now);
    void start_games();
    void collect(double now);
    void note_registered(const std::set<std::string> &ids);

    ShadowMountClient client_;
    std::shared_ptr<Shared> shared_;
    AfterDownload mode_ = AfterDownload::scan;
    ScanBanner banner_;

    bool seeded_ = false;
    std::map<std::string, std::string> last_status_; // job id -> status
    std::set<std::string> handled_;                  // job ids whose completion was acted on

    std::vector<Watched> pending_; // completed since the last request
    std::vector<Watched> watch_;   // looked for in the game list
    double due_ = -1.0;            // when the debounce ends (negative: none)
    bool in_flight_ = false;
    bool games_in_flight_ = false;
    bool drop_scan_ = false; // dismissed while the scan request was out
    double next_poll_ = 0.0;
    double poll_deadline_ = 0.0;
    int scans_sent_ = 0;
};

// The banner's words (translated). detail may be empty.
std::string scan_banner_text(const ScanBanner &banner);
std::string scan_banner_detail(const ScanBanner &banner);

// One worker for the console: runs one job at a time (the caller never overlaps them).
class ThreadRunner
{
  public:
    void operator()(std::function<void()> job);

  private:
    Thread thread_;
};

} // namespace orbit
