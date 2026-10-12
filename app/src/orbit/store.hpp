// Wings TV app - The app's view of Pegasus DL, kept in sync on worker threads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Orbit Store's store.hpp (GPL-3.0-or-later). Two workers talk to
// the Backend:
//   - the main worker polls health and the queue (every second while something
//     is active, else every three), the slower lists now and then (sources and
//     settings; installed games only while the Library asks; logs only while the
//     Logs screen is open), watches a browser-capture session, and carries out
//     the player's actions in order;
//   - the catalogue worker reads the 7 MB catalogue, only when the sources
//     changed (or never loaded), so a long read never stalls the queue.
// Results reach the frame loop through an inbox, so screens read a plain
// snapshot and never wait on the network.

#pragma once

#include "orbit/backend.hpp"
#include "orbit/model.hpp"
#include "orbit/thread.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace orbit
{

enum class Link : std::uint8_t
{
    connecting, // no answer yet
    online,
    offline, // Pegasus DL is not answering
};

struct Snapshot
{
    Link link = Link::connecting;
    std::string link_error; // why the last poll failed
    double offline_since = 0.0;
    std::string endpoint; // where Pegasus is expected ("127.0.0.1:6970")

    bool have_info = false;
    PegasusInfo info;
    bool have_settings = false;
    SettingsView settings;

    bool have_queue = false;
    std::vector<Job> jobs;

    bool have_catalog = false;
    bool catalog_loading = false;
    std::string catalog_error;
    std::vector<Package> packages;
    std::uint64_t catalog_serial = 0; // changes when packages are replaced
    // Each package's best link tier (pack_tier), same order as `packages`; empty
    // while unknown. Re-read when the debrid accounts change.
    std::vector<std::uint8_t> tiers;
    std::uint64_t tiers_serial = 0; // changes when tiers are replaced

    bool have_sources = false;
    std::vector<Source> sources;
    bool have_defaults = false;
    DefaultSources defaults;
    bool have_installed = false;
    std::vector<InstalledGame> installed;
    std::uint64_t installed_serial = 0; // changes when `installed` is replaced
    std::vector<LogLine> logs; // newest last, capped
    std::int64_t log_seq = 0;

    // The package whose page is open, with its links.
    PackageDetail detail;
    std::string detail_error;
    std::uint64_t detail_serial = 0;

    // The last "download this link" and the browser capture it may have started.
    LinkResult link_result;
    std::uint64_t link_serial = 0;
    CaptureView capture;
    std::uint64_t capture_serial = 0;

    // The last checked direct link.
    Probe probe;
    std::uint64_t probe_serial = 0;

    std::uint64_t serial = 0; // changes with any update

    bool online() const
    {
        return link == Link::online;
    }
    const Package *package(std::string_view id) const;
    const Job *job(std::string_view id) const;
};

// How one start_link() of a series ended ("Download all parts"), on the frame thread.
struct LinkStart
{
    std::string error; // empty: Pegasus took it
    LinkResult::Kind kind = LinkResult::Kind::dispatched;
    std::string message;
};

// The outcome of the last player action of one kind.
struct ActionState
{
    bool busy = false;
    std::string error;
    std::string message; // a short confirmation, when the action has one
    std::uint64_t serial = 0; // changes when an action finishes
};

class Store
{
  public:
    // The backend must outlive the store. start() launches the workers; tests
    // may instead call poll_once(), load_catalog_once() and run_next_action().
    explicit Store(Backend &backend);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    void start();
    void stop();

    // Frame thread: applies whatever the workers finished.
    void tick();
    const Snapshot &state() const
    {
        return state_;
    }
    // Queue actions (pause, resume, cancel, retry, clear finished, archive prompts).
    const ActionState &action() const
    {
        return channel_[0].state;
    }
    // Starting a download from a package link.
    const ActionState &link_action() const
    {
        return channel_[1].state;
    }
    // Source management.
    const ActionState &source_action() const
    {
        return channel_[2].state;
    }
    // Checking and queueing a direct link.
    const ActionState &probe_action() const
    {
        return channel_[3].state;
    }

    // ---- management calls (settings, folders, APR Emu, file manager, logs) ----
    // Lanes: admin and files show a busy state; poll is for status polls that
    // must not flash one. Calls run in order on the worker, the callback on the
    // frame thread (inside tick()).
    static constexpr int lane_admin = 4;
    static constexpr int lane_files = 5;
    static constexpr int lane_poll = 6;
    template <typename T>
    void call(int lane, std::function<Result<T>(Backend &)> work,
              std::function<void(Result<T> &&)> done)
    {
        submit(lane,
               [this, work = std::move(work), done = std::move(done)]() mutable
               {
                   Result<T> result = work(backend_);
                   Outcome out;
                   out.error = result.error;
                   out.unreachable = result.unreachable();
                   inbox_.post([done, result = std::move(result)]() mutable
                               { done(std::move(result)); });
                   return out;
               });
    }
    const ActionState &lane_state(int lane) const
    {
        return channel_[lane].state;
    }
    // Frame thread (from a call's callback): what the player just changed.
    void apply_settings(const SettingsView &value);
    void apply_installed(const std::vector<InstalledGame> &value);
    void clear_logs_local();

    // Screens opt in each frame (App::update clears them first).
    void set_fast(bool fast)
    {
        fast_.store(fast);
    }
    void want_logs(bool wanted)
    {
        want_logs_.store(wanted);
    }
    void want_installed(bool wanted)
    {
        want_installed_.store(wanted);
    }
    void refresh_now();

    // ---- queue ----
    void job_action(const std::string &job_id, const std::string &action);
    void clear_finished();
    void archive_password(const std::string &job_id, const std::string &password);
    void archive_choice(const std::string &job_id, int index);

    // ---- packages and links ----
    void open_package(const std::string &package_id);
    // service: "real-debrid", "torbox" or empty.
    void start_link(const std::string &package_id, int link_index, const std::string &service);
    // The same call, and `done` on the frame thread once it ended (after the
    // snapshot and link_action() show it), so a series can count its parts.
    void start_link(const std::string &package_id, int link_index, const std::string &service,
                    std::function<void(const LinkStart &)> done);
    void debrid_select(const std::string &selection_id, const std::vector<int> &indexes,
                       const std::string &service);
    // False (and nothing sent) while Pegasus is already resolving the link.
    bool cancel_capture();
    void dismiss_capture();
    void probe_link(const std::string &url);
    void queue_direct(const Probe &probe, const std::string &file_name);

    // ---- sources ----
    void source_add_url(const std::string &url);
    void source_delete(const std::string &id);
    void source_refresh(const std::string &id);
    void source_enable(const std::string &id, bool enabled);
    void refresh_defaults(bool force);
    void apply_defaults(const std::vector<std::string> &ids);
    // "Not now": Pegasus stops offering the onboarding list.
    void dismiss_defaults();
    // Reads the library again (after the player asked for a refresh).
    void reload_catalog();

    // ---- worker steps (public for tests) ----
    void poll_once();
    bool run_next_action();
    // Reads the catalogue if it is wanted. True when it did.
    bool load_catalog_once();

  private:
    struct Outcome
    {
        std::string error;
        std::string message;
        bool unreachable = false;
    };
    struct Command
    {
        int channel = 0;
        std::function<Outcome()> work;
    };
    struct Channel
    {
        ActionState state;
        int pending = 0; // frame thread: queued actions without a result yet
    };

    void submit(int channel, std::function<Outcome()> work);
    void worker();
    void catalog_worker();
    void post_failure(const std::string &error, bool unreachable);
    void post_jobs(std::vector<Job> jobs);
    void post_sources(std::vector<Source> sources);
    void post_capture(CaptureView view);
    Outcome jobs_outcome(Result<std::vector<Job>> &&result);
    Outcome sources_outcome(Result<std::vector<Source>> &&result);
    Outcome link_outcome(Result<LinkResult> &&result);
    void post_defaults(DefaultSources defaults);
    void post_tiers(Result<std::vector<std::uint8_t>> &&tiers);
    void note_sources(const std::vector<Source> &sources);
    void poll_capture();
    void track_session(const CaptureView &view);

    Backend &backend_;
    Inbox inbox_;
    Snapshot state_;
    Channel channel_[7];

    // Shared with the workers.
    Mutex mutex_;
    Signal signal_;
    std::deque<Command> commands_;
    bool poll_requested_ = true;
    bool stopping_ = false;
    std::string capture_session_; // under mutex_: the session being watched
    std::atomic<bool> fast_{false};
    std::atomic<bool> want_logs_{false};
    std::atomic<bool> want_installed_{false};
    std::atomic<bool> slow_requested_{false};
    std::atomic<int> active_jobs_{0};
    std::atomic<bool> catalog_failed_{false};
    std::atomic<bool> catalog_loaded_{false};
    std::atomic<bool> installed_dirty_{false}; // a download completed: read /appdb again
    Thread thread_;

    Mutex catalog_mutex_;
    Signal catalog_signal_;
    bool catalog_stopping_ = false;
    std::string wanted_key_ = "?";  // what the sources look like now
    std::string loaded_key_ = "!";  // what the catalogue was last read for
    Thread catalog_thread_;

    // Main-worker only.
    int slow_countdown_ = 0; // polls until the slow lists are read again
    std::int64_t log_since_ = 0;
    bool installed_fetched_ = false;
    bool defaults_fetched_ = false;
    bool capture_recovered_ = false;
    int tiers_ready_bits_ = 0; // debrid accounts the grid's tiers were computed for
    std::set<std::string> seen_completed_;
};

} // namespace orbit
