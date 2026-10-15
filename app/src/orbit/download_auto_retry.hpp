// Wings TV app - Reconnect downloads that dropped, the way Orbit does.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// At full speed (many connections) debrid hosts sometimes reset a connection and
// Pegasus DL marks the job failed. Pressing Retry carries on from the partial
// file. This does that by itself: a job that failed with a connection-type error
// and can resume is retried after 10, 20, then 40 seconds (Orbit's 5 s << attempt).
// Three tries in a row without new bytes and it stays failed. New bytes after a
// retry reset the count, so a long download that drops now and then keeps going.
// Real errors (not found, disk full, size changed, ...), and jobs the player
// paused or cancelled, are never retried.
//
// DownloadAutoRetry is the pure model (time is passed in, tested on its own).
// download_reconnect() is the one shared copy: the Store worker feeds it every
// queue poll (PegasusBackend::queue), the Downloads screen reads its row text.

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// The HTTP status inside Pegasus' error text ("status=503", "returned status 404",
// "HTTP/1.1 502"), or 0 when there is none.
int download_error_http_code(std::string_view text);

// True when the failure is a dropped / reset / timed-out connection or a busy
// server (429, 5xx) that a retry may get past. False for answers that will not
// change: not found, refused, disk full, size or ETag changed, cancelled, ...
bool transient_download_error(std::string_view error, std::string_view detail);

// What the model needs from one queue entry.
struct RetryJobSnapshot
{
    std::string id;
    std::string status; // orbit status names: "failed", "downloading", "paused", ...
    std::string error;
    std::string detail;
    double received = 0.0;  // bytes done
    bool resumable = false; // Pegasus says it can carry on from the partial file
};

class DownloadAutoRetry
{
public:
    static constexpr int kMaxTries = 3;
    static constexpr double kReconnectedShown = 5.0; // seconds "Reconnected" stays on the row
    static constexpr double kStuckAfter = 30.0;      // a sent retry still "failed" this long: a new failure

    // 10, 20, 40 seconds for try 1, 2, 3.
    static double backoff_seconds(int attempt);

    struct View
    {
        enum class Kind
        {
            none,
            waiting,      // failed, retry due in `seconds`
            reconnecting, // retry sent, no new bytes yet
            reconnected,  // new bytes after a retry, shown briefly
        };
        Kind kind = Kind::none;
        int seconds = 0;
        int attempt = 0;
        int max = kMaxTries;
    };

    void set_enabled(bool on);
    bool enabled() const
    {
        return enabled_;
    }

    // One queue poll. Returns the ids whose retry is due now; the caller sends
    // them and reports each with sent().
    std::vector<std::string> observe(const std::vector<RetryJobSnapshot> &jobs, double now);
    // The retry request went out (ok) or Pegasus refused it.
    void sent(const std::string &id, bool ok, double now);
    // The player asked to stop reconnecting this one: it stays failed.
    void stop(const std::string &id);

    View view(const std::string &id, double now) const;

    // [WINGS-RETRY] lines written since the last call.
    std::vector<std::string> take_log();

private:
    struct Track
    {
        bool seen_live = false; // seen running: only failures Wings watched happen are retried
        bool handled = false;   // the current failure has been decided
        bool gave_up = false;
        bool in_flight = false; // retry sent, waiting for new bytes
        int tries = 0;          // tries in a row without new bytes
        double due = -1.0;      // when the next retry goes out; negative: none waiting
        double mark = 0.0;      // bytes when the retry went out
        double sent_at = 0.0;
        double high = 0.0;      // most bytes seen
        double reconnected_until = -1.0;
    };
    void say(const std::string &line);

    std::map<std::string, Track> tracks_;
    std::vector<std::string> log_;
    bool enabled_ = true;
};

// The row text, or "" for View::Kind::none. Short and plain.
std::string auto_retry_text(const DownloadAutoRetry::View &view);

// The shared, locked copy (Store worker writes, UI thread reads). Time is the
// steady clock. Log lines go to stdout (the title log) unless a sink is set.
class DownloadReconnect
{
public:
    void set_enabled(bool on);
    bool enabled() const;
    std::vector<std::string> observe(const std::vector<RetryJobSnapshot> &jobs);
    void sent(const std::string &id, bool ok, const std::string &error);
    void stop(const std::string &id);
    DownloadAutoRetry::View view(const std::string &id) const;
    void set_log(std::function<void(const std::string &)> sink);

private:
    void flush();

    mutable std::mutex mutex_;
    DownloadAutoRetry model_;
    std::function<void(const std::string &)> sink_;
};

DownloadReconnect &download_reconnect();

} // namespace orbit
