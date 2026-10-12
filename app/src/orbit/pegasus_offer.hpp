// Wings TV app - The first-run "load Pegasus DL" choices under "Waiting for Pegasus DL".
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the pure PegasusLoadFlow against the real PegasusManager (list, download, verify, send) and
// draws the three choices and the progress inline, below the waiting message. Nothing here opens a
// popup; nothing is sent while GET /api/health answers.
#pragma once

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "orbit/pegasus_load_flow.hpp"
#include "orbit/ui.hpp"
#include "ui/feedback.hpp"

#include <string>

namespace orbit
{

struct OfferOutcome
{
    bool up = false;           // the player pressed up out of the choices: focus the top bar
    bool mode_changed = false; // persist `mode`
    PegasusStartMode mode = PegasusStartMode::ask;
    std::string toast;         // "Auto-load is on. ..."
};

class PegasusOffer
{
  public:
    // Frame thread, every frame. pegasus_screen: the tab on screen shows the waiting card.
    // focused: the content area (not the top bar) has the focus.
    OfferOutcome update(const hui::InputFrame &input, bool pegasus_screen, bool focused, bool online,
                        PegasusStartMode mode, double clock, hui::ui::Feedback &feedback);
    // Anything to draw under the waiting card (choices, progress or a result line).
    bool visible() const;
    // The choices hold the controller focus (they are showing and the content area is focused).
    bool takes_input() const
    {
        return flow_.choosable();
    }
    // The grace is running: the waiting card shows its spinner and "Looking for Pegasus DL" line.
    bool in_grace() const
    {
        return have_status_ && flow_.state() == LoadState::grace;
    }
    // Preview scenes: show this flow state as it is (no manager, no clock). update() then does nothing.
    // Returns the "Auto-load is on" text when the flow has that notice pending, else "".
    std::string preview_show(PegasusLoadFlow flow, std::uint64_t got, std::uint64_t size,
                             const std::string &detail = {}, bool untested = false);
    void draw(const ui::Type &type, hui::gfx::DrawList &list, double clock, bool focused) const;

    const PegasusLoadFlow &flow() const
    {
        return flow_;
    }

  private:
    enum class Stage : std::uint8_t
    {
        none,
        prepare,     // find the release; read the list first when there is none
        downloading, // a download job runs or just ended
        sending      // verify + send job runs or just ended
    };
    struct Target
    {
        std::string tag;
        std::uint64_t size = 0;
        bool untested = false;
        bool found = false;
    };
    Target pick(bool prefer_cached) const;
    bool file_there(const std::string &tag) const;
    void refresh_cached(double clock, bool force);
    void run_stage(bool online, double clock);
    void fail(LoadFail reason, const std::string &detail = {});
    std::string status_line() const;

    PegasusLoadFlow flow_;
    Stage stage_ = Stage::none;
    bool auto_run_ = false;    // the running load was started by Auto mode (keeps the cached release)
    bool list_asked_ = false;
    bool redownloaded_ = false;
    bool cached_ = false;
    double cached_at_ = -100.0;
    Target target_;
    std::string detail_;       // the failure sentence from the manager, shown under the reason
    int focus_ = 0;            // 0 Enable, 1 Once, 2 Manual (manual-only: the single button)
    bool have_status_ = false;
    bool preview_ = false;
    std::uint64_t got_ = 0;
    std::uint64_t size_ = 0;
};

} // namespace orbit
