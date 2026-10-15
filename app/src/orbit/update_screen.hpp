// Wings TV app - Updating Wings: the startup dialog and the full-screen update page.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// UI only. The orchestrator fills an UpdateView from the update and install logic every frame and acts on the
// UpdateAction the dialog or the page returns; nothing here downloads, checks or installs anything.

#pragma once

#include "core/input.hpp"
#include "orbit/panels.hpp"
#include "orbit/ui.hpp"
#include "ui/feedback.hpp"
#include "ui/glyphs.hpp"

#include <string>
#include <vector>

namespace orbit
{

struct Context;
struct Frame;
struct WingsFlowSnapshot; // wings_install.hpp

// What the update looks like right now (filled by the orchestrator).
struct UpdateView
{
    enum class Step
    {
        check,    // the update and its checksum
        download, // the helper downloads and checks it
        install,  // the helper puts it in place
        restart   // close Wings and open it again
    };
    enum class Kind
    {
        available,
        downloading,
        checking,
        ready_to_install,
        installing,
        installed,
        manual,
        failed
    };
    Kind kind = Kind::available;
    Step step = Step::download; // used for manual and failed: the step that stopped
    std::string current, latest;
    std::vector<std::string> notes; // what's new, <= 3 lines
    double done_bytes = 0.0, total_bytes = 0.0, bytes_per_s = 0.0, eta_s = 0.0;
    std::string message; // status or failure reason
    std::vector<std::string> manual_steps;
    bool can_cancel = false, can_retry = false;
};

// What the player asked for. quit_now comes only after the page's own "Quit Wings now?" confirm.
enum class UpdateAction
{
    none,
    update_now,
    later,
    cancel,
    install,
    retry,
    quit_now,
    back
};

// ---- pure helpers (unit-tested) ----

// "12.3 MB" / "1.2 GB".
std::string update_size_text(double bytes);
// "about 10 s left" / "about 3 min left"; empty when unknown.
std::string update_eta_text(double seconds);
// "12.3 MB of 54.7 MB · 4.1 MB/s · about 10 s left" (parts that are unknown are left out).
std::string update_progress_text(const UpdateView &view);
// 0..1 of the download.
float update_fraction(const UpdateView &view);
// The step the page highlights: -1 none yet (available), 0 download .. 3 restart.
int update_current_step(const UpdateView &view);
// Settings > Wings > Wings update: "Up to date" (view null), "{latest} available", "Installed — restart to finish".
std::string update_row_value(const UpdateView *view);

struct UpdateButton
{
    std::string label;
    UpdateAction action = UpdateAction::none;
    bool primary = false;
};
// The page's buttons for a state, left to right (the primary one first). Empty: no buttons (installing).
std::vector<UpdateButton> update_buttons(const UpdateView &view);

// The flow as the update logic sees it (wings_install.hpp) as the screen draws it. False (and *out untouched)
// while nothing newer is known (idle).
bool update_view_from_flow(const WingsFlowSnapshot &flow, UpdateView *out);

// ---- the startup dialog: "Wings X is available", what's new, Update now / Later ----
class UpdateDialog
{
  public:
    void open(const UpdateView &view);
    void close()
    {
        open_ = false;
    }
    bool is_open() const
    {
        return open_;
    }
    // update_now or later (Back = later) once, then the dialog closes itself.
    UpdateAction update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(Context &ctx, Frame &frame) const;
    std::vector<hui::ui::Hint> hints() const;

  private:
    bool open_ = false;
    int focus_ = 0; // 0 Update now, 1 Later
    std::string title_;
    std::vector<std::string> notes_;
    ui::Ease in_;
    float button_focus_[2] = {0.0f, 0.0f};
};

// ---- the full-screen update page: 4-step indicator and one status block ----
class UpdatePage
{
  public:
    void open(const UpdateView &view);
    void close()
    {
        open_ = false;
    }
    bool is_open() const
    {
        return open_;
    }
    // view: this frame's state. Returns the player's action; back closes the page itself.
    UpdateAction update(const UpdateView &view, const hui::InputFrame &input, float dt,
                        hui::ui::Feedback &feedback);
    // Draws over everything on frame.overlay (opaque), with its quit confirm on top.
    void draw(Context &ctx, Frame &frame) const;
    std::vector<hui::ui::Hint> hints() const;
    // The step shown: never lower than one already shown, until the player starts again (Update now, Retry).
    int shown_step() const
    {
        return shown_step_;
    }

  private:
    void take(const UpdateView &view);

    bool open_ = false;
    UpdateView view_;
    int shown_step_ = -1;
    int focus_ = 0;
    std::vector<UpdateButton> buttons_;
    std::vector<float> button_focus_;
    ConfirmDialog quit_confirm_;
    ui::Ease in_;
};

} // namespace orbit
