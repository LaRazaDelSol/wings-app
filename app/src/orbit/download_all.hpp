// Wings TV app - "Download all parts": one part at a time, a saved record of what was queued.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The series runner starts one part through the existing per-link queue call,
// waits for its outcome, and only then starts the next (never a flood of
// commands). A part that does not queue is asked again after a short wait when
// the error may pass by itself (timed out, Pegasus busy), then tried on the
// other hosts that carry the same part; when every one fails the part is
// listed and the series goes on with the next. Only a row of failed parts
// (Pegasus seems stuck) or Circle stops it early. The waits are driven by
// tick() on the frame thread: nothing sleeps, no thread is started. The
// (packageId, linkIndex) pairs queued are kept in a small versioned file of the
// checked save container (prefs kind), so a second press, or a new run of the
// app, skips them.

#pragma once

#include "orbit/game_page.hpp"
#include "orbit/game_view.hpp"
#include "orbit/store.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// Empty when the part was queued; otherwise why it was not ("Part n: ..." is built by the caller).
// A capture that started without an error is not a queued part: "still resolving".
std::string part_failure_reason(const LinkStart &outcome);

// One part of a set: refs[0] is the link of the chosen host, refs[1..] the same
// part on other hosts, tried in order. `hosts` names them (parallel to refs).
struct PartPlan
{
    int number = 0;
    std::vector<LinkRef> refs;
    std::vector<std::string> hosts;
};

struct FailedPart
{
    int number = 0;
    std::string why;
};

struct AlternatePart
{
    int number = 0;
    std::string host; // the host the part was queued from
};

// Attempts per link while the error is the kind that may pass by itself.
constexpr int kPartAttempts = 3;
// Waits before the second and the third attempt, in seconds.
constexpr float kFirstBackoff = 2.0f;
constexpr float kSecondBackoff = 6.0f;
// This many failed parts in a row stop the series ("Pegasus seems stuck").
constexpr int kStopAfterFailedParts = 4;

class DownloadAll
{
public:
    using Done = std::function<void(const LinkStart &)>;
    // Starts one part and calls Done once, on the frame thread, when its fate is known.
    using Start = std::function<void(const LinkRef &, Done)>;

    enum class End : std::uint8_t
    {
        none,                   // never ran, or running
        finished,               // every part queued
        finished_with_failures, // went to the end; some parts did not queue
        stopped,                // kStopAfterFailedParts failed parts in a row
        cancelled,              // stopped by the player
    };

    // Public so the screen can draw progress: "Queueing part {current} ({done+1} of {total})".
    bool active = false;
    std::string host; // the chosen host of the set
    int total = 0;
    int done = 0; // parts decided (queued or failed)
    int ok = 0;   // parts queued by this run
    End end = End::none;
    int current = 0;          // number of the part being tried
    int attempt = 0;          // 1-based attempt on the current link
    std::string trying_host;  // host of the link being tried
    float waiting = 0.0f;     // seconds left before the next attempt; 0 when none is pending
    std::vector<FailedPart> failed;
    int via_alternate = 0; // parts queued from another host than the chosen one
    std::vector<AlternatePart> alternates;
    // The last part that did not queue (kept for older callers; `failed` has them all).
    int failed_number = 0;
    std::string failed_why;

    // `queued` is called for every part that was queued, with the link that worked;
    // `finished` once at the end.
    void begin(std::vector<PartPlan> parts, std::string host_name, Start start,
               std::function<void(const LinkRef &)> queued, std::function<void()> finished);
    // One link per part, no other hosts. `numbers` are the parts' own numbers (for messages).
    void begin(std::vector<LinkRef> refs, std::vector<int> numbers, std::string host_name, Start start,
               std::function<void(const LinkRef &)> queued, std::function<void()> finished);
    // Every frame: counts down a wait between attempts and starts the next one.
    void tick(float dt);
    // Stops at once: a wait ends, a part in flight is let go (its late answer
    // changes nothing; the screen also cancels the Store's link action).
    void cancel();

    // The toast of a clean run, and the notice (title + message) of any other end.
    std::string toast() const;
    std::string report_title() const;
    std::string report_message() const;

private:
    void attempt_now();
    void on_result(int call, const LinkStart &outcome);
    void part_queued();
    void part_failed(const std::string &why);
    void next_part();
    void finish(End how);
    const std::string &host_of(std::size_t ref) const;

    std::vector<PartPlan> parts_;
    std::size_t part_ = 0;  // index in parts_
    std::size_t ref_ = 0;   // index in the part's refs
    int tries_ = 0;         // attempts made on the current link
    int in_a_row_ = 0;      // failed parts in a row
    std::string last_why_;  // why the current part's last attempt failed
    bool in_flight_ = false;
    bool waiting_ = false;
    int call_ = 0; // numbers every start; a Done of an older one is ignored
    bool cancel_ = false;
    Start start_;
    std::function<void(const LinkRef &)> queued_;
    std::function<void()> finished_;
};

// ---- the record of queued parts ----

// Payload: "v=1" then one "q=<packageId>\t<linkIndex>" line per pair. Damaged
// lines are skipped; false only for an empty payload or an unknown version.
std::string encode_queued_record(const StartedLinks &started);
bool decode_queued_record(std::string_view payload, StartedLinks *out);
// First readable, valid file wins; the record stays as it was when none loads.
bool load_queued_record(const std::vector<std::string> &paths, StartedLinks *out);
// Writes every path that can be written; returns how many were.
int save_queued_record(const std::vector<std::string> &paths, const StartedLinks &started);
// Next to each Wings prefs file: "<dir>/queued_parts.sav".
std::vector<std::string> queued_record_paths(const std::vector<std::string> &prefs_paths);

} // namespace orbit
