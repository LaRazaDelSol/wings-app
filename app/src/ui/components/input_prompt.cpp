// ps5-homebrew-ui - Component: InputPrompt.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/components/input_prompt.hpp"

#include "ui/components/action_glyph.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hui::ui
{

using gfx::Color;
using gfx::Rect;

namespace
{

constexpr float kSideMargin = 48.0f;
constexpr float kTitleLine = 1.25f; // line height of the title, in title sizes
constexpr int kCancel = 0;
constexpr int kDone = 1;
constexpr int kClear = 2; // only with style.clear_button; sits left of Cancel

} // namespace

// The prompt owns these knobs of its two parts; everything else on them is
// the user's.
void InputPrompt::sync()
{
    field.style.theme = style.theme;
    // The focus is on the keys: the field shows a caret, not a second ring.
    // (TextField has no knob for that; a clear focus colour does it.)
    field.style.theme.focus.a = 0.0f;
    field.style.sounds = style.sounds;
    field.style.reduced_motion = style.reduced_motion;
    field.style.max_length = style.max_length;
    field.style.password = style.password && !revealed_;
    field.style.on_page = false;

    keyboard.style.theme = style.theme;
    keyboard.style.sounds = style.sounds;
    // Done and back are the prompt's to answer: it validates first, then
    // plays one sound instead of two.
    keyboard.style.sounds.activate = audio::Cue::count;
    keyboard.style.sounds.cancel = audio::Cue::count;
    keyboard.style.reduced_motion = style.reduced_motion;
    keyboard.style.panel = false;
    keyboard.style.key_height = style.key_height;
    keyboard.style.gap = style.key_gap;
    keyboard.style.max_length = style.max_length;
    keyboard.style.auto_capital = style.auto_capital;
    keyboard.style.exits = {};
    keyboard.style.exits.down = style.buttons;
    keyboard.style.exits.up = !chips_.empty();
}

Rect InputPrompt::panel_rect() const
{
    const float width = std::max(std::min(style.width, bounds_.w - 2.0f * kSideMargin), 200.0f);
    float height =
        2.0f * style.padding + field.preferred_height() + style.gap + keyboard.preferred_height();
    if (!title_.empty())
        height += style.title_size * kTitleLine + style.gap * 0.6f;
    if (style.buttons)
        height += style.gap + style.button_height;
    if (!style.hints.empty())
        height += hints_height() + style.gap * 0.5f;
    height += chips_height();
    return {bounds_.cx() - width * 0.5f, bounds_.cy() - height * 0.5f, width, height};
}

void InputPrompt::place()
{
    const Rect panel = panel_rect();
    const float inner = panel.w - 2.0f * style.padding;
    float y = panel.y + style.padding;
    if (!title_.empty())
        y += style.title_size * kTitleLine + style.gap * 0.6f;
    const float field_height = field.preferred_height();
    field.set_bounds({panel.x + style.padding, y, inner, field_height});
    y += field_height + style.gap;
    y += chips_height();
    keyboard.set_bounds({panel.x + style.padding, y, inner, keyboard.preferred_height()});
}

int InputPrompt::button_count() const
{
    return style.clear_button ? 3 : 2;
}

// Left to right: [Clear] Cancel Done.
int InputPrompt::button_id(int slot) const
{
    if (style.clear_button)
        return slot == 0 ? kClear : (slot == 1 ? kCancel : kDone);
    return slot == 0 ? kCancel : kDone;
}

Rect InputPrompt::button_rect(int slot) const
{
    const Rect panel = panel_rect();
    const float inner = panel.w - 2.0f * style.padding;
    const float count = static_cast<float>(button_count());
    const float width =
        std::min(style.button_width, (inner - style.button_gap * (count - 1.0f)) / count);
    const float total = count * width + style.button_gap * (count - 1.0f);
    // The row ends on the right, where the eye finishes reading.
    return {panel.x + panel.w - style.padding - total +
                static_cast<float>(slot) * (width + style.button_gap),
            panel.y + panel.h - style.padding - style.button_height, width, style.button_height};
}

float InputPrompt::chips_height() const
{
    return chips_.empty() ? 0.0f : style.chip_height + style.gap;
}

// The chips in a row under the field. When they do not all fit, the row
// starts later so the focused one stays in view.
void InputPrompt::draw_chips(Canvas &canvas) const
{
    const Theme &theme = style.theme;
    Painter paint(canvas.list, canvas.fonts, theme, canvas.glass);
    const Rect f = field.bounds();
    const float y = f.y + f.h + style.gap;
    const float right = f.x + f.w;
    float x = f.x;
    if (!style.chips_label.empty())
    {
        const float w =
            paint.label(style.chips_label, x, y + style.chip_height * 0.5f + style.chip_size * 0.35f,
                        style.chip_size, theme.text_muted);
        x += w + style.chip_gap * 1.5f;
    }
    const float left = x;
    const auto width = [&](std::size_t i)
    {
        return std::min(paint.label_width(chips_[i], style.chip_size) + 32.0f, (right - left) * 0.6f);
    };
    std::size_t first = 0;
    const std::size_t focus = on_chips_ ? static_cast<std::size_t>(chip_) : 0;
    for (;;)
    {
        float used = 0.0f;
        for (std::size_t i = first; i <= focus && i < chips_.size(); ++i)
            used += width(i) + style.chip_gap;
        if (used - style.chip_gap <= right - left || first >= focus)
            break;
        ++first;
    }
    for (std::size_t i = first; i < chips_.size(); ++i)
    {
        const float w = width(i);
        if (x + w > right)
            break;
        const Rect r{x, y, w, style.chip_height};
        Look look;
        look.focus = on_chips_ && static_cast<int>(i) == chip_ ? 1.0f : 0.0f;
        paint.chip(r, fit_label(paint, chips_[i], style.chip_size, w - 24.0f), 0.0f, look);
        x += w + style.chip_gap;
    }
}

float InputPrompt::hints_height() const
{
    if (style.hints.empty())
        return 0.0f;
    const int columns = std::max(style.hint_columns, 1);
    const int rows = (static_cast<int>(style.hints.size()) + columns - 1) / columns;
    return static_cast<float>(rows) * style.hint_height;
}

// Every button the prompt answers to, with its glyph, in a grid of columns.
void InputPrompt::draw_hints(Canvas &canvas, const Rect &panel) const
{
    const Theme &theme = style.theme;
    Painter paint(canvas.list, canvas.fonts, theme, canvas.glass);
    const int columns = std::max(style.hint_columns, 1);
    const float inner = panel.w - 2.0f * style.padding;
    const float cell = inner / static_cast<float>(columns);
    const Rect keys = keyboard.bounds();
    const float top = keys.y + keys.h + style.gap * 0.5f;
    const float glyph = style.hint_size * 1.45f;
    for (std::size_t i = 0; i < style.hints.size(); ++i)
    {
        const PromptHint &hint = style.hints[i];
        const float x0 = panel.x + style.padding + cell * static_cast<float>(i % columns);
        const float cy = top + style.hint_height * (static_cast<float>(i / columns) + 0.5f);
        float x = x0;
        for (std::size_t a = 0; a < hint.actions.size() && a < 2; ++a)
        {
            draw_action_glyph(canvas, theme, hint.actions[a], x, cy, glyph);
            x += action_glyph_width(hint.actions[a], glyph) + 4.0f;
        }
        const float room = std::max(x0 + cell - x - 10.0f, 8.0f);
        paint.label(fit_label(paint, hint.label, style.hint_size, room), x + 4.0f,
                    cy + style.hint_size * 0.35f, style.hint_size, theme.text_muted);
    }
}

void InputPrompt::open(Feedback &feedback, std::string_view initial)
{
    revealed_ = false;
    sync();
    if (!visible())
    {
        fade_.snap(0.0f);
        pop_.snap(0.0f);
    }
    open_ = true;
    on_buttons_ = false;
    on_chips_ = false;
    chip_ = 0;
    button_ = button_count() - 1;
    pressed_ = -1;
    press_.value = 0.0f;
    field.set_text(initial);
    field.set_error("");
    edits_ = 0;
    keyboard.set_layout(0);
    keyboard.set_shift(KeyboardShift::off);
    keyboard.set_length(field.length());
    place();
    // The middle of the board is the shortest way to anywhere.
    const int rows =
        keyboard.layouts().empty()
            ? 0
            : static_cast<int>(
                  keyboard.layouts()[static_cast<std::size_t>(keyboard.layout())].rows.size());
    keyboard.set_focus(rows / 2, 4, true);
    keyboard.enter();
    highlight_.snap(button_rect(button_));
    play_cue(feedback, style, style.sounds.open, bounds_.cx());
}

void InputPrompt::close(Feedback &feedback)
{
    if (!open_)
        return;
    open_ = false;
    play_cue(feedback, style, style.sounds.close, bounds_.cx());
}

void InputPrompt::dismiss()
{
    open_ = false;
    fade_.snap(0.0f);
    pop_.snap(0.0f);
}

bool InputPrompt::visible() const
{
    return open_ || fade_.value > 0.004f;
}

Event InputPrompt::submit(Feedback &feedback)
{
    if (style.trim)
    {
        // Backspace works at the caret: bring it to the end first, or trailing
        // spaces behind a caret moved to the start would never go away.
        field.set_caret_end();
        while (!field.text().empty() && field.text().back() == ' ')
        {
            if (!field.backspace())
                break;
        }
    }
    if (field.text().empty() && !style.allow_empty)
    {
        // The field shakes and says why; the prompt stays open.
        field.set_error(style.empty_error);
        return refuse(feedback, style, InputFrame{}, highlight_.refusal(), bounds_.cx());
    }
    pressed_ = kDone;
    press_.trigger();
    // The answer's own cue is the goodbye: no closing sound on top of it.
    play_cue(feedback, style, style.sounds.activate, bounds_.cx());
    if (style.sounds.rumble > 0.0f)
        feedback.rumble(0.3f * style.sounds.rumble, 0.04f);
    open_ = false;
    return Event::activated;
}

Event InputPrompt::cancel(Feedback &feedback)
{
    pressed_ = kCancel;
    press_.trigger();
    close(feedback);
    return Event::cancelled;
}

Event InputPrompt::handle(const InputFrame &input, Feedback &feedback)
{
    if (!open_)
        return Event::none;
    if (input.is_pressed(Action::back))
        return cancel(feedback);
    if (style.submit_on_menu && input.is_pressed(Action::menu))
        return submit(feedback);
    if (style.password && style.reveal_key != Action::count && input.is_pressed(style.reveal_key))
    {
        revealed_ = !revealed_;
        sync();
        play_cue(feedback, style, style.sounds.move, field.bounds().cx(), revealed_ ? 1.04f : 0.96f);
        return Event::moved;
    }

    if (on_chips_)
    {
        const int last = static_cast<int>(chips_.size()) - 1;
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = chip_ + (input.nav == Direction::right ? 1 : -1);
            if (next < 0 || next > last)
                return refuse(feedback, style, input, highlight_.refusal(), bounds_.cx());
            chip_ = next;
            play_cue(feedback, style, style.sounds.move, bounds_.cx(), 1.0f);
            return Event::moved;
        }
        if (input.nav == Direction::down)
        {
            on_chips_ = false;
            play_cue(feedback, style, style.sounds.move, bounds_.cx(), 0.96f);
            return Event::moved;
        }
        if (input.is_pressed(Action::confirm) && chip_ >= 0 && chip_ <= last)
        {
            field.set_text(chips_[static_cast<std::size_t>(chip_)]);
            keyboard.set_length(field.length());
            ++edits_;
            return submit(feedback);
        }
        return Event::none;
    }

    if (!on_buttons_)
    {
        keyboard.set_length(field.length());
        const Event event = keyboard.handle(input, feedback);
        bool edited = false;
        if (keyboard.cleared())
        {
            edited = !field.text().empty();
            field.clear();
        }
        for (int i = 0; i < keyboard.erased(); ++i)
            edited = field.backspace() || edited;
        if (!keyboard.typed().empty())
            edited = field.insert(keyboard.typed()) || edited;
        if (edited)
        {
            // Typing answers the complaint about an empty text.
            field.set_error("");
            keyboard.set_length(field.length());
            ++edits_;
            return Event::changed;
        }
        if (event == Event::activated)
            return submit(feedback);
        if (style.caret_keys)
        {
            const int step = input.is_pressed(Action::page_prev)   ? -1
                             : input.is_pressed(Action::page_next) ? 1
                                                                   : 0;
            if (step != 0)
            {
                if (!field.move_caret(step))
                    return refuse(feedback, style, input, highlight_.refusal(), bounds_.cx());
                play_cue(feedback, style, style.sounds.move, field.bounds().cx(),
                         step < 0 ? 0.96f : 1.04f);
                return Event::moved;
            }
        }
        if (event == Event::none && keyboard.exit() == Direction::up && !chips_.empty())
        {
            on_chips_ = true;
            chip_ = 0;
            play_cue(feedback, style, style.sounds.move, bounds_.cx(), 1.04f);
            return Event::moved;
        }
        if (event == Event::none && keyboard.exit() == Direction::down && style.buttons)
        {
            on_buttons_ = true;
            button_ = button_count() - 1;
            highlight_.snap(button_rect(button_));
            play_cue(feedback, style, style.sounds.move, button_rect(button_).cx(), 0.94f);
            return Event::moved;
        }
        return event;
    }

    const float x = button_rect(button_).cx();
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int next = button_ + (input.nav == Direction::right ? 1 : -1);
        if (next < 0 || next >= button_count())
            return refuse(feedback, style, input, highlight_.refusal(), x);
        button_ = next;
        highlight_.target(button_rect(button_));
        play_cue(feedback, style, style.sounds.move, button_rect(button_).cx());
        return Event::moved;
    }
    if (input.nav == Direction::up)
    {
        on_buttons_ = false;
        play_cue(feedback, style, style.sounds.move, x, 1.04f);
        return Event::moved;
    }
    if (input.nav == Direction::down)
        return refuse(feedback, style, input, highlight_.refusal(), x);
    if (input.is_pressed(Action::confirm))
    {
        const int id = button_id(button_);
        if (id == kDone)
            return submit(feedback);
        if (id == kClear)
        {
            if (field.text().empty())
                return refuse(feedback, style, input, highlight_.refusal(), x);
            field.clear();
            keyboard.set_length(0);
            pressed_ = kClear;
            press_.trigger();
            ++edits_;
            play_cue(feedback, style, field.style.erase, x, 0.9f);
            return Event::changed;
        }
        return cancel(feedback);
    }
    return Event::none;
}

void InputPrompt::update(float dt)
{
    sync();
    place();
    const float omega = style.omega();
    if (open_)
    {
        fade_.target = 1.0f;
        fade_.update(dt, omega * 1.2f);
        pop_.target = 1.0f;
        pop_.update(dt, omega, style.damping());
    }
    else
    {
        // Leaving is quicker and never bounces: the player has moved on.
        const float out = omega * std::max(style.exit_speed, 0.1f);
        fade_.target = 0.0f;
        fade_.update(dt, out);
        pop_.target = 0.0f;
        pop_.update(dt, out, 1.0f);
    }
    field.set_active(open_);
    keyboard.set_active(open_ && !on_buttons_ && !on_chips_);
    field.update(dt);
    keyboard.update(dt);
    buttons_focus_.target = open_ && on_buttons_ ? 1.0f : 0.0f;
    buttons_focus_.update(dt, 18.0f);
    highlight_.target(button_rect(button_));
    highlight_.update(dt, style);
    press_.update(dt, 7.0f);
}

void InputPrompt::draw(Canvas &canvas) const
{
    if (!visible())
        return;
    const Theme &theme = style.theme;
    gfx::DrawList &list = canvas.list;
    const float fade = tween::clamp01(fade_.value);
    list.rounded_rect(bounds_, 0.0f, style.scrim_color.with_alpha(style.scrim * fade));

    const Rect panel = panel_rect();
    const bool still = style.reduced_motion;
    const float pop = pop_.value;
    list.push_opacity(tween::clamp01(fade * 1.5f));
    list.push_transform(still ? 1.0f : tween::lerp(style.enter_scale, 1.0f, pop), panel.cx(),
                        panel.cy(), 0.0f, still ? 0.0f : style.enter_rise * (1.0f - pop));

    draw_overlay_panel(canvas, theme, panel, style.frosted, style.frost);
    Painter paint(list, canvas.fonts, theme, canvas.glass);
    const float inner = panel.w - 2.0f * style.padding;
    if (!title_.empty())
    {
        const std::vector<std::string> title =
            wrap_heading(canvas, theme, title_, style.title_size, inner, 1);
        if (!title.empty())
            paint.heading(title[0], panel.x + style.padding,
                          panel.y + style.padding + style.title_size * 0.88f, style.title_size,
                          theme.text);
    }
    field.draw(canvas);
    keyboard.draw(canvas);
    if (!style.hints.empty())
        draw_hints(canvas, panel);
    if (!chips_.empty())
        draw_chips(canvas);

    if (style.buttons)
    {
        for (int slot = 0; slot < button_count(); ++slot)
        {
            const int i = button_id(slot);
            const Rect r = button_rect(slot);
            Look look;
            look.press = i == pressed_ ? tween::clamp01(press_.value) : 0.0f;
            const std::string &label = i == kDone    ? style.done_label
                                       : i == kClear ? style.clear_label
                                                     : style.cancel_label;
            paint.button(r, fit_label(paint, label, 24.0f, r.w - 28.0f),
                         i == kDone ? ButtonKind::primary : ButtonKind::secondary, look);
        }
        // One ring that glides between the two answers.
        HighlightStyle ring;
        ring.kind = HighlightKind::ring;
        highlight_.draw(canvas, style, ring, tween::clamp01(buttons_focus_.value));
    }

    list.pop_transform();
    list.pop_opacity();
}

} // namespace hui::ui
