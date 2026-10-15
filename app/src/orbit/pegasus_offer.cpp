// Wings TV app - The first-run "load Pegasus DL" choices (see pegasus_offer.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_offer.hpp"

#include "orbit/busy_cancel.hpp"
#include "orbit/i18n.hpp"
#include "orbit/pegasus_manager.hpp"
#include "orbit/pegasus_offer_layout.hpp"
#include "orbit/pegasus_start_mode.hpp"
#include "orbit/spinner_arc.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

namespace orbit
{

namespace
{

std::string megabytes(std::uint64_t bytes)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

const char *choice_label(int index, bool retry = false)
{
    if (retry && index == 0)
        return tr("Retry");
    switch (index)
    {
    case 0:
        return tr("Enable Pegasus-dl.elf Auto-Load");
    case 1:
        return tr("Load Pegasus-dl.elf once");
    default:
        return tr("I will load Pegasus-dl.elf myself");
    }
}

} // namespace

bool PegasusOffer::file_there(const std::string &tag) const
{
    struct stat st;
    return stat(pegasus_manager().file_path(tag).c_str(), &st) == 0;
}

PegasusOffer::Target PegasusOffer::pick(bool prefer_cached) const
{
    const std::vector<PegasusRelease> releases = pegasus_manager().releases();
    Target t;
    const PegasusRelease *r = nullptr;
    bool untested = false;
    if (prefer_cached)
        r = pick_cached_pegasus_release(releases, [this](const std::string &tag) { return file_there(tag); });
    if (r == nullptr)
        r = pick_pegasus_release(releases, &untested);
    if (r != nullptr)
    {
        t.found = true;
        t.tag = r->tag;
        t.size = r->size;
        t.untested = untested;
    }
    return t;
}

void PegasusOffer::refresh_cached(double clock, bool force)
{
    if (!force && clock - cached_at_ < 1.0)
        return;
    cached_at_ = clock;
    const Target t = pick(flow_.mode() == PegasusStartMode::autoload);
    cached_ = t.found && file_there(t.tag);
    if (stage_ == Stage::none)
        target_ = t;
}

void PegasusOffer::fail(LoadFail reason, const std::string &detail)
{
    stage_ = Stage::none;
    detail_ = detail;
    flow_.on_failed(reason);
}

void PegasusOffer::run_stage(bool online, double clock)
{
    if (stage_ == Stage::none)
        return;
    PegasusManager &pm = pegasus_manager();
    const PegasusStatus st = pm.status();
    if (st.busy())
    {
        // Mirror what the job is doing into the machine.
        if (st.phase == PegasusPhase::downloading)
            flow_.on_downloading();
        else if (st.phase == PegasusPhase::verifying && stage_ == Stage::sending)
            flow_.on_verifying();
        else if (st.phase == PegasusPhase::sending)
            flow_.on_sending();
        return;
    }
    if (!flow_.busy())
    {
        stage_ = Stage::none; // cancelled (Pegasus answered)
        return;
    }
    if (stage_ == Stage::prepare)
    {
        const Target t = pick(auto_run_);
        if (!t.found)
        {
            if (!list_asked_ && st.net_ok)
            {
                list_asked_ = true;
                if (pm.check_now())
                    return;
            }
            if (!st.net_ok)
                fail(LoadFail::needs_internet);
            else
                fail(LoadFail::download, st.check_error);
            return;
        }
        target_ = t;
        if (file_there(t.tag))
        {
            stage_ = Stage::sending;
            flow_.on_verifying();
            if (!pm.verify_and_send(t.tag, online))
                fail(LoadFail::send);
        }
        else if (st.net_ok)
        {
            stage_ = Stage::downloading;
            flow_.on_downloading();
            if (!pm.download_release(t.tag))
                fail(LoadFail::download, st.net_note);
        }
        else
        {
            fail(LoadFail::needs_internet);
        }
        return;
    }
    if (stage_ == Stage::downloading)
    {
        if (st.message_is_error || !file_there(target_.tag))
        {
            fail(LoadFail::download, st.message);
            return;
        }
        stage_ = Stage::sending;
        flow_.on_verifying();
        if (!pm.verify_and_send(target_.tag, online))
            fail(LoadFail::send);
        return;
    }
    // sending: the job ended
    if (st.sent)
    {
        stage_ = Stage::none;
        flow_.on_sent(clock);
        return;
    }
    if (st.fail == PegasusFail::verify && !redownloaded_ && st.net_ok)
    {
        // A cached file that no longer matches is fetched again, once.
        redownloaded_ = true;
        stage_ = Stage::prepare;
        return;
    }
    if (st.fail == PegasusFail::verify)
        fail(LoadFail::verify, st.message);
    else if (st.fail == PegasusFail::no_loader)
    {
        // Usually the loader is still starting after a boot: the flow retries on its own timer.
        flow_.on_loader_refused(clock);
        stage_ = Stage::none;
        detail_ = st.message;
    }
    else
        fail(LoadFail::send, st.message);
}

OfferOutcome PegasusOffer::update(const hui::InputFrame &input, bool pegasus_screen, bool focused, bool online,
                                  PegasusStartMode mode, double clock, hui::ui::Feedback &feedback)
{
    OfferOutcome out;
    if (preview_)
        return out; // a fixed picture of the flow (the preview scenes)
    PegasusManager &pm = pegasus_manager();
    const PegasusStatus st = pm.status();
    have_status_ = st.attached;
    if (!st.attached)
        return out;
    pm.note_pegasus_online(online);
    flow_.set_mode(mode);
    refresh_cached(clock, false);

    LoadFacts facts;
    facts.now = clock;
    facts.health_ok = online;
    facts.net_ok = st.net_ok;
    facts.cached = cached_;
    const LoadCommand cmd = flow_.tick(facts);
    if (cmd == LoadCommand::begin_load)
    {
        stage_ = Stage::prepare;
        auto_run_ = flow_.mode() == PegasusStartMode::autoload || flow_.retry_run();
        list_asked_ = false;
        redownloaded_ = false;
        detail_.clear();
    }
    else if (cmd == LoadCommand::cancel)
    {
        pm.cancel();
        stage_ = Stage::none;
    }
    // Circle while downloading, verifying, sending or waiting (B-18): back to the
    // choices at once; the job's late result changes nothing (PegasusManager::cancel).
    if (pegasus_screen && focused && !online && flow_.cancellable() && input.is_pressed(hui::Action::back))
    {
        if (flow_.cancel_by_player() == LoadCommand::cancel)
            pm.cancel();
        stage_ = Stage::none;
        detail_.clear();
        feedback.play(hui::audio::Cue::back);
    }

    if (pegasus_screen && focused && flow_.choosable() && !online)
    {
        const bool only_manual = flow_.manual_only();
        const bool two = flow_.retryable() && !only_manual; // Retry and the gray choice: slots 0 and 2
        if (only_manual)
            focus_ = 2;
        else if (two && focus_ == 1)
            focus_ = 0;
        if (input.nav == hui::Direction::left && !only_manual && focus_ > 0)
        {
            focus_ = two ? 0 : focus_ - 1;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.nav == hui::Direction::right && !only_manual && focus_ < 2)
        {
            focus_ = two ? 2 : focus_ + 1;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.nav == hui::Direction::up)
        {
            out.up = true;
        }
        if (input.is_pressed(hui::Action::confirm))
        {
            const LoadChoice choice = focus_ == 0   ? (flow_.retryable() ? LoadChoice::retry : LoadChoice::enable_auto)
                                      : focus_ == 1 ? LoadChoice::once
                                                    : LoadChoice::manual;
            feedback.play(hui::audio::Cue::select);
            const LoadCommand c = flow_.choose(choice, facts);
            if (c == LoadCommand::begin_load)
            {
                stage_ = Stage::prepare;
                auto_run_ = flow_.mode() == PegasusStartMode::autoload || flow_.retry_run();
                list_asked_ = false;
                redownloaded_ = false;
                detail_.clear();
            }
        }
    }
    else if (flow_.choosable() && !flow_.manual_only() && focus_ == 2)
    {
        focus_ = 0; // a fresh offer starts on the primary choice
    }

    if (flow_.take_mode_change())
    {
        out.mode_changed = true;
        out.mode = flow_.mode();
    }
    if (flow_.take_auto_notice())
        out.toast = tr("Auto-load is on. Change it any time in Settings > Pegasus DL.");

    run_stage(online, clock);
    got_ = st.got_bytes;
    size_ = target_.size;
    return out;
}

std::string PegasusOffer::preview_show(PegasusLoadFlow flow, std::uint64_t got, std::uint64_t size,
                                       const std::string &detail, bool untested)
{
    preview_ = true;
    have_status_ = true;
    got_ = got;
    size_ = size;
    detail_ = detail;
    target_.size = size;
    target_.untested = untested;
    focus_ = 0;
    const bool notice = flow.take_auto_notice();
    flow_ = flow;
    return notice ? std::string(tr("Auto-load is on. Change it any time in Settings > Pegasus DL.")) : std::string();
}

bool PegasusOffer::visible() const
{
    if (!have_status_)
        return false;
    switch (flow_.state())
    {
    case LoadState::grace:
    case LoadState::offering:
    case LoadState::failed:
    case LoadState::dropped:
    case LoadState::loader_wait:
    case LoadState::downloading:
    case LoadState::verifying:
    case LoadState::sending:
    case LoadState::waiting_for_pegasus:
        return true;
    default:
        return false;
    }
}

std::string PegasusOffer::status_line() const
{
    std::string line;
    switch (flow_.state())
    {
    case LoadState::downloading:
        if (size_ == 0)
            return tr("Reading the official release list");
        line = tr("Downloading {got} of {size} MB", {{"got", megabytes(got_)}, {"size", megabytes(size_)}});
        if (target_.untested)
            line += "  \xC2\xB7  " + std::string(tr("{tag} is newer than Wings was tested with", {{"tag", target_.tag}}));
        return line;
    case LoadState::verifying:
        return tr("Verifying the checksum");
    case LoadState::sending:
        return tr("Sending to the payload loader on port {port}", {{"port", std::to_string(kPegasusLoaderPort)}});
    case LoadState::waiting_for_pegasus:
        return tr("Sent. Waiting for Pegasus DL to start");
    case LoadState::loader_wait:
        return tr("Waiting for the payload loader to start...");
    case LoadState::dropped:
        return tr("Pegasus DL stopped responding");
    case LoadState::failed:
        switch (flow_.fail())
        {
        case LoadFail::no_loader:
            return tr("No payload loader found on port {port}. Load Pegasus DL with your loader.",
                      {{"port", std::to_string(kPegasusLoaderPort)}});
        case LoadFail::verify:
            return tr("The file did not match its published SHA-256, so it was not sent.");
        case LoadFail::send:
            return tr("The payload loader stopped accepting data. Load Pegasus DL with your loader.");
        case LoadFail::no_answer:
            return tr("Pegasus DL has not answered yet. Wings keeps checking.");
        case LoadFail::needs_internet:
            return tr("Needs the internet once to fetch Pegasus DL");
        default:
            line = tr("Could not download Pegasus DL.");
            if (!detail_.empty())
                line += " " + detail_;
            return line;
        }
    default:
        break;
    }
    if (flow_.needs_internet())
        return tr("Needs the internet once to fetch Pegasus DL");
    return {};
}

void PegasusOffer::draw(const ui::Type &type, hui::gfx::DrawList &list, double clock, bool focused) const
{
    if (!visible())
        return;
    constexpr float kLineY = 790.0f;
    std::string line = status_line();
    if (!line.empty() && flow_.cancellable())
        line = with_stop_hint(line.c_str()); // B-18: Circle goes back to the choices
    if (!line.empty())
        ui::text_fit(list, type.regular, line, 960.0f, kLineY, 23.0f, 1500.0f,
                     flow_.retryable() ? ui::color::tier_red : ui::color::text2,
                     ui::Align::center);
    const LoadState state = flow_.state();
    if (!line.empty() && (flow_.busy() || state == LoadState::waiting_for_pegasus || state == LoadState::loader_wait))
    {
        // Work is going on: a small spinner left of the line (a press during the grace shows this too).
        const float w = std::min(1500.0f, type.regular.measure(line, 23.0f));
        draw_spinner_arc(list, 960.0f - w * 0.5f - 24.0f, kLineY - 8.0f, 10.0f, clock, ui::color::text2);
    }
    if (state == LoadState::downloading && size_ > 0)
    {
        const float fraction = std::min(1.0f, static_cast<float>(got_) / static_cast<float>(size_));
        const ui::Rect bar{660.0f, kLineY + 22.0f, 600.0f, 8.0f};
        list.rounded_rect(bar, 4.0f, ui::Color::rgb(0xffffff, 0.12f));
        list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * fraction), bar.h}, 4.0f, ui::color::action);
    }
    if (!flow_.choosable())
        return;

    // The choices, in one row: primary, secondary, tertiary (gray).
    const bool only_manual = flow_.manual_only();
    const bool retry = flow_.retryable() && !only_manual; // Retry (white) and the gray choice
    const int first = only_manual ? 2 : 0;
    float widths[3] = {0.0f, 0.0f, 0.0f};
    for (int i = first; i < 3; ++i)
        if (!(retry && i == 1))
            widths[i] = ui::button_width(type, choice_label(i, retry), 20.0f, ui::ButtonKind::secondary) + 12.0f;
    const OfferLayout layout = offer_layout(widths, first, kLineY + 30.0f);
    for (int i = first; i < 3; ++i)
    {
        if (widths[i] <= 0.0f)
            continue;
        const ui::Rect r = layout.choice[i];
        const float focus = focused && focus_ == i ? 1.0f : 0.0f;
        if (i == 0)
        {
            ui::button(list, type, r, choice_label(i, retry), ui::ButtonKind::primary, focus);
        }
        else if (i == 1)
        {
            ui::button(list, type, r, choice_label(i), ui::ButtonKind::secondary, focus);
        }
        else
        {
            // Tertiary: a quiet gray pill, the same ring when focused.
            ui::focus_ring(list, r, r.h * 0.5f, focus);
            list.rounded_rect(r, r.h * 0.5f, ui::Color::rgb(0xffffff, 0.05f));
            ui::text_fit(list, type.medium, choice_label(i), r.cx(), r.cy() + 20.0f * 0.36f, 20.0f, r.w - 32.0f,
                         ui::color::text3, ui::Align::center);
        }
    }
    ui::text_fit(list, type.regular,
                 tr("Source: github.com/pegasus-ps5/pegasus-dl official release, verified by SHA-256."), 960.0f,
                 layout.source_y, 19.0f, 1500.0f, ui::color::text3, ui::Align::center);
}

} // namespace orbit
