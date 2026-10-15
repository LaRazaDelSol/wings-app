// Wings TV app - Updating Wings: the startup dialog and the full-screen update page.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/update_screen.hpp"

#include "orbit/i18n.hpp"
#include "orbit/screens.hpp"
#include "orbit/theme.hpp"
#include "orbit/wings_install.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{
constexpr float kButtonWidth = 260.0f;
constexpr float kStepRadius = 26.0f;
constexpr float kStepsY = 330.0f;            // centre line of the step dots
constexpr float kCardTop = 450.0f;           // the status block
constexpr float kCardHeight = 470.0f;
constexpr float kBodySize = 28.0f;
constexpr float kBodyLine = 42.0f;

std::string one_decimal(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

int step_index(UpdateView::Step step)
{
    return static_cast<int>(step);
}

// Moves the focus over a row of `count` buttons; refuses at the ends.
bool move_focus(int &focus, int count, const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    if (input.nav != Direction::left && input.nav != Direction::right)
        return false;
    const int next = focus + (input.nav == Direction::right ? 1 : -1);
    if (count > 0 && next >= 0 && next < count)
    {
        focus = next;
        feedback.play(hui::audio::Cue::focus);
        return true;
    }
    refuse(feedback, input);
    return false;
}

void ease_buttons(float *values, int count, int focus, bool live, float dt)
{
    for (int i = 0; i < count; ++i)
    {
        const float target = live && i == focus ? 1.0f : 0.0f;
        values[i] += (target - values[i]) * (1.0f - std::exp(-18.0f * dt));
    }
}

// A numbered line: the number in a small glass disc, the words beside it (two lines at most).
float numbered(hui::gfx::DrawList &list, const ui::Type &type, int number, const std::string &words, float x,
               float baseline, float width)
{
    list.circle(x + 20.0f, baseline - 10.0f, 20.0f, ui::color::field_open);
    ui::text(list, type.semibold, std::to_string(number), x + 20.0f, baseline - 2.0f, 22.0f, ui::color::text,
             ui::Align::center);
    return ui::paragraph(list, type.regular, words, x + 60.0f, baseline, kBodySize, width - 60.0f, kBodyLine,
                         ui::color::text2, 2) +
           10.0f;
}

// A 6 px semantic dot with its word (colour never alone).
void state_word(hui::gfx::DrawList &list, const ui::Type &type, float x, float baseline, const char *word,
                ui::Color colour)
{
    list.circle(x + 6.0f, baseline - 8.0f, 6.0f, colour);
    ui::text(list, type.semibold, word, x + 22.0f, baseline, 22.0f, colour);
}
} // namespace

// ---- pure helpers ----

std::string update_size_text(double bytes)
{
    const double mb = std::max(0.0, bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        return tr("{n} GB", {{"n", one_decimal(mb / 1024.0)}});
    return tr("{n} MB", {{"n", one_decimal(mb)}});
}

std::string update_eta_text(double seconds)
{
    if (!(seconds > 0.0) || !std::isfinite(seconds))
        return std::string();
    if (seconds < 60.0)
        return tr("about {n} s left", {{"n", std::to_string(static_cast<long>(std::ceil(seconds)))}});
    if (seconds < 3600.0)
        return tr("about {n} min left", {{"n", std::to_string(static_cast<long>(std::ceil(seconds / 60.0)))}});
    return tr("about {n} h left", {{"n", std::to_string(static_cast<long>(std::ceil(seconds / 3600.0)))}});
}

std::string update_progress_text(const UpdateView &view)
{
    std::string out = view.total_bytes > 0.0
                          ? tr("{done} of {total}", {{"done", update_size_text(view.done_bytes)},
                                                     {"total", update_size_text(view.total_bytes)}})
                          : tr("{done} downloaded", {{"done", update_size_text(view.done_bytes)}});
    if (view.bytes_per_s > 0.0 && std::isfinite(view.bytes_per_s))
        out += " · " + tr("{rate}/s", {{"rate", update_size_text(view.bytes_per_s)}});
    const std::string eta = update_eta_text(view.eta_s);
    if (!eta.empty())
        out += " · " + eta;
    return out;
}

float update_fraction(const UpdateView &view)
{
    if (!(view.total_bytes > 0.0))
        return 0.0f;
    return static_cast<float>(std::clamp(view.done_bytes / view.total_bytes, 0.0, 1.0));
}

int update_current_step(const UpdateView &view)
{
    switch (view.kind)
    {
    case UpdateView::Kind::available:
        return -1;
    case UpdateView::Kind::downloading: // Check while the published checksum is read, then Download
        return step_index(view.step == UpdateView::Step::check ? UpdateView::Step::check
                                                                : UpdateView::Step::download);
    case UpdateView::Kind::checking: // the helper checks the finished download
        return step_index(UpdateView::Step::download);
    case UpdateView::Kind::ready_to_install: // downloaded and checked: Install is next
        return step_index(UpdateView::Step::install);
    case UpdateView::Kind::installing:
        return step_index(UpdateView::Step::install);
    case UpdateView::Kind::installed:
        return step_index(UpdateView::Step::restart);
    case UpdateView::Kind::manual:
    case UpdateView::Kind::failed:
    default:
        return step_index(view.step);
    }
}

std::string update_row_value(const UpdateView *view)
{
    if (view == nullptr)
        return tr("Up to date");
    if (view->kind == UpdateView::Kind::installed)
        return tr("Installed — restart to finish");
    return tr("{latest} available", {{"latest", view->latest}});
}

std::vector<UpdateButton> update_buttons(const UpdateView &view)
{
    std::vector<UpdateButton> out;
    const auto add = [&](const char *label, UpdateAction action, bool primary)
    { out.push_back({tr(label), action, primary}); };
    switch (view.kind)
    {
    case UpdateView::Kind::available:
        add("Update now", UpdateAction::update_now, true);
        add("Back", UpdateAction::back, false);
        break;
    case UpdateView::Kind::downloading:
    case UpdateView::Kind::checking:
        if (view.can_cancel)
            add("Cancel", UpdateAction::cancel, false);
        break;
    case UpdateView::Kind::ready_to_install:
        add("Install", UpdateAction::install, true);
        add("Cancel", UpdateAction::cancel, false);
        break;
    case UpdateView::Kind::installing:
        break;
    case UpdateView::Kind::installed:
        add("Quit Wings now", UpdateAction::quit_now, true);
        add("Back", UpdateAction::back, false);
        break;
    case UpdateView::Kind::manual:
        add("Back", UpdateAction::back, false);
        break;
    case UpdateView::Kind::failed:
        if (view.can_retry)
            add("Retry", UpdateAction::retry, true);
        add("Back", UpdateAction::back, false);
        break;
    }
    return out;
}

bool update_view_from_flow(const WingsFlowSnapshot &flow, UpdateView *out)
{
    using Kind = UpdateView::Kind;
    Kind kind = Kind::available;
    switch (flow.state)
    {
    case WingsFlowState::idle:
        return false;
    case WingsFlowState::available:
        kind = Kind::available;
        break;
    case WingsFlowState::downloading:
        kind = Kind::downloading;
        break;
    case WingsFlowState::checking:
        kind = Kind::checking;
        break;
    case WingsFlowState::ready_to_install:
        kind = Kind::ready_to_install;
        break;
    case WingsFlowState::installing:
        kind = Kind::installing;
        break;
    case WingsFlowState::installed:
        kind = Kind::installed;
        break;
    case WingsFlowState::manual:
        kind = Kind::manual;
        break;
    case WingsFlowState::failed:
        kind = Kind::failed;
        break;
    }
    UpdateView v;
    v.kind = kind;
    v.step = static_cast<UpdateView::Step>(std::clamp(flow.step_index, 0, kWingsFlowSteps - 1));
    v.current = flow.running_text;
    v.latest = flow.version_text;
    v.done_bytes = static_cast<double>(flow.bytes);
    v.total_bytes = static_cast<double>(flow.total);
    v.bytes_per_s = flow.speed_bps > 0.0 ? flow.speed_bps : 0.0;
    v.eta_s = flow.eta_s > 0 ? static_cast<double>(flow.eta_s) : 0.0;
    v.message = kind == Kind::failed && !flow.reason.empty() ? flow.reason : flow.text;
    for (const WingsInstallStep &step : flow.manual_steps)
        v.manual_steps.push_back(step.detail.empty() ? step.label : step.label + ": " + step.detail);
    v.can_cancel = flow.buttons.cancel;
    v.can_retry = flow.buttons.retry;
    *out = std::move(v);
    return true;
}

// ---- UpdateDialog ----

void UpdateDialog::open(const UpdateView &view)
{
    open_ = true;
    focus_ = 0;
    title_ = tr("Wings {latest} is available", {{"latest", view.latest}});
    notes_.assign(view.notes.begin(), view.notes.begin() + std::min<std::size_t>(3, view.notes.size()));
    in_.snap(0.0f);
}

UpdateAction UpdateDialog::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    UpdateAction action = UpdateAction::none;
    in_.target = open_ ? 1.0f : 0.0f;
    in_.update(dt, 18.0f);
    if (open_)
    {
        move_focus(focus_, 2, input, feedback);
        if (input.is_pressed(Action::confirm))
        {
            open_ = false;
            feedback.play(hui::audio::Cue::select);
            action = focus_ == 0 ? UpdateAction::update_now : UpdateAction::later;
        }
        else if (input.is_pressed(Action::back))
        {
            open_ = false;
            feedback.play(hui::audio::Cue::modal_close);
            action = UpdateAction::later;
        }
    }
    ease_buttons(button_focus_, 2, focus_, open_, dt);
    return action;
}

void UpdateDialog::draw(Context &ctx, Frame &frame) const
{
    if (!open_ && in_.value < 0.01f)
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx.type;
    const float a = std::clamp(in_.value, 0.0f, 1.0f);
    draw_scrim(list, a);
    const float width = 960.0f;
    const float notes_height = notes_.empty() ? 0.0f : 56.0f + static_cast<float>(notes_.size()) * 76.0f;
    const float height = 72.0f + 60.0f + notes_height + 40.0f + ui::kButtonHeight + 44.0f;
    const ui::Rect r{960.0f - width * 0.5f, 540.0f - height * 0.5f + (1.0f - a) * 20.0f, width, height};
    list.push_opacity(a);
    ui::modal(list, r);
    ui::text_fit(list, type.light, title_, r.x + 48.0f, r.y + 82.0f, 38.0f, r.w - 96.0f, ui::color::text);
    float y = r.y + 150.0f;
    if (!notes_.empty())
    {
        ui::text(list, type.semibold, tr("What's new"), r.x + 48.0f, y, 22.0f, ui::color::text3);
        y += 48.0f;
        for (const std::string &note : notes_)
        {
            list.circle(r.x + 56.0f, y - 9.0f, 4.0f, ui::color::text2);
            ui::paragraph(list, type.regular, note, r.x + 76.0f, y, 25.0f, r.w - 124.0f, 36.0f,
                          ui::color::text2, 2);
            y += 76.0f;
        }
    }
    const float by = r.y + r.h - 44.0f - ui::kButtonHeight;
    const ui::Rect later{r.x + r.w - 48.0f - kButtonWidth, by, kButtonWidth, ui::kButtonHeight};
    const ui::Rect now{later.x - 20.0f - kButtonWidth, by, kButtonWidth, ui::kButtonHeight};
    ui::button(list, type, now, tr("Update now"), ui::ButtonKind::primary, button_focus_[0]);
    ui::button(list, type, later, tr("Later"), ui::ButtonKind::secondary, button_focus_[1]);
    list.pop_opacity();
}

std::vector<hui::ui::Hint> UpdateDialog::hints() const
{
    return {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Later")}};
}

// ---- UpdatePage ----

void UpdatePage::open(const UpdateView &view)
{
    open_ = true;
    shown_step_ = -1;
    focus_ = 0;
    buttons_.clear();
    in_.snap(0.0f);
    take(view);
}

void UpdatePage::take(const UpdateView &view)
{
    // Never look like it went backwards: the shown step only moves on, except when the player starts again.
    shown_step_ = std::max(shown_step_, update_current_step(view));
    std::vector<UpdateButton> next = update_buttons(view);
    const bool same = next.size() == buttons_.size() &&
                      std::equal(next.begin(), next.end(), buttons_.begin(),
                                 [](const UpdateButton &l, const UpdateButton &r) { return l.action == r.action; });
    if (!same)
    {
        buttons_ = std::move(next);
        focus_ = 0; // the primary button (or the only one) takes the focus
        button_focus_.assign(buttons_.size(), 0.0f);
    }
    view_ = view;
}

UpdateAction UpdatePage::update(const UpdateView &view, const hui::InputFrame &input, float dt,
                                hui::ui::Feedback &feedback)
{
    UpdateAction action = UpdateAction::none;
    in_.target = open_ ? 1.0f : 0.0f;
    in_.update(dt, 14.0f);
    if (!open_)
        return action;
    take(view);

    const hui::InputFrame idle{};
    const bool had_confirm = quit_confirm_.is_open();
    const ConfirmDialog::Result result = quit_confirm_.update(had_confirm ? input : idle, dt, feedback);
    if (result == ConfirmDialog::Result::confirmed)
        action = UpdateAction::quit_now;
    if (!had_confirm)
    {
        move_focus(focus_, static_cast<int>(buttons_.size()), input, feedback);
        if (input.is_pressed(Action::confirm) && !buttons_.empty())
        {
            const UpdateAction chosen = buttons_[static_cast<std::size_t>(focus_)].action;
            feedback.play(hui::audio::Cue::select);
            if (chosen == UpdateAction::quit_now)
            {
                quit_confirm_.open(tr("Quit Wings now?"),
                                   tr("Wings closes. Open it again to start the new version."),
                                   tr("Quit Wings"), tr("Not yet"), false);
                feedback.play(hui::audio::Cue::modal_open);
            }
            else
            {
                action = chosen;
                if (chosen == UpdateAction::update_now || chosen == UpdateAction::retry)
                    shown_step_ = -1; // the player starts again: the steps may begin anew
            }
        }
        else if (input.is_pressed(Action::back))
        {
            feedback.play(hui::audio::Cue::back);
            action = UpdateAction::back;
        }
    }
    if (action == UpdateAction::back)
        open_ = false;
    for (std::size_t i = 0; i < button_focus_.size(); ++i)
    {
        const float target = !quit_confirm_.is_open() && static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
        button_focus_[i] += (target - button_focus_[i]) * (1.0f - std::exp(-18.0f * dt));
    }
    return action;
}

void UpdatePage::draw(Context &ctx, Frame &frame) const
{
    if (!open_ && in_.value < 0.01f)
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx.type;
    const float a = std::clamp(in_.value, 0.0f, 1.0f);
    list.push_opacity(a);
    // Opaque and neutral: the page covers the tab under it (chrome never carries a hue of its own).
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, ui::color::night1);

    draw_heading(ctx, list, tr("Wings update"),
                 view_.current.empty() ? std::string()
                                       : tr("You have {current} · New {latest}",
                                            {{"current", view_.current}, {"latest", view_.latest}}));

    // ---- the four steps ----
    const char *labels[4] = {tr("Check"), tr("Download"), tr("Install"), tr("Restart Wings")};
    const float left = ui::kGutter + 120.0f, right = 1920.0f - ui::kGutter - 120.0f;
    const float pitch = (right - left) / 3.0f;
    const int current = shown_step_;
    for (int i = 0; i < 3; ++i)
    {
        const float x1 = left + static_cast<float>(i) * pitch + kStepRadius + 12.0f;
        const float x2 = left + static_cast<float>(i + 1) * pitch - kStepRadius - 12.0f;
        list.line(x1, kStepsY, x2, kStepsY, 3.0f,
                  i < current ? ui::Color::rgb(0xffffff, 0.7f) : ui::color::line);
    }
    for (int i = 0; i < 4; ++i)
    {
        const float cx = left + static_cast<float>(i) * pitch;
        const bool done = i < current;
        const bool now = i == current;
        if (now)
            ui::focus_ring(list, {cx - kStepRadius, kStepsY - kStepRadius, 2.0f * kStepRadius, 2.0f * kStepRadius},
                           kStepRadius, 0.6f);
        list.circle(cx, kStepsY, kStepRadius,
                    done ? ui::Color::rgb(0xffffff, 0.85f) : now ? ui::color::text : ui::color::field_open);
        if (done)
            ui::icon_check(list, cx, kStepsY, 24.0f, ui::color::night0);
        else
            ui::text(list, type.semibold, std::to_string(i + 1), cx, kStepsY + 9.0f, 26.0f,
                     now ? ui::color::night0 : ui::color::text3, ui::Align::center);
        ui::text(list, now ? type.semibold : type.medium, labels[i], cx, kStepsY + 72.0f, 24.0f,
                 now || done ? ui::color::text : ui::color::text3, ui::Align::center);
    }

    // ---- the status block ----
    const ui::Rect card{ui::kGutter, kCardTop, 1920.0f - 2.0f * ui::kGutter, kCardHeight};
    ui::surface(list, card, ui::kPanelRadius, ui::color::field);
    const float x = card.x + 56.0f, width = card.w - 112.0f;
    float y = card.y + 84.0f;
    const auto title = [&](const std::string &words)
    {
        ui::text_fit(list, type.light, words, x, y, 46.0f, width, ui::color::text);
        y += 66.0f;
    };
    const auto body = [&](const std::string &words, int lines)
    { y = ui::paragraph(list, type.regular, words, x, y, kBodySize, width, kBodyLine, ui::color::text2, lines) + 8.0f; };
    switch (view_.kind)
    {
    case UpdateView::Kind::available:
        title(tr("Wings {latest} is available", {{"latest", view_.latest}}));
        if (!view_.notes.empty())
        {
            ui::text(list, type.semibold, tr("What's new"), x, y, 22.0f, ui::color::text3);
            y += 44.0f;
            for (std::size_t i = 0; i < view_.notes.size() && i < 3; ++i)
            {
                list.circle(x + 8.0f, y - 10.0f, 4.0f, ui::color::text2);
                ui::text_fit(list, type.regular, view_.notes[i], x + 28.0f, y, kBodySize, width - 28.0f,
                             ui::color::text2);
                y += kBodyLine;
            }
        }
        break;
    case UpdateView::Kind::downloading:
    case UpdateView::Kind::checking:
    {
        const bool checking = view_.kind == UpdateView::Kind::checking;
        title(checking ? std::string(tr("Checking the download…"))
                       : tr("Downloading Wings {latest}", {{"latest", view_.latest}}));
        // Checking comes after a whole download: the bar stays full, it never runs back.
        ui::progress_bar(list, {x, y, width, 12.0f}, checking ? 1.0f : update_fraction(view_), theme::semantic::info);
        y += 62.0f;
        body(checking ? std::string(tr("Making sure the file arrived whole and is the real release."))
                      : update_progress_text(view_),
             1);
        break;
    }
    case UpdateView::Kind::ready_to_install:
        title(tr("Ready to install"));
        body(tr("Wings {latest} is downloaded and checked. Install puts it in place in a few seconds. Your current "
                "version is kept until the new one runs, so nothing is lost if something goes wrong.",
                {{"latest", view_.latest}}),
             3);
        break;
    case UpdateView::Kind::installing:
    {
        title(tr("Installing…"));
        body(tr("This takes a few seconds. Don't turn off the console."), 2);
        for (int i = 0; i < 3; ++i)
        {
            const float phase = 0.5f + 0.5f * std::sin(ctx.clock * 5.0f - static_cast<float>(i) * 0.9f);
            list.circle(x + 8.0f + static_cast<float>(i) * 24.0f, y + 20.0f, 7.0f,
                        ui::Color::rgb(0xffffff, 0.3f + 0.7f * phase));
        }
        break;
    }
    case UpdateView::Kind::installed:
        state_word(list, type, x, y - 52.0f, tr("Installed"), theme::semantic::good);
        title(tr("Wings {latest} is installed", {{"latest", view_.latest}}));
        body(tr("Close Wings and open it again to use version {latest}.", {{"latest", view_.latest}}), 1);
        y += 6.0f;
        y = numbered(list, type, 1, tr("Quit Wings"), x, y, width);
        y = numbered(list, type, 2, tr("Open Wings again"), x, y, width);
        break;
    case UpdateView::Kind::manual:
        title(tr("Finish the update by hand"));
        if (!view_.message.empty())
            body(view_.message, 2);
        for (std::size_t i = 0; i < view_.manual_steps.size() && y < card.y + card.h - 120.0f; ++i)
            y = numbered(list, type, static_cast<int>(i) + 1, view_.manual_steps[i], x, y, width);
        break;
    case UpdateView::Kind::failed:
        state_word(list, type, x, y - 52.0f, tr("Failed"), theme::semantic::bad);
        title(tr("The update stopped"));
        body(view_.message.empty() ? std::string(tr("Something went wrong. Nothing was changed.")) : view_.message,
             4);
        break;
    }

    // ---- the buttons, along the foot of the block ----
    float bx = x;
    const float by = card.y + card.h - 48.0f - ui::kButtonHeight;
    for (std::size_t i = 0; i < buttons_.size(); ++i)
    {
        const UpdateButton &b = buttons_[i];
        const ui::ButtonKind kind = b.primary ? ui::ButtonKind::primary : ui::ButtonKind::secondary;
        const float w = std::max(kButtonWidth, ui::button_width(type, b.label, 20.0f, kind));
        ui::button(list, type, {bx, by, w, ui::kButtonHeight}, b.label, kind,
                   i < button_focus_.size() ? button_focus_[i] : 0.0f);
        bx += w + 20.0f;
    }
    ui::hints(list, type, hints());
    list.pop_opacity();
    quit_confirm_.draw(ctx, frame);
}

std::vector<hui::ui::Hint> UpdatePage::hints() const
{
    std::vector<hui::ui::Hint> items;
    if (!buttons_.empty())
        items.push_back({hui::ui::Button::cross, tr("Select")});
    items.push_back({hui::ui::Button::circle, tr("Back")});
    return items;
}

} // namespace orbit
