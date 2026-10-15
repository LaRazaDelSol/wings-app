// Wings TV app - TorBox "still preparing" status and the wait that forces it through (B-19).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// POST /api/debrid/queue with TorBox asks TorBox for the file and polls it every
// 2 s for up to 60 s. When TorBox has not finished fetching the file to its own
// server by then, Pegasus DL fails the call with
//   "TorBox is still preparing this download; try again shortly"
// and keeps nothing: no job, no progress. Wings recognises that answer, tells the
// player TorBox is preparing the file, and asks again by itself every 30 s for up
// to 10 minutes (Circle stops it at once).
//
// Pure: no I/O, time is passed in (seconds, any steady clock).

#pragma once

#include <string>
#include <string_view>

namespace orbit
{

// The TorBox page that lists what TorBox is preparing for the player.
constexpr const char *kTorboxDashboardUrl = "https://torbox.app/dashboard";

// True when Pegasus' error says TorBox is still preparing the file. Ignores case
// and anything wrapped around it ("HTTP 502: ..."); false for every other error
// (rate limit, HTTP status, unsupported host, no token, selection expired, ...).
bool is_torbox_preparing(std::string_view error);

// The automatic retry while TorBox prepares the file.
//   start(now)  TorBox answered "still preparing" (first time or after a retry).
//   tick(now)   every frame: waiting (seconds left), retry_now (send it again),
//               gave_up (once, 10 minutes after the first answer), else idle /
//               in_flight (a retry is out, its answer not back yet).
//   cancel()    Circle, or the retry worked: back to idle.
class TorboxPrepareWait
{
public:
    static constexpr double kRetryEvery = 30.0;   // seconds between tries
    static constexpr double kGiveUpAfter = 600.0; // 10 minutes after the first "still preparing"

    struct Tick
    {
        enum class Kind
        {
            idle,
            waiting,   // next try in `seconds_left`
            retry_now, // the caller sends the debrid queue again now
            in_flight, // a retry is out
            gave_up,   // reported once; then idle
        };
        Kind kind = Kind::idle;
        int seconds_left = 0;
    };

    void start(double now);
    Tick tick(double now);
    void cancel();

    // Waiting or a retry in flight.
    bool active() const
    {
        return state_ == State::waiting || state_ == State::in_flight;
    }
    // Retries sent since the first "still preparing".
    int attempts() const
    {
        return attempts_;
    }

private:
    enum class State
    {
        idle,
        waiting,
        in_flight,
        gave_up,
    };
    State state_ = State::idle;
    double started_ = 0.0;
    double next_ = 0.0;
    int attempts_ = 0;
};

// "TorBox is getting this file ready on its server... trying again in 20 s"
// (seconds <= 0: "... trying again now").
std::string torbox_waiting_text(int seconds);
// "TorBox is getting this file ready on its server..." (busy box while a try is out).
std::string torbox_asking_text();
// The notice once the wait gives up.
std::string torbox_gave_up_text();
// Title of the notice ("TorBox is still preparing this file").
std::string torbox_preparing_title();
// The small chip on the link card.
std::string torbox_card_label();

} // namespace orbit
