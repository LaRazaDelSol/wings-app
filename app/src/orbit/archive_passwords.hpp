// Wings TV app - Known archive passwords, tried one at a time before the prompt opens.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pegasus holds a ZIP / RAR / 7z download (or a Files-tab extraction) at
// waiting_password until a password arrives. Before the player is asked, Wings
// sends the public passwords release groups use, in a fixed order, each at most
// once per job. The machine is pure: the screen reports what it sees (the job's
// state and detail text, a clock) and does what the returned step says; it never
// talks to the backend itself, so it is tested with a scripted fake.
//
// How Pegasus reports a wrong password is NOT verified on a live server. The
// web UI reopens its dialog when the job's detail text changes while it stays
// waiting_password, and a job that goes back to extracting and then returns to
// waiting_password is the other plausible shape. Both count as "did not work";
// so does the state staying unchanged for kSettleMs, which keeps the machine from
// waiting forever on a server that says nothing.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// The passwords, in the order they are tried (case-sensitive).
constexpr std::size_t kKnownPasswordCount = 6;
const std::array<const char *, kKnownPasswordCount> &known_archive_passwords();

// "Tried 6 known passwords" (the note under the prompt's field).
std::string tried_note(int tried);

class PasswordTrials
{
  public:
    // What the job (or file operation) looks like now.
    enum class Seen : std::uint8_t
    {
        waiting,  // waiting_password
        active,   // running / extracting / queued: it moved on
        finished, // completed
        ended,    // failed, cancelled, or gone from the list
    };

    struct Step
    {
        enum class Kind : std::uint8_t
        {
            none,
            send,   // send `password` now, then call sent() or send_failed()
            prompt, // open the password prompt, with `note` under the field
            done,   // the job is over; the trial was dropped
        };
        Kind kind = Kind::none;
        std::string password;
        int number = 0; // send: 1-based position of the password; prompt: how many were tried
        int total = 0;
        std::string note;
    };

    // After a send step: the call was made (the settle clock starts) ...
    void sent(const std::string &key, std::int64_t now_ms);
    // ... or it failed: automatic trying stops and the prompt opens, with the error in its note.
    Step send_failed(const std::string &key, std::string_view error);

    // Called every time the screen sees the job. Returns at most one step.
    Step observe(const std::string &key, Seen seen, std::string_view detail, std::int64_t now_ms);

    // The player's own password: kept for this job only (never offered to another job).
    // `detail` and `now_ms` are what the job looked like when it was sent: a try that leaves the
    // job waiting (text changed, went away and came back, or nothing moved for kSettleMs) reopens
    // the prompt once, saying it did not work.
    void set_user_password(const std::string &key, std::string password, std::string_view detail = {},
                           std::int64_t now_ms = -1);
    const std::string *user_password(const std::string &key) const;

    // 1-based number of the password in flight ("Trying known passwords (3/6)"); 0 when not trying.
    int trying_number(const std::string &key) const;
    int tried_count(const std::string &key) const;

    // Every job being tracked (a screen reports the ones that left its list as ended).
    std::vector<std::string> keys() const;

    void forget(const std::string &key)
    {
        trials_.erase(key);
    }

    static constexpr std::int64_t kSettleMs = 8000;

  private:
    enum class Phase : std::uint8_t
    {
        next,     // send the next password when the job waits
        awaiting, // one was sent; waiting for its outcome
        prompt,   // the prompt was opened (or automatic trying was given up)
    };
    struct Trial
    {
        Phase phase = Phase::next;
        int tried = 0;     // passwords sent so far (also the index of the next one)
        bool left = false; // seen leaving waiting_password since the last send
        std::int64_t sent_at = 0;
        std::string detail_at_send;
        std::string user_password;
        bool has_user_password = false;
        bool prompt_left = false; // prompt phase: the job moved on after the player's try
        bool user_sent = false;   // the player's password is in flight
    };
    static Step send_step(Trial &trial, std::string_view detail);
    static Step prompt_step(Trial &trial, std::string_view error);

    std::map<std::string, Trial> trials_;
};

} // namespace orbit
