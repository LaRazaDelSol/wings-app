// Wings TV app - After a download finishes: ask ShadowMount to scan, and say what happened.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/shadowmount_scan.hpp"

#include <algorithm>

namespace orbit
{

void ThreadRunner::operator()(std::function<void()> job)
{
    thread_.join(); // the previous job has finished: the caller never overlaps two
    std::function<void()> again = job;
    if (!thread_.start(std::move(job)))
        again();
}

void AfterInstall::update(double now, const std::vector<Job> &jobs, bool have_queue)
{
    collect(now);
    if (have_queue)
        observe(now, jobs);
    if (!in_flight_)
    {
        if (due_ >= 0.0 && now >= due_)
        {
            start_scan(now);
        }
        else if (!watch_.empty() &&
                 (banner_.phase == ScanPhase::scanning || banner_.phase == ScanPhase::registered))
        {
            if (now >= poll_deadline_)
            {
                if (banner_.phase == ScanPhase::scanning)
                    banner_.phase = ScanPhase::timed_out;
                watch_.clear();
            }
            else if (now >= next_poll_)
            {
                start_games();
            }
        }
    }
    // A client that answers at once (tests) has its result ready in this same frame.
    collect(now);
}

void AfterInstall::observe(double now, const std::vector<Job> &jobs)
{
    const bool acting = seeded_ && mode_ == AfterDownload::scan && client_.wired();
    for (const Job &job : jobs)
    {
        const auto previous = last_status_.find(job.id);
        // Only a job that was seen unfinished counts: after a reconnect the queue lists old
        // completed jobs as new ids, and those must not start a scan.
        const bool was_open = previous != last_status_.end() && previous->second != "completed" &&
                              previous->second != "failed" && previous->second != "cancelled";
        if (acting && job.status == "completed" && was_open && handled_.insert(job.id).second)
        {
            pending_.push_back({job.title_id, job.name});
            if (due_ < 0.0 && !in_flight_)
                due_ = now + kScanDebounceS;
        }
        last_status_[job.id] = job.status;
    }
    seeded_ = true;
}

void AfterInstall::scan_now(double now)
{
    if (!in_flight_ && due_ < 0.0)
        due_ = now;
}

void AfterInstall::dismiss()
{
    banner_ = ScanBanner{};
    watch_.clear();
    // A scan request still out: its answer must not bring the banner back.
    drop_scan_ = in_flight_ && !games_in_flight_;
}

void AfterInstall::start_scan(double now)
{
    (void)now;
    for (const Watched &item : pending_)
        watch_.push_back(item);
    pending_.clear();
    due_ = -1.0;
    in_flight_ = true;
    banner_ = ScanBanner{};
    banner_.phase = ScanPhase::asking;
    ++scans_sent_;
    const std::shared_ptr<Shared> shared = shared_;
    ShadowMountClient client = client_;
    client_.run(
        [shared, client]() mutable
        {
            const VersionReply version = client.version();
            ScanReply scan;
            if (version.ok)
                scan = client.scan();
            Lock lock(shared->mutex);
            shared->version = version;
            shared->scan = scan;
            shared->scan_ready = true;
        });
}

void AfterInstall::start_games()
{
    in_flight_ = true;
    games_in_flight_ = true;
    const std::shared_ptr<Shared> shared = shared_;
    ShadowMountClient client = client_;
    client_.run(
        [shared, client]() mutable
        {
            const GamesReply games = client.games();
            Lock lock(shared->mutex);
            shared->games = games;
            shared->games_ready = true;
        });
}

void AfterInstall::note_registered(const std::set<std::string> &ids)
{
    for (auto it = watch_.begin(); it != watch_.end();)
    {
        if (!it->title_id.empty() && ids.count(it->title_id) != 0)
        {
            banner_.registered.push_back(it->name.empty() ? it->title_id : it->name);
            banner_.phase = ScanPhase::registered;
            it = watch_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void AfterInstall::collect(double now)
{
    bool have_scan = false;
    bool have_games = false;
    VersionReply version;
    ScanReply scan;
    GamesReply games;
    {
        Lock lock(shared_->mutex);
        if (shared_->scan_ready)
        {
            have_scan = true;
            version = shared_->version;
            scan = shared_->scan;
            shared_->scan_ready = false;
        }
        if (shared_->games_ready)
        {
            have_games = true;
            games = shared_->games;
            shared_->games_ready = false;
        }
    }
    if (have_scan)
    {
        in_flight_ = false;
        if (drop_scan_)
        {
            drop_scan_ = false;
            watch_.clear();
        }
        else if (!version.ok || !scan.ok || (!scan.queued && !scan.deferred))
        {
            banner_ = ScanBanner{};
            banner_.phase = ScanPhase::unreachable;
            watch_.clear();
        }
        else if (scan.deferred)
        {
            banner_ = ScanBanner{};
            banner_.phase = ScanPhase::deferred;
            banner_.reason = scan.reason;
            watch_.clear();
        }
        else
        {
            banner_ = ScanBanner{};
            banner_.phase = ScanPhase::scanning;
            next_poll_ = now + kScanPollEveryS;
            poll_deadline_ = now + kScanPollForS;
        }
        // Completions that arrived while the request was out get their own request.
        if (!pending_.empty() && due_ < 0.0)
            due_ = now + kScanDebounceS;
    }
    if (have_games)
    {
        in_flight_ = false;
        games_in_flight_ = false;
        next_poll_ = now + kScanPollEveryS;
        if (games.ok && (banner_.phase == ScanPhase::scanning || banner_.phase == ScanPhase::registered))
            note_registered(games.title_ids);
    }
}

} // namespace orbit
