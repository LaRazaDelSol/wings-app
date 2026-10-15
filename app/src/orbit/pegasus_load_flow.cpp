// Wings TV app - First-run flow for loading Pegasus DL (see pegasus_load_flow.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_load_flow.hpp"

#include "orbit/pegasus_version.hpp"

#include <cstdio>

namespace orbit
{

LoadCommand PegasusLoadFlow::begin(const LoadFacts &facts)
{
    state_ = facts.cached ? LoadState::verifying : LoadState::downloading;
    fail_ = LoadFail::none;
    return LoadCommand::begin_load;
}

LoadCommand PegasusLoadFlow::tick(const LoadFacts &facts)
{
    if (facts.health_ok)
    {
        // Pegasus DL answers: nothing is offered, nothing is sent, a running job is stopped.
        unreachable_ = false;
        needs_internet_ = false;
        connected_ = true;
        quiet_ = false;
        retry_since_ = -1.0;
        LoadCommand cmd = LoadCommand::none;
        switch (state_)
        {
        case LoadState::downloading:
        case LoadState::verifying:
        case LoadState::loader_wait:
            cmd = state_ == LoadState::loader_wait ? LoadCommand::none : LoadCommand::cancel;
            state_ = LoadState::idle;
            break;
        case LoadState::sending:
        case LoadState::waiting_for_pegasus:
            state_ = LoadState::done;
            break;
        case LoadState::done:
            break;
        default:
            state_ = LoadState::idle;
            break;
        }
        return cmd;
    }

    if (!unreachable_)
    {
        unreachable_ = true;
        since_ = facts.now;
        if (state_ == LoadState::done)
            state_ = LoadState::idle;
    }
    needs_internet_ = choosable() && !facts.cached && !facts.net_ok;

    if (state_ == LoadState::waiting_for_pegasus && facts.now - sent_at_ >= kPegasusAnswerWaitS)
    {
        state_ = LoadState::failed;
        fail_ = LoadFail::no_answer;
        return LoadCommand::none;
    }
    if (state_ == LoadState::loader_wait)
    {
        if (facts.now < next_retry_)
            return LoadCommand::none;
        retry_run_ = true;
        return begin(facts); // keeps the retry window
    }
    // Answered before and silent for a while now: report it; nothing is sent by itself.
    if (connected_ && facts.now - since_ >= kPegasusDroppedS &&
        (state_ == LoadState::idle || state_ == LoadState::grace || state_ == LoadState::offering))
    {
        state_ = LoadState::dropped;
        fail_ = LoadFail::none;
        connected_ = false;
        return LoadCommand::none;
    }
    // After a successful automatic send Wings only reports state for the rest of the launch.
    const bool report_only = mode_ == PegasusStartMode::autoload && sent_;
    if (state_ == LoadState::idle && mode_ != PegasusStartMode::manual && !report_only && !quiet_ &&
        !connected_)
        state_ = LoadState::grace;
    if (state_ != LoadState::grace || facts.now - since_ < kPegasusOfferGraceS)
        return LoadCommand::none;

    if (mode_ == PegasusStartMode::autoload && !auto_tried_)
    {
        if (!facts.cached && !facts.net_ok)
        {
            needs_internet_ = true;
            state_ = LoadState::offering;
            return LoadCommand::none;
        }
        auto_tried_ = true;
        retry_run_ = false;
        return begin(facts);
    }
    state_ = LoadState::offering;
    needs_internet_ = !facts.cached && !facts.net_ok;
    return LoadCommand::none;
}

LoadCommand PegasusLoadFlow::cancel_by_player()
{
    if (!cancellable())
        return LoadCommand::none;
    const bool running = busy();
    state_ = LoadState::offering;
    fail_ = LoadFail::none;
    retry_run_ = false;
    retry_since_ = -1.0;
    return running ? LoadCommand::cancel : LoadCommand::none;
}

LoadCommand PegasusLoadFlow::choose(LoadChoice choice, const LoadFacts &facts)
{
    if (!choosable() || facts.health_ok)
        return LoadCommand::none;
    if (choice == LoadChoice::manual)
    {
        if (retryable())
            quiet_ = true; // session only: the persisted start-up mode stays as it is
        else
        {
            mode_ = PegasusStartMode::manual;
            mode_changed_ = true;
        }
        state_ = LoadState::idle;
        fail_ = LoadFail::none;
        return LoadCommand::none;
    }
    retry_run_ = false;
    retry_since_ = -1.0;
    if (choice == LoadChoice::retry)
    {
        if (!retryable())
            return LoadCommand::none;
        retry_run_ = true;
        return begin(facts);
    }
    if (manual_only())
        return LoadCommand::none;
    if (choice == LoadChoice::enable_auto)
    {
        mode_ = PegasusStartMode::autoload;
        mode_changed_ = true;
        notice_ = true;
        auto_tried_ = true; // this explicit load is the launch's one automatic attempt
    }
    return begin(facts);
}

void PegasusLoadFlow::set_mode(PegasusStartMode mode)
{
    if (mode == mode_)
        return;
    mode_ = mode;
    if (mode == PegasusStartMode::manual && !busy() && state_ != LoadState::waiting_for_pegasus)
        state_ = LoadState::idle;
}

void PegasusLoadFlow::on_downloading()
{
    if (busy())
        state_ = LoadState::downloading;
}

void PegasusLoadFlow::on_verifying()
{
    if (busy())
        state_ = LoadState::verifying;
}

void PegasusLoadFlow::on_sending()
{
    if (busy())
        state_ = LoadState::sending;
}

void PegasusLoadFlow::on_sent(double now)
{
    if (!busy())
        return;
    retry_since_ = -1.0;
    sent_ = true;
    sent_at_ = now;
    state_ = LoadState::waiting_for_pegasus;
    fail_ = LoadFail::none;
}

void PegasusLoadFlow::on_failed(LoadFail reason)
{
    if (!busy())
        return; // cancelled because Pegasus answered
    state_ = LoadState::failed;
    fail_ = reason;
}

void PegasusLoadFlow::on_loader_refused(double now)
{
    if (!busy())
        return;
    if (retry_since_ < 0.0)
        retry_since_ = now;
    if (now - retry_since_ >= kPegasusLoaderWindowS)
    {
        state_ = LoadState::failed;
        fail_ = LoadFail::no_loader;
        retry_since_ = -1.0;
        return;
    }
    state_ = LoadState::loader_wait;
    next_retry_ = now + kPegasusLoaderRetryS;
}

bool PegasusLoadFlow::take_mode_change()
{
    const bool value = mode_changed_;
    mode_changed_ = false;
    return value;
}

bool PegasusLoadFlow::take_auto_notice()
{
    const bool value = notice_;
    notice_ = false;
    return value;
}

// ---- release rules ----

const PegasusRelease *pick_pegasus_release(const std::vector<PegasusRelease> &releases, bool *untested)
{
    const PegasusRelease *in_range = nullptr;
    const PegasusRelease *any = nullptr;
    const auto newer = [](const PegasusRelease *a, const PegasusRelease &b)
    {
        bool ok = false;
        const int c = compare_pegasus_versions(b.tag, a->tag, &ok);
        return ok && c > 0;
    };
    for (const PegasusRelease &r : releases)
    {
        if (!r.downloadable || r.prerelease)
            continue;
        if (any == nullptr || newer(any, r))
            any = &r;
        if (pegasus_version_fit(r.tag) == PegasusVersionFit::tested &&
            (in_range == nullptr || newer(in_range, r)))
            in_range = &r;
    }
    if (untested != nullptr)
        *untested = in_range == nullptr && any != nullptr;
    return in_range != nullptr ? in_range : any;
}

const PegasusRelease *pick_cached_pegasus_release(const std::vector<PegasusRelease> &releases,
                                                  const std::function<bool(const std::string &)> &have)
{
    const PegasusRelease *best = nullptr;
    for (const PegasusRelease &r : releases)
    {
        if (!r.downloadable || !have(r.tag))
            continue;
        bool ok = false;
        if (best == nullptr || (compare_pegasus_versions(r.tag, best->tag, &ok) > 0 && ok))
            best = &r;
    }
    return best;
}

bool sha256_file(const std::string &path, std::string *hex, std::uint64_t *size, bool *elf_magic)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    Sha256 hash;
    std::uint8_t chunk[64 * 1024];
    std::uint64_t total = 0;
    bool first = true;
    bool magic = false;
    for (;;)
    {
        const std::size_t n = std::fread(chunk, 1, sizeof chunk, f);
        if (n == 0)
            break;
        if (first)
            magic = looks_like_elf(chunk, n);
        first = false;
        hash.update(chunk, n);
        total += n;
    }
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad)
        return false;
    if (hex != nullptr)
        *hex = hash.finish_hex();
    if (size != nullptr)
        *size = total;
    if (elf_magic != nullptr)
        *elf_magic = magic;
    return true;
}

} // namespace orbit
