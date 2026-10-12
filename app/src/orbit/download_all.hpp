// Wings TV app - "Download all parts": one part at a time, a saved record of what was queued.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The series runner starts one part through the existing per-link queue call,
// waits for its outcome, and only then starts the next (never a flood of
// commands). The first hard failure stops it and says how many parts were
// queued; Circle stops it after the part in flight. The (packageId, linkIndex)
// pairs queued are kept in a small versioned file of the checked save container
// (prefs kind), so a second press, or a new run of the app, skips them.

#pragma once

#include "orbit/game_page.hpp"
#include "orbit/game_view.hpp"
#include "orbit/store.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// Empty when the part was queued; otherwise why it was not ("Part n: ..." is built by the caller).
std::string part_failure_reason(const LinkStart &outcome);

class DownloadAll
{
public:
    using Done = std::function<void(const LinkStart &)>;
    // Starts one part and calls Done once, on the frame thread, when it ended.
    using Start = std::function<void(const LinkRef &, Done)>;

    enum class End : std::uint8_t
    {
        none,      // never ran, or running
        finished,  // every part queued
        failed,    // stopped at the first part that did not queue
        cancelled, // stopped by the player after the part in flight
    };

    // Public so the screen can draw progress: "Queueing part {done+1} of {total}".
    bool active = false;
    std::string host;
    int total = 0;
    int done = 0; // parts whose outcome came back
    int ok = 0;   // parts queued by this run
    End end = End::none;
    int failed_number = 0; // the part number that stopped it
    std::string failed_why;

    // `numbers` are the parts' own numbers (for messages), same length as `refs`.
    // `queued` is called for every part that was queued, `finished` once at the end.
    void begin(std::vector<LinkRef> refs, std::vector<int> numbers, std::string host_name, Start start,
               std::function<void(const LinkRef &)> queued, std::function<void()> finished);
    // Stops after the part in flight (it still counts if it queues). Nothing else is started.
    void cancel();

    // The toast of a clean run, and the notice (title + message) of a stopped one.
    std::string toast() const;
    std::string report_title() const;
    std::string report_message() const;

private:
    void step();
    void on_result(int index, const LinkStart &outcome);
    void finish(End how);

    std::vector<LinkRef> refs_;
    std::vector<int> numbers_;
    int next_ = 0;
    int generation_ = 0;
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
