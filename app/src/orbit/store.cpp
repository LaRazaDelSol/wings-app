// Wings TV app - The app's view of Pegasus DL, kept in sync on worker threads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Orbit Store's store.cpp (GPL-3.0-or-later): the worker loop,
// inbox posting and serial counters are kept; everything Orbit-specific
// (starter, restart, pairing, updates, Zero, Library) is gone.

#include "orbit/store.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace orbit
{

namespace
{
// Slow lists (sources, settings) are re-read every this many polls.
constexpr int kSlowEvery = 10;
constexpr std::size_t kMaxLogLines = 400;
constexpr double kIdlePoll = 3.0;
constexpr double kActivePoll = 1.0;
// How long the catalogue worker waits before it tries a failed read again.
constexpr double kCatalogRetry = 8.0;

enum Chan
{
    chan_jobs = 0,
    chan_link = 1,
    chan_source = 2,
    chan_probe = 3,
    chan_none = -1,
};
} // namespace

const Package *Snapshot::package(std::string_view id) const
{
    for (const Package &package : packages)
    {
        if (package.id == id)
            return &package;
    }
    return nullptr;
}

const Job *Snapshot::job(std::string_view id) const
{
    for (const Job &job : jobs)
    {
        if (job.id == id)
            return &job;
    }
    return nullptr;
}

Store::Store(Backend &backend) : backend_(backend)
{
    state_.endpoint = backend.endpoint();
}

Store::~Store()
{
    stop();
}

void Store::start()
{
    thread_.start([this] { worker(); });
    // The catalogue parse is shallow (one package at a time) but a stack of its own is cheap.
    catalog_thread_.start([this] { catalog_worker(); });
}

void Store::stop()
{
    {
        Lock lock(mutex_);
        stopping_ = true;
    }
    signal_.notify();
    {
        Lock lock(catalog_mutex_);
        catalog_stopping_ = true;
    }
    catalog_signal_.notify();
    thread_.join();
    catalog_thread_.join();
}

void Store::tick()
{
    inbox_.drain();
}

void Store::apply_settings(const SettingsView &value)
{
    state_.settings = value;
    state_.have_settings = true;
    ++state_.serial;
}

void Store::ensure_default_download_folder(const std::string &folder)
{
    const FolderNeed need = folder_need(state_.have_settings, state_.settings.download_root,
                                        default_folder_tried_);
    if (!need.set_default)
        return;
    default_folder_tried_ = true;
    submit(-1,
           [this, folder]
           {
               Outcome out;
               std::string failure;
               const Result<bool> made = backend_.mkdir(folder);
               // 409: the folder is already there, which is all we need.
               if (!made.ok() && made.status != 409)
                   failure = made.error.empty() ? "The default download folder could not be created."
                                                : made.error;
               if (failure.empty())
               {
                   SettingsPatch patch;
                   patch.has_root = true;
                   patch.root = folder;
                   Result<SettingsView> updated = backend_.settings_update(patch);
                   if (updated.ok())
                   {
                       inbox_.post([this, value = std::move(updated.value)]() mutable
                                   { apply_settings(value); });
                       return out;
                   }
                   failure = updated.error.empty() ? "The default download folder was refused."
                                                   : updated.error;
               }
               inbox_.post([this, failure] { default_folder_status_ = failure; });
               return out;
           });
}

void Store::apply_installed(const std::vector<InstalledGame> &value)
{
    state_.installed = value;
    state_.have_installed = true;
    ++state_.installed_serial;
    ++state_.serial;
}

void Store::clear_logs_local()
{
    state_.logs.clear();
    ++state_.serial;
}

void Store::refresh_now()
{
    {
        Lock lock(mutex_);
        poll_requested_ = true;
        slow_requested_.store(true);
    }
    signal_.notify();
}

// ---- posting results to the frame thread ----

void Store::post_failure(const std::string &error, bool unreachable)
{
    inbox_.post(
        [this, error, unreachable]
        {
            state_.link_error = error;
            if (unreachable)
            {
                if (state_.link != Link::offline)
                    state_.offline_since = now_seconds();
                state_.link = Link::offline;
            }
            ++state_.serial;
        });
}

void Store::post_jobs(std::vector<Job> jobs)
{
    int active = 0;
    for (const Job &job : jobs)
    {
        active += job_active(job) ? 1 : 0;
        // A download that finished may have put a game on the console: the
        // installed list (Store tiles, Library) is read again.
        if (job.status == "completed" && seen_completed_.insert(job.id).second)
            installed_dirty_.store(true);
    }
    if (seen_completed_.size() > 512)
        seen_completed_.clear();
    active_jobs_.store(active);
    inbox_.post(
        [this, jobs = std::move(jobs)]() mutable
        {
            state_.jobs = std::move(jobs);
            state_.have_queue = true;
            ++state_.serial;
        });
}

void Store::post_sources(std::vector<Source> sources)
{
    note_sources(sources);
    inbox_.post(
        [this, value = std::move(sources)]() mutable
        {
            state_.sources = std::move(value);
            state_.have_sources = true;
            ++state_.serial;
        });
}

void Store::post_capture(CaptureView view)
{
    inbox_.post(
        [this, view = std::move(view)]() mutable
        {
            state_.capture = std::move(view);
            ++state_.capture_serial;
            ++state_.serial;
        });
}

void Store::post_defaults(DefaultSources defaults)
{
    inbox_.post(
        [this, value = std::move(defaults)]() mutable
        {
            state_.defaults = std::move(value);
            state_.have_defaults = true;
            ++state_.serial;
        });
}

void Store::note_sources(const std::vector<Source> &sources)
{
    std::string key;
    for (const Source &s : sources)
    {
        key += s.id;
        key += s.enabled ? "+" : "-";
        key += std::to_string(s.last_refresh_at);
        key += ':';
        key += std::to_string(s.package_count);
        key += ';';
    }
    if (key.empty())
        key = "none";
    {
        Lock lock(catalog_mutex_);
        wanted_key_ = std::move(key);
    }
    catalog_signal_.notify();
}

// ---- polling ----

void Store::track_session(const CaptureView &view)
{
    Lock lock(mutex_);
    capture_session_ = view.terminal() ? std::string() : view.session_id;
}

void Store::poll_capture()
{
    std::string session;
    {
        Lock lock(mutex_);
        session = capture_session_;
    }
    if (session.empty())
        return;
    Result<CaptureView> status = backend_.capture_status(session);
    if (!status.ok())
    {
        // A missed poll is not a failed capture; try again next time.
        if (status.unreachable())
            post_failure(status.error, true);
        return;
    }
    CaptureView view = std::move(status.value);
    if (view.session_id.empty())
        view.session_id = session;
    const bool done = view.terminal();
    if (done)
    {
        Lock lock(mutex_);
        capture_session_.clear();
    }
    if (view.state == "completed" && view.result.has_jobs)
        post_jobs(view.result.jobs);
    else if (view.state == "completed")
    {
        Lock lock(mutex_);
        poll_requested_ = true;
    }
    post_capture(std::move(view));
}

void Store::poll_once()
{
    // Tier A, every poll: is Pegasus there, and what is it doing.
    Result<PegasusInfo> health = backend_.health();
    if (!health.ok())
    {
        post_failure(health.error, health.unreachable());
        // Re-read the slow lists as soon as Pegasus is back.
        slow_countdown_ = 0;
        capture_recovered_ = false;
        defaults_fetched_ = false;
        return;
    }
    const bool supported = health.value.supported;
    inbox_.post(
        [this, info = std::move(health.value)]() mutable
        {
            state_.info = std::move(info);
            state_.have_info = true;
            state_.link = Link::online;
            state_.link_error.clear();
            ++state_.serial;
        });
    if (!supported)
        return; // reachable, but a version Wings has no adapter for: nothing more to ask

    Result<std::vector<Job>> queue = backend_.queue();
    if (queue.ok())
        post_jobs(std::move(queue.value));
    else
        post_failure(queue.error, queue.unreachable());

    // A browser capture in flight: watch it, or pick one up that started earlier.
    poll_capture();
    if (!capture_recovered_)
    {
        capture_recovered_ = true;
        bool have_session = false;
        {
            Lock lock(mutex_);
            have_session = !capture_session_.empty();
        }
        if (!have_session)
        {
            Result<CaptureView> active = backend_.capture_active();
            if (active.ok() && active.value.active && !active.value.session_id.empty() &&
                active.value.session_id != "0")
            {
                track_session(active.value);
                post_capture(std::move(active.value));
            }
        }
    }

    // Tier B, only while a screen asks: new log lines.
    if (want_logs_.load())
    {
        Result<std::vector<LogLine>> logs = backend_.logs(log_since_);
        if (logs.ok() && !logs.value.empty())
        {
            log_since_ = logs.value.back().seq;
            inbox_.post(
                [this, lines = std::move(logs.value)]() mutable
                {
                    for (LogLine &line : lines)
                        state_.logs.push_back(std::move(line));
                    if (state_.logs.size() > kMaxLogLines)
                        state_.logs.erase(state_.logs.begin(),
                                          state_.logs.end() - static_cast<long>(kMaxLogLines));
                    state_.log_seq = state_.logs.back().seq;
                    ++state_.serial;
                });
        }
    }

    // Tier C: the game database, only when the Library wants it.
    const bool slow_due = slow_requested_.exchange(false) || slow_countdown_ <= 0;
    if (want_installed_.load() && (!installed_fetched_ || slow_due || installed_dirty_.load()))
    {
        installed_dirty_.store(false);
        Result<std::vector<InstalledGame>> installed = backend_.installed();
        if (installed.ok())
        {
            installed_fetched_ = true;
            inbox_.post(
                [this, value = std::move(installed.value)]() mutable
                {
                    state_.installed = std::move(value);
                    state_.have_installed = true;
                    ++state_.installed_serial;
                    ++state_.serial;
                });
        }
    }

    // Tier C, now and then: sources and settings (the catalogue follows the sources).
    if (!slow_due)
    {
        --slow_countdown_;
        return;
    }
    slow_countdown_ = kSlowEvery;
    Result<std::vector<Source>> sources = backend_.sources();
    if (sources.ok())
    {
        post_sources(std::move(sources.value));
    }
    else
    {
        // No source list (route missing?): still read the catalogue once.
        bool first = false;
        {
            Lock lock(catalog_mutex_);
            first = wanted_key_ == "?";
            if (first)
                wanted_key_ = "none";
        }
        if (first)
            catalog_signal_.notify();
    }
    Result<SettingsView> settings = backend_.settings();
    if (settings.ok())
    {
        const int bits = (settings.value.real_debrid_ready ? 1 : 0) |
                         (settings.value.torbox_ready ? 2 : 0);
        inbox_.post(
            [this, value = std::move(settings.value)]() mutable
            {
                state_.settings = std::move(value);
                state_.have_settings = true;
                ++state_.serial;
            });
        // A debrid account became (or stopped being) usable: the best tier of
        // the packages changes with it. Before the catalogue is read, its own
        // load classifies with the settings the backend then has.
        if (bits != tiers_ready_bits_)
        {
            tiers_ready_bits_ = bits;
            if (catalog_loaded_.load())
                post_tiers(backend_.package_tiers());
        }
    }
    if (!defaults_fetched_)
    {
        Result<DefaultSources> defaults = backend_.default_sources(false);
        if (defaults.ok())
        {
            defaults_fetched_ = true;
            post_defaults(std::move(defaults.value));
        }
    }
}

// ---- the catalogue ----

bool Store::load_catalog_once()
{
    std::string key;
    {
        Lock lock(catalog_mutex_);
        if (wanted_key_ == loaded_key_ || wanted_key_ == "?")
            return false; // nothing changed, or the sources are not known yet
        key = wanted_key_;
    }
    inbox_.post(
        [this]
        {
            state_.catalog_loading = true;
            ++state_.serial;
        });
    Result<std::vector<Package>> library = backend_.library();
    if (!library.ok())
    {
        catalog_failed_.store(true);
        if (library.unreachable())
            post_failure(library.error, true);
        const std::string error = library.error;
        inbox_.post(
            [this, error]
            {
                state_.catalog_loading = false;
                state_.catalog_error = error;
                ++state_.serial;
            });
        return true;
    }
    catalog_failed_.store(false);
    {
        Lock lock(catalog_mutex_);
        loaded_key_ = key;
    }
    // Classified here, on the catalogue thread, and delivered with the packages
    // so the grid never shows a list whose tiers belong to another one.
    Result<std::vector<std::uint8_t>> tiers = backend_.package_tiers();
    std::vector<std::uint8_t> tier_values;
    if (tiers.ok() && tiers.value.size() == library.value.size())
        tier_values = std::move(tiers.value);
    catalog_loaded_.store(true);
    inbox_.post(
        [this, value = std::move(library.value), tier_values = std::move(tier_values)]() mutable
        {
            state_.packages = std::move(value);
            state_.tiers = std::move(tier_values);
            ++state_.tiers_serial;
            state_.have_catalog = true;
            state_.catalog_loading = false;
            state_.catalog_error.clear();
            ++state_.catalog_serial;
            ++state_.serial;
        });
    return true;
}

void Store::post_tiers(Result<std::vector<std::uint8_t>> &&tiers)
{
    if (!tiers.ok())
        return;
    inbox_.post(
        [this, value = std::move(tiers.value)]() mutable
        {
            // Only for the catalogue the frame thread holds now.
            if (value.size() != state_.packages.size())
                return;
            state_.tiers = std::move(value);
            ++state_.tiers_serial;
            ++state_.serial;
        });
}

void Store::reload_catalog()
{
    {
        Lock lock(catalog_mutex_);
        loaded_key_ = "!";
    }
    catalog_signal_.notify();
}

void Store::catalog_worker()
{
    for (;;)
    {
        {
            Lock lock(catalog_mutex_);
            for (;;)
            {
                if (catalog_stopping_)
                    return;
                if (wanted_key_ != loaded_key_ && wanted_key_ != "?")
                    break;
                catalog_signal_.wait(catalog_mutex_, 30.0);
            }
        }
        // Only read while Pegasus is up: a failed read waits before it retries.
        load_catalog_once();
        if (catalog_failed_.load())
        {
            Lock lock(catalog_mutex_);
            if (!catalog_stopping_)
                catalog_signal_.wait(catalog_mutex_, kCatalogRetry);
        }
    }
}

// ---- commands ----

void Store::submit(int channel, std::function<Outcome()> work)
{
    if (channel >= 0)
    {
        Channel &c = channel_[channel];
        ++c.pending;
        c.state.busy = true;
        c.state.error.clear();
        c.state.message.clear();
    }
    {
        Lock lock(mutex_);
        commands_.push_back({channel, std::move(work)});
    }
    signal_.notify();
}

Store::Outcome Store::jobs_outcome(Result<std::vector<Job>> &&result)
{
    Outcome out;
    if (result.ok())
        post_jobs(std::move(result.value));
    out.error = result.error;
    out.unreachable = result.unreachable();
    return out;
}

Store::Outcome Store::sources_outcome(Result<std::vector<Source>> &&result)
{
    Outcome out;
    if (result.ok())
        post_sources(std::move(result.value));
    out.error = result.error;
    out.unreachable = result.unreachable();
    return out;
}

Store::Outcome Store::link_outcome(Result<LinkResult> &&result)
{
    Outcome out;
    out.error = result.error;
    out.unreachable = result.unreachable();
    if (!result.ok())
        return out;
    LinkResult value = std::move(result.value);
    if (value.kind == LinkResult::Kind::capture)
    {
        track_session(value.capture);
        post_capture(value.capture);
    }
    else if (value.kind == LinkResult::Kind::dispatched)
    {
        if (value.dispatch.has_jobs)
        {
            post_jobs(value.dispatch.jobs);
        }
        else
        {
            Lock lock(mutex_);
            poll_requested_ = true;
        }
        out.message = value.dispatch.toast;
    }
    else
    {
        out.message = value.message;
    }
    inbox_.post(
        [this, value = std::move(value)]() mutable
        {
            state_.link_result = std::move(value);
            ++state_.link_serial;
            ++state_.serial;
        });
    return out;
}

void Store::job_action(const std::string &job_id, const std::string &action)
{
    submit(chan_jobs,
           [this, job_id, action] { return jobs_outcome(backend_.job_action(job_id, action)); });
}

void Store::clear_finished()
{
    submit(chan_jobs, [this] { return jobs_outcome(backend_.clear_finished()); });
}

void Store::archive_password(const std::string &job_id, const std::string &password)
{
    submit(chan_jobs, [this, job_id, password]
           { return jobs_outcome(backend_.archive_password(job_id, password)); });
}

void Store::archive_choice(const std::string &job_id, int index)
{
    submit(chan_jobs,
           [this, job_id, index] { return jobs_outcome(backend_.archive_choice(job_id, index)); });
}

void Store::open_package(const std::string &package_id)
{
    submit(chan_none,
           [this, package_id]
           {
               Result<PackageDetail> detail = backend_.package(package_id);
               inbox_.post(
                   [this, detail = std::move(detail)]() mutable
                   {
                       // Best supported links first; the original order inside a tier.
                       sort_links_by_tier(detail.value.links);
                       state_.detail = std::move(detail.value);
                       state_.detail_error = detail.error;
                       ++state_.detail_serial;
                       ++state_.serial;
                   });
               return Outcome{};
           });
}

void Store::start_link(const std::string &package_id, int link_index, const std::string &service)
{
    submit(chan_link,
           [this, package_id, link_index, service]
           { return link_outcome(backend_.start_link(package_id, link_index, service)); });
}

void Store::start_link(const std::string &package_id, int link_index, const std::string &service,
                       std::function<void(const LinkStart &)> done)
{
    auto outcome = std::make_shared<LinkStart>();
    submit(chan_link,
           [this, package_id, link_index, service, outcome]
           {
               Result<LinkResult> result = backend_.start_link(package_id, link_index, service);
               outcome->error = result.error;
               outcome->kind = result.value.kind;
               outcome->message = result.value.message;
               return link_outcome(std::move(result));
           });
    // A second command posts after the first one's channel update: the callback
    // sees the snapshot and link_action() as they are once this link ended.
    submit(chan_none,
           [this, outcome, done = std::move(done)]
           {
               inbox_.post([outcome, done] { done(*outcome); });
               return Outcome{};
           });
}

void Store::debrid_select(const std::string &selection_id, const std::vector<int> &indexes,
                          const std::string &service)
{
    submit(chan_link,
           [this, selection_id, indexes, service]
           { return link_outcome(backend_.debrid_select(selection_id, indexes, service)); });
}

bool Store::cancel_capture()
{
    CaptureView view = state_.capture;
    if (!view.active && view.state.empty())
        return true;
    if (view.state == "queueing")
        return false; // Pegasus is already resolving the link: it cannot be stopped now
    if (view.terminal() || view.session_id.empty())
    {
        dismiss_capture();
        return true;
    }
    submit(chan_link,
           [this, view]() mutable
           {
               Outcome out;
               Result<bool> result = backend_.capture_cancel(view.session_id);
               out.error = result.error;
               out.unreachable = result.unreachable();
               {
                   Lock lock(mutex_);
                   capture_session_.clear();
               }
               view.state = "cancelled";
               view.message = tr("Browser capture cancelled.");
               post_capture(std::move(view));
               return out;
           });
    return true;
}

void Store::dismiss_capture()
{
    {
        Lock lock(mutex_);
        capture_session_.clear();
    }
    state_.capture = {};
    ++state_.capture_serial;
    ++state_.serial;
}

void Store::probe_link(const std::string &url)
{
    submit(chan_probe,
           [this, url]
           {
               Outcome out;
               Result<Probe> result = backend_.probe_link(url);
               out.unreachable = result.unreachable();
               out.error = result.error;
               if (result.ok() && !result.value.rejection.empty())
                   out.error = result.value.rejection;
               if (result.ok() && out.error.empty())
               {
                   inbox_.post(
                       [this, probe = std::move(result.value)]() mutable
                       {
                           state_.probe = std::move(probe);
                           ++state_.probe_serial;
                           ++state_.serial;
                       });
               }
               return out;
           });
}

void Store::queue_direct(const Probe &probe, const std::string &file_name)
{
    submit(chan_probe,
           [this, probe, file_name]
           {
               Outcome out;
               Result<Dispatch> result = backend_.queue_direct(probe, file_name);
               out.error = result.error;
               out.unreachable = result.unreachable();
               if (result.ok())
               {
                   if (result.value.has_jobs)
                   {
                       post_jobs(std::move(result.value.jobs));
                   }
                   else
                   {
                       Lock lock(mutex_);
                       poll_requested_ = true;
                   }
                   out.message = result.value.toast;
               }
               return out;
           });
}

void Store::source_add_url(const std::string &url)
{
    submit(chan_source,
           [this, url]
           {
               Outcome out = sources_outcome(backend_.source_add_url(url));
               if (out.error.empty())
                   out.message = "Source added";
               return out;
           });
}

void Store::source_delete(const std::string &id)
{
    submit(chan_source,
           [this, id]
           {
               Outcome out = sources_outcome(backend_.source_delete(id));
               if (out.error.empty())
                   out.message = "Source deleted";
               return out;
           });
}

void Store::source_refresh(const std::string &id)
{
    submit(chan_source,
           [this, id]
           {
               Outcome out = sources_outcome(backend_.source_refresh(id));
               if (out.error.empty())
                   out.message = "Source refreshed";
               return out;
           });
}

void Store::source_enable(const std::string &id, bool enabled)
{
    submit(chan_source,
           [this, id, enabled]
           {
               Outcome out = sources_outcome(backend_.source_enable(id, enabled));
               if (out.error.empty())
                   out.message = enabled ? "Source turned on" : "Source turned off";
               return out;
           });
}

void Store::refresh_defaults(bool force)
{
    submit(chan_source,
           [this, force]
           {
               Outcome out;
               Result<DefaultSources> result = backend_.default_sources(force);
               out.error = result.error;
               out.unreachable = result.unreachable();
               if (result.ok())
                   post_defaults(std::move(result.value));
               return out;
           });
}

void Store::apply_defaults(const std::vector<std::string> &ids)
{
    const std::int64_t revision = state_.defaults.revision;
    submit(chan_source,
           [this, ids, revision]
           {
               Outcome out;
               Result<std::vector<std::string>> result =
                   backend_.default_sources_apply(revision, ids);
               out.error = result.error;
               out.unreachable = result.unreachable();
               if (!result.ok())
                   return out;
               out.message = result.value.empty() ? "Sources added" : result.value.front();
               Result<std::vector<Source>> sources = backend_.sources();
               if (sources.ok())
                   post_sources(std::move(sources.value));
               Result<DefaultSources> defaults = backend_.default_sources(false);
               if (defaults.ok())
                   post_defaults(std::move(defaults.value));
               return out;
           });
}

void Store::dismiss_defaults()
{
    submit(chan_source,
           [this]
           {
               Outcome out;
               Result<bool> seen = backend_.default_sources_seen();
               out.error = seen.error;
               out.unreachable = seen.unreachable();
               Result<DefaultSources> defaults = backend_.default_sources(false);
               if (defaults.ok())
                   post_defaults(std::move(defaults.value));
               return out;
           });
}

bool Store::run_next_action()
{
    Command command;
    {
        Lock lock(mutex_);
        if (commands_.empty())
            return false;
        command = std::move(commands_.front());
        commands_.pop_front();
    }
    const Outcome outcome = command.work();
    if (outcome.unreachable)
        post_failure(outcome.error, true);
    const int channel = command.channel;
    inbox_.post(
        [this, channel, outcome]
        {
            if (channel >= 0)
            {
                Channel &c = channel_[channel];
                if (--c.pending <= 0)
                {
                    c.pending = 0;
                    c.state.busy = false;
                }
                c.state.error = outcome.error;
                c.state.message = outcome.message;
                ++c.state.serial;
            }
            ++state_.serial;
        });
    return true;
}

void Store::worker()
{
    double next_poll = 0.0;
    for (;;)
    {
        bool have_command = false;
        bool poll = false;
        {
            Lock lock(mutex_);
            for (;;)
            {
                if (stopping_)
                    return;
                have_command = !commands_.empty();
                poll = poll_requested_ || now_seconds() >= next_poll;
                if (have_command || poll)
                    break;
                signal_.wait(mutex_, next_poll - now_seconds());
            }
            if (!have_command)
                poll_requested_ = false;
        }
        if (have_command)
        {
            run_next_action();
            continue; // an action already refreshed what it changed
        }
        poll_once();
        bool capture = false;
        {
            Lock lock(mutex_);
            capture = !capture_session_.empty();
        }
        const bool busy = fast_.load() || active_jobs_.load() > 0 || capture;
        next_poll = now_seconds() + (busy ? kActivePoll : kIdlePoll);
    }
}

} // namespace orbit
