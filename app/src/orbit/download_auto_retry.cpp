// Wings TV app - Reconnect downloads that dropped (see download_auto_retry.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/download_auto_retry.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <set>

namespace orbit
{

namespace
{

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_any(const std::string &text, std::initializer_list<const char *> words)
{
    for (const char *word : words)
        if (text.find(word) != std::string::npos)
            return true;
    return false;
}

// Statuses the player chose: never retried.
bool player_stopped(const std::string &status)
{
    return status == "paused" || status == "pausing" || status == "cancelled" || status == "cancelling";
}

std::string fmt_bytes(double bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", bytes);
    return buf;
}

} // namespace

int download_error_http_code(std::string_view text)
{
    const std::string t = lower(text);
    // Shapes Pegasus DL 1.11.0 writes: "remote download failed: %s status=%d %s",
    // "HTTP server returned status %d", curl's "HTTP/1.1 503 ...".
    static const char *const kLeads[] = {"status=", "status:", "status ", "http/1.1 ", "http/1.0 ", "http/2 ",
                                         "http "};
    for (const char *lead : kLeads)
    {
        const std::string l(lead);
        std::size_t at = t.find(l);
        while (at != std::string::npos)
        {
            std::size_t i = at + l.size();
            while (i < t.size() && t[i] == ' ')
                ++i;
            std::size_t j = i;
            while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j])))
                ++j;
            if (j - i == 3)
            {
                const int code = std::stoi(t.substr(i, 3));
                if (code >= 100 && code <= 599)
                    return code;
            }
            at = t.find(l, at + 1);
        }
    }
    return 0;
}

bool transient_download_error(std::string_view error, std::string_view detail)
{
    std::string text = lower(error);
    if (!detail.empty())
        text += " | " + lower(detail);
    if (text.find_first_not_of(" |") == std::string::npos)
        return false; // nothing to go on
    // Answers that will not change (Orbit: 404, disk full, size changed, ETag mismatch).
    if (has_any(text, {"cancel", "not found", "disk full", "no space", "not enough free", "free space",
                       "storage allocation", "size changed", "etag", "mismatch", "byte range",
                       "range not satisfiable", "support byte ranges", "unable to resume", "not resumable",
                       "expired", "password", "install", "invalid", "unsupported", "not supported",
                       "forbidden", "unauthorized", "permission", "checksum", "corrupt"}))
        return false;
    const int code = download_error_http_code(text);
    if (code == 408 || code == 429 || code == 500 || code == 502 || code == 503 || code == 504)
        return true;
    if (code != 0 && code != 206 && code != 200)
        return false; // 400, 401, 403, 404, 410, 416, ...
    // Dropped, reset, timed out or stalled connections (curl and Pegasus wording).
    return has_any(text, {"timed out", "timeout", "connection reset", "reset by peer", "connection died",
                          "connection closed", "closed abruptly", "peer closed", "transfer closed",
                          "partial file", "recv failure", "failure when receiving", "send failure",
                          "server returned nothing", "couldn't connect", "could not connect",
                          "failed to connect", "connection refused", "couldn't resolve", "could not resolve",
                          "too slow", "stall", "gateway", "service unavailable", "temporarily", "try again",
                          "segmented download failed", "range download failed", "download failed",
                          "network"});
}

double DownloadAutoRetry::backoff_seconds(int attempt)
{
    if (attempt < 1)
        attempt = 1;
    if (attempt > kMaxTries)
        attempt = kMaxTries;
    return static_cast<double>(5 << attempt); // 10, 20, 40
}

void DownloadAutoRetry::say(const std::string &line)
{
    log_.push_back("[WINGS-RETRY] " + line);
    if (log_.size() > 64)
        log_.erase(log_.begin());
}

void DownloadAutoRetry::set_enabled(bool on)
{
    if (enabled_ == on)
        return;
    enabled_ = on;
    say(on ? "auto-reconnect on" : "auto-reconnect off");
    if (!on)
        for (auto &entry : tracks_)
        {
            entry.second.due = -1.0;
            entry.second.in_flight = false;
        }
}

std::vector<std::string> DownloadAutoRetry::observe(const std::vector<RetryJobSnapshot> &jobs, double now)
{
    std::vector<std::string> due;
    std::set<std::string> present;
    for (const RetryJobSnapshot &job : jobs)
    {
        present.insert(job.id);
        if (job.status == "completed")
        {
            tracks_.erase(job.id);
            continue;
        }
        Track &t = tracks_[job.id];
        if (player_stopped(job.status))
        {
            if (t.due >= 0.0 || t.in_flight)
                say("job " + job.id + ": " + job.status + " by the player, not reconnecting");
            const bool live = job.status == "paused" || job.status == "pausing";
            t = Track{};
            t.seen_live = live; // a paused job that fails after Resume may still be retried
            continue;
        }
        if (job.status == "failed")
        {
            if (t.due >= 0.0)
            {
                if (!enabled_)
                {
                    t.due = -1.0;
                    continue;
                }
                if (now >= t.due)
                {
                    t.due = -1.0;
                    t.in_flight = true;
                    t.sent_at = now;
                    t.mark = std::max(t.high, job.received);
                    due.push_back(job.id);
                    say("job " + job.id + ": retrying now (try " + std::to_string(t.tries) + " of " +
                        std::to_string(kMaxTries) + ", " + fmt_bytes(t.mark) + " bytes kept)");
                }
                continue;
            }
            // A retry that went out but never got the job moving counts as a failed try.
            if (t.in_flight && now - t.sent_at > kStuckAfter)
                t.handled = false;
            if (t.handled)
                continue;
            t.handled = true;
            t.in_flight = false;
            t.high = std::max(t.high, job.received);
            if (!t.seen_live)
                continue; // failed before Wings watched it run
            const std::string why = job.error.empty() ? job.detail : job.error;
            if (!enabled_)
            {
                say("job " + job.id + ": failed (" + why + "); auto-reconnect is off");
                continue;
            }
            if (!transient_download_error(job.error, job.detail))
            {
                say("job " + job.id + ": real error, not retrying: " + why);
                continue;
            }
            if (!job.resumable)
            {
                say("job " + job.id + ": dropped but cannot resume, not retrying: " + why);
                continue;
            }
            if (t.tries >= kMaxTries)
            {
                t.gave_up = true;
                say("job " + job.id + ": gave up after " + std::to_string(kMaxTries) +
                    " tries without progress: " + why);
                continue;
            }
            ++t.tries;
            t.due = now + backoff_seconds(t.tries);
            say("job " + job.id + ": dropped (" + why + "), reconnecting in " +
                std::to_string(static_cast<int>(backoff_seconds(t.tries))) + " s (try " +
                std::to_string(t.tries) + " of " + std::to_string(kMaxTries) + ")");
            continue;
        }
        // Queued, starting, downloading, merging, ...
        if (t.gave_up)
            t.tries = 0; // the player pressed Retry after Wings gave up: a fresh start
        t.gave_up = false;
        t.seen_live = true;
        t.handled = false;
        if (t.due >= 0.0)
        {
            // Retried by hand while Wings was waiting.
            t.due = -1.0;
            t.in_flight = true;
            t.mark = std::max(t.high, job.received);
        }
        if (t.in_flight && job.received > t.mark)
        {
            t.in_flight = false;
            t.tries = 0;
            t.reconnected_until = now + kReconnectedShown;
            say("job " + job.id + ": reconnected, moving again at " + fmt_bytes(job.received) + " bytes");
        }
        t.high = std::max(t.high, job.received);
    }
    // Entries cleared from Pegasus' list.
    for (auto it = tracks_.begin(); it != tracks_.end();)
        it = present.count(it->first) != 0 ? std::next(it) : tracks_.erase(it);
    return due;
}

void DownloadAutoRetry::sent(const std::string &id, bool ok, double now)
{
    (void)now;
    const auto it = tracks_.find(id);
    if (it == tracks_.end())
        return;
    if (!ok)
    {
        // Pegasus refused the retry: decide again on the next poll (it counts as a try).
        it->second.in_flight = false;
        it->second.handled = false;
    }
}

void DownloadAutoRetry::stop(const std::string &id)
{
    const auto it = tracks_.find(id);
    if (it == tracks_.end())
        return;
    if (it->second.due >= 0.0 || it->second.in_flight)
        say("job " + id + ": the player stopped reconnecting");
    it->second.due = -1.0;
    it->second.in_flight = false;
    it->second.handled = true;
    it->second.gave_up = true;
}

DownloadAutoRetry::View DownloadAutoRetry::view(const std::string &id, double now) const
{
    View v;
    const auto it = tracks_.find(id);
    if (it == tracks_.end())
        return v;
    const Track &t = it->second;
    v.attempt = t.tries;
    if (t.due >= 0.0)
    {
        v.kind = View::Kind::waiting;
        v.seconds = std::max(0, static_cast<int>(std::ceil(t.due - now)));
    }
    else if (t.in_flight)
        v.kind = View::Kind::reconnecting;
    else if (t.reconnected_until > now)
        v.kind = View::Kind::reconnected;
    return v;
}

std::vector<std::string> DownloadAutoRetry::take_log()
{
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

std::string auto_retry_text(const DownloadAutoRetry::View &view)
{
    using Kind = DownloadAutoRetry::View::Kind;
    const std::string n = std::to_string(view.attempt);
    const std::string max = std::to_string(view.max);
    switch (view.kind)
    {
    case Kind::waiting:
        if (view.seconds <= 0)
            return tr("Reconnecting now (try {n} of {max})", {{"n", n}, {"max", max}});
        return tr("Reconnecting in {s} s (try {n} of {max})",
                  {{"s", std::to_string(view.seconds)}, {"n", n}, {"max", max}});
    case Kind::reconnecting:
        return tr("Reconnecting (try {n} of {max})", {{"n", n}, {"max", max}});
    case Kind::reconnected:
        return tr("Reconnected");
    case Kind::none:
        break;
    }
    return std::string();
}

// ------------------------------------------------------------- shared copy --

namespace
{

double steady_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void DownloadReconnect::set_enabled(bool on)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.set_enabled(on);
    flush();
}

bool DownloadReconnect::enabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return model_.enabled();
}

std::vector<std::string> DownloadReconnect::observe(const std::vector<RetryJobSnapshot> &jobs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> due = model_.observe(jobs, steady_seconds());
    flush();
    return due;
}

void DownloadReconnect::sent(const std::string &id, bool ok, const std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.sent(id, ok, steady_seconds());
    if (!ok)
    {
        const std::string line = "[WINGS-RETRY] job " + id + ": Pegasus refused the retry: " + error;
        if (sink_)
            sink_(line);
        else
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    }
    flush();
}

void DownloadReconnect::stop(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.stop(id);
    flush();
}

DownloadAutoRetry::View DownloadReconnect::view(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return model_.view(id, steady_seconds());
}

void DownloadReconnect::set_log(std::function<void(const std::string &)> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void DownloadReconnect::flush()
{
    for (const std::string &line : model_.take_log())
    {
        if (sink_)
            sink_(line);
        else
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    }
}

DownloadReconnect &download_reconnect()
{
    static DownloadReconnect instance;
    return instance;
}

} // namespace orbit
