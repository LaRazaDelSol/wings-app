// Wings TV app - First-run flow for loading Pegasus DL: the pure state machine and release rules.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Waiting for Pegasus DL" can offer to send the official, checksum-verified Pegasus DL file to the
// console's payload loader on 127.0.0.1:9021. This file is the decision logic only: time and facts
// go in, a command comes out. It touches no network, no disk and no clock, so every rule is a unit
// test (grace period, one automatic send per launch, refusal fallback, health cancels, offline).
// pegasus_offer.* runs it against the real manager and draws it.
#pragma once

#include "orbit/pegasus_manager.hpp"
#include "orbit/pegasus_start_mode.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orbit
{

// How long Pegasus must have been unreachable, without a break, before anything is offered or sent.
inline constexpr double kPegasusOfferGraceS = 10.0;
// After a send, how long Wings keeps waiting for Pegasus DL to answer before saying so.
inline constexpr double kPegasusAnswerWaitS = 90.0;
// The payload loader refusing 127.0.0.1:9021 usually means it is still starting after a boot: the send
// is retried every kPegasusLoaderRetryS for up to kPegasusLoaderWindowS before it is a failure.
inline constexpr double kPegasusLoaderRetryS = 3.0;
inline constexpr double kPegasusLoaderWindowS = 30.0;
// Pegasus DL answered earlier and now does not: after this long it is "stopped responding".
inline constexpr double kPegasusDroppedS = 5.0;

enum class LoadState : std::uint8_t
{
    idle,
    grace,
    offering,
    downloading,
    verifying,
    sending,
    waiting_for_pegasus,
    loader_wait, // the loader refused: retrying every few seconds
    failed,
    dropped,     // Pegasus DL answered earlier and stopped; only an explicit Retry sends
    done
};

enum class LoadFail : std::uint8_t
{
    none,
    needs_internet, // nothing cached and no network
    no_loader,      // 127.0.0.1:9021 refused the connection
    download,       // the release list or the file could not be fetched
    verify,         // the file did not match the published SHA-256
    send,           // the loader stopped accepting data
    no_answer       // sent, but Pegasus DL never answered
};

enum class LoadChoice : std::uint8_t
{
    enable_auto,
    once,
    manual,
    retry // explicit: from failed or dropped; always allowed
};

enum class LoadCommand : std::uint8_t
{
    none,
    begin_load, // download if needed, verify, send
    cancel      // Pegasus answered while loading: stop
};

struct LoadFacts
{
    double now = 0.0;
    bool health_ok = false; // GET /api/health answers
    bool net_ok = false;    // the network gate is open
    bool cached = false;    // a file for the target release is already on disk
};

class PegasusLoadFlow
{
  public:
    explicit PegasusLoadFlow(PegasusStartMode mode = PegasusStartMode::ask) : mode_(mode)
    {
    }

    LoadCommand tick(const LoadFacts &facts);
    // The player picked one of the three choices (only meaningful while offering() is true).
    LoadCommand choose(LoadChoice choice, const LoadFacts &facts);
    // Settings changed the mode.
    void set_mode(PegasusStartMode mode);
    // Circle while loading (B-18): back to the choices at once. LoadCommand::cancel
    // when a job runs (the runner stops it and ignores its result).
    LoadCommand cancel_by_player();
    bool cancellable() const
    {
        return busy() || state_ == LoadState::loader_wait || state_ == LoadState::waiting_for_pegasus;
    }

    // What the runner saw.
    void on_downloading();
    void on_verifying();
    void on_sending();
    void on_sent(double now);
    void on_failed(LoadFail reason);
    // The send found 127.0.0.1:9021 refusing: retried by tick() until the window is spent.
    void on_loader_refused(double now);

    LoadState state() const
    {
        return state_;
    }
    LoadFail fail() const
    {
        return fail_;
    }
    PegasusStartMode mode() const
    {
        return mode_;
    }
    bool busy() const
    {
        return state_ == LoadState::downloading || state_ == LoadState::verifying ||
               state_ == LoadState::sending;
    }
    // The choices are on screen (Failed and Dropped keep them so the player can try again).
    bool offering() const
    {
        return state_ == LoadState::offering || retryable();
    }
    // Failed or dropped: the choices are "Retry" and a gray "I will load it myself" that is session only.
    bool retryable() const
    {
        return state_ == LoadState::failed || state_ == LoadState::dropped;
    }
    // The choices may be pressed: while offering, and during the grace (a press then starts the load
    // at once; the runner re-checks health right before the send). Grace is `grace`, not `offering`.
    bool choosable() const
    {
        return offering() || state_ == LoadState::grace;
    }
    // Only "I will load it myself" is offered: nothing cached and no internet.
    bool manual_only() const
    {
        return needs_internet_;
    }
    // The load that just began is a retry (the cached release is preferred).
    bool retry_run() const
    {
        return retry_run_;
    }
    bool needs_internet() const
    {
        return needs_internet_;
    }
    bool sent_this_launch() const
    {
        return sent_;
    }
    // The mode was changed by a choice since the last call (to be persisted).
    bool take_mode_change();
    // "Auto-load is on" notice, once, after Enable.
    bool take_auto_notice();

  private:
    LoadCommand begin(const LoadFacts &facts);

    PegasusStartMode mode_;
    LoadState state_ = LoadState::idle;
    LoadFail fail_ = LoadFail::none;
    bool unreachable_ = false;
    double since_ = 0.0;
    double sent_at_ = 0.0;
    bool auto_tried_ = false;  // the automatic attempt of this launch was spent
    bool sent_ = false;
    bool connected_ = false;   // Pegasus DL answered at some point this launch
    bool quiet_ = false;       // the gray choice of a failure: nothing more is offered until it answers
    bool retry_run_ = false;
    double retry_since_ = -1.0; // first refusal of the current retry window, < 0 none
    double next_retry_ = 0.0;
    bool needs_internet_ = false;
    bool mode_changed_ = false;
    bool notice_ = false;
};

// ---- release rules ----
// The release to load: the newest downloadable, non-pre-release one inside Wings' tested range, else
// the newest overall with *untested set. Null when nothing can be downloaded.
const PegasusRelease *pick_pegasus_release(const std::vector<PegasusRelease> &releases, bool *untested);
// Automatic mode never moves to a newer release by itself: of the releases whose file `have` reports
// on disk, the newest one; null when none.
const PegasusRelease *pick_cached_pegasus_release(const std::vector<PegasusRelease> &releases,
                                                  const std::function<bool(const std::string &)> &have);

// Streams the file through SHA-256 (64 KiB at a time). False when it cannot be read.
bool sha256_file(const std::string &path, std::string *hex, std::uint64_t *size, bool *elf_magic);

} // namespace orbit
