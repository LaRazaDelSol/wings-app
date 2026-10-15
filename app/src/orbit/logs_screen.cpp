// Wings TV app - The Logs tab: Pegasus DL's log and Wings' own diagnostics.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/screens.hpp"

#include "orbit/busy_cancel.hpp"
#include "orbit/diagnostics.hpp"
#include "orbit/model.hpp"

#include <algorithm>
#include <cmath>

namespace orbit
{

namespace
{
constexpr float kRow = 60.0f;
constexpr float kListEnd = 940.0f;
const InputFrame kIdle{};

int level_rank(const std::string &level)
{
    if (level == "error" || level == "fatal")
        return 2;
    if (level == "warn" || level == "warning")
        return 1;
    return 0;
}

ui::Color level_color(const std::string &level)
{
    const int rank = level_rank(level);
    return rank == 2 ? ui::color::tier_red
                     : (rank == 1 ? ui::color::tier_yellow : ui::color::text3);
}
} // namespace

LogsScreen::LogsScreen(Context &context) : Screen(context)
{
}

void LogsScreen::focus()
{
    follow_ = true;
}

bool LogsScreen::modal() const
{
    return confirm_.is_open() || list_.is_open() ||
           ctx_.store.lane_state(Store::lane_admin).busy;
}

void LogsScreen::set_level(Level level)
{
    level_ = level;
    seen_serial_ = ~0ull;
    follow_ = true;
}

void LogsScreen::show_diagnostics(bool on)
{
    diagnostics_ = on;
    index_ = 0;
    seen_serial_ = ~0ull;
}

void LogsScreen::rebuild()
{
    const Snapshot &state = ctx_.store.state();
    shown_.clear();
    const int minimum = level_ == Level::all ? 0 : (level_ == Level::warnings ? 1 : 2);
    for (std::size_t i = 0; i < state.logs.size(); ++i)
    {
        if (level_rank(state.logs[i].level) >= minimum)
            shown_.push_back(static_cast<int>(i));
    }
    diag_ = diagnostics_lines();
}

std::vector<std::string> LogsScreen::diagnostics_lines() const
{
    std::string text = ctx_.diagnostics_text;
    if (text.empty())
    {
        // The preview and the tests have no diagnostics writer: say what is known.
        const Snapshot &s = ctx_.store.state();
        text = "Wings diagnostics\n";
        text += "App          version " + (ctx_.app_version.empty() ? std::string("-") : ctx_.app_version) + "\n";
        text += "Pegasus DL   state " +
                std::string(s.link == Link::online ? "online" : (s.link == Link::offline ? "waiting" : "connecting")) +
                ", address " + s.endpoint + "\n";
        if (s.have_info)
            text += "             version " + s.info.version + ", " + s.info.compat + ", adapter " +
                    s.info.adapter + "\n";
        if (s.have_catalog)
            text += "             catalogue " + std::to_string(s.packages.size()) + " packages\n";
        text += "             queue " + std::to_string(s.jobs.size()) + " jobs\n";
        text += "(The full report with cover art and memory lines is written on the console.)\n";
    }
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

void LogsScreen::open_menu()
{
    std::vector<ListItem> items;
    menu_codes_.clear();
    const auto add = [&](const char *code, std::string label, std::string detail, std::string right = {},
                         bool enabled = true)
    {
        ListItem item;
        item.label = std::move(label);
        item.detail = std::move(detail);
        item.right = std::move(right);
        item.enabled = enabled;
        items.push_back(std::move(item));
        menu_codes_.push_back(code);
    };
    add("all", tr("Show everything"), tr("Information, warnings and errors"),
        level_ == Level::all ? std::string(tr("Current")) : std::string(), !diagnostics_);
    add("warnings", tr("Warnings and errors"), tr("Hide plain information"),
        level_ == Level::warnings ? std::string(tr("Current")) : std::string(), !diagnostics_);
    add("errors", tr("Errors only"), tr("Only what went wrong"),
        level_ == Level::errors ? std::string(tr("Current")) : std::string(), !diagnostics_);
    add("follow", follow_ ? tr("Pause following") : tr("Follow the newest line"),
        tr("While following, the list stays at the newest line"), {}, !diagnostics_);
    add("view", diagnostics_ ? tr("Show Pegasus DL's log") : tr("Show Wings diagnostics"),
        tr("The report this app writes about itself"));
    add("clear", tr("Clear Pegasus DL's log\xE2\x80\xA6"),
        tr("Empties the log held by Pegasus DL"), {}, !diagnostics_);
    list_.open(tr("Logs"), tr("Choose a level, pause, or clear"), std::move(items),
               ListDialog::Mode::single);
}

Intent LogsScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    if (!diagnostics_)
        ctx_.store.want_logs(true);
    // Circle on "Asking Pegasus DL...": back at once, the late answer is ignored (B-18).
    if (ctx_.store.lane_state(Store::lane_admin).busy && input.is_pressed(Action::back))
    {
        ctx_.store.cancel(Store::lane_admin);
        return intent;
    }
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();
    const bool busy = ctx_.store.lane_state(Store::lane_admin).busy;

    // Rebuild when new lines arrived (or the diagnostics text may have changed).
    if (state.serial != seen_serial_)
    {
        seen_serial_ = state.serial;
        rebuild();
    }

    const ConfirmDialog::Result confirmed = confirm_.update(had_confirm ? input : kIdle, dt, feedback);
    if (confirmed == ConfirmDialog::Result::confirmed && pending_ == Pending::clear)
    {
        ctx_.store.call<bool>(
            Store::lane_admin, [](Backend &b) { return b.logs_clear(); },
            [this](Result<bool> &&r)
            {
                if (!r.ok())
                {
                    pending_ = Pending::notice;
                    confirm_.notice(tr("The log could not be cleared"), r.error);
                    return;
                }
                ctx_.store.clear_logs_local();
                toast_ = tr("Pegasus DL's log cleared");
            });
    }
    if (confirmed != ConfirmDialog::Result::none)
        pending_ = Pending::none;

    const ListDialog::Result listed = list_.update(had_list ? input : kIdle, dt, feedback);
    if (listed == ListDialog::Result::chosen)
    {
        const int chosen = list_.chosen();
        if (chosen >= 0 && chosen < static_cast<int>(menu_codes_.size()))
        {
            const std::string &code = menu_codes_[static_cast<std::size_t>(chosen)];
            if (code == "all")
                set_level(Level::all);
            else if (code == "warnings")
                set_level(Level::warnings);
            else if (code == "errors")
                set_level(Level::errors);
            else if (code == "follow")
                follow_ = !follow_;
            else if (code == "view")
                show_diagnostics(!diagnostics_);
            else if (code == "clear")
            {
                pending_ = Pending::clear;
                confirm_.open(tr("Clear Pegasus DL's log?"),
                              tr("The log lines held by Pegasus DL are removed. Downloads and "
                                 "settings are not affected."),
                              tr("Clear"), tr("Keep"), true);
            }
        }
    }

    const int count = diagnostics_ ? static_cast<int>(diag_.size()) : static_cast<int>(shown_.size());
    const bool panel = had_confirm || had_list || busy;
    if (focused && !panel)
    {
        if (input.nav == Direction::up)
        {
            if (index_ > 0)
            {
                --index_;
                follow_ = false;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                intent.kind = Intent::Kind::top_bar;
            }
        }
        else if (input.nav == Direction::down)
        {
            if (index_ + 1 < count)
            {
                ++index_;
                if (index_ + 1 >= count)
                    follow_ = true; // reached the newest line: follow again
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
        {
            refuse(feedback, input);
        }
        if (input.is_pressed(Action::confirm))
        {
            open_menu();
            feedback.play(hui::audio::Cue::modal_open);
        }
        else if (input.is_pressed(Action::north))
        {
            show_diagnostics(!diagnostics_);
            feedback.play(hui::audio::Cue::tab);
        }
        else if (input.is_pressed(Action::west) && !diagnostics_)
        {
            follow_ = !follow_;
            feedback.play(hui::audio::Cue::toggle);
        }
    }
    // Following keeps the newest line in view.
    if (follow_ && !diagnostics_ && count > 0)
        index_ = count - 1;
    index_ = std::clamp(index_, 0, std::max(0, count - 1));
    const float visible = kListEnd - layout::kListTop;
    scroll_.target = std::max(0.0f, static_cast<float>(index_ + 1) * kRow - visible);
    scroll_.update(dt, follow_ ? 14.0f : 11.0f);
    if (!toast_.empty())
    {
        intent.toast = toast_;
        toast_.clear();
    }
    return intent;
}

void LogsScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    std::string summary;
    if (diagnostics_)
        summary = tr("Wings diagnostics");
    else if (state.online())
    {
        const char *level = level_ == Level::all ? tr("All levels")
                            : level_ == Level::warnings ? tr("Warnings and errors")
                                                        : tr("Errors only");
        summary = trn(static_cast<long>(shown_.size()), "{count} line", "{count} lines") + "  \xC2\xB7  " +
                  level + (follow_ ? "  \xC2\xB7  " + std::string(tr("following")) : std::string());
    }
    draw_heading(ctx_, list, tr("Logs"), summary);
    if (!diagnostics_ && !state.online())
    {
        draw_waiting(ctx_, list, 330.0f);
    }
    else
    {
        const float width = 1920.0f - 2.0f * ui::kGutter;
        const int count = diagnostics_ ? static_cast<int>(diag_.size()) : static_cast<int>(shown_.size());
        if (count == 0)
            ui::text(list, type.regular,
                     diagnostics_ ? std::string(tr("No diagnostics yet."))
                                  : std::string(tr("No log lines yet.")),
                     ui::kGutter, layout::kListTop + 40.0f, 26.0f, ui::color::text2);
        // The component column is as wide as the widest visible component, so every
        // message on screen starts at the same x.
        float component_w = 0.0f;
        if (!diagnostics_)
        {
            for (int i = 0; i < count; ++i)
            {
                const float y = layout::kListTop + static_cast<float>(i) * kRow - scroll_.value;
                if (y + kRow < layout::kListTop - 20.0f || y > kListEnd + 20.0f)
                    continue;
                const LogLine &line = state.logs[static_cast<std::size_t>(shown_[static_cast<std::size_t>(i)])];
                component_w = std::max(component_w, type.regular.measure(line.component, 19.0f));
            }
            component_w = std::min(component_w, 220.0f);
        }
        list.push_clip({0.0f, layout::kListTop - 16.0f, 1920.0f, kListEnd - layout::kListTop + 32.0f});
        for (int i = 0; i < count; ++i)
        {
            const float y = layout::kListTop + static_cast<float>(i) * kRow - scroll_.value;
            if (y + kRow < layout::kListTop - 20.0f || y > kListEnd + 20.0f)
                continue;
            const ui::Rect r{ui::kGutter, y, width, kRow - 8.0f};
            const bool current = focused && i == index_ && !follow_;
            ui::surface(list, r, 12.0f, ui::Color::rgb(0xffffff, current ? 0.14f : 0.06f));
            if (diagnostics_)
            {
                ui::text_fit(list, type.regular, diag_[static_cast<std::size_t>(i)], r.x + 24.0f,
                             r.cy() + 8.0f, 22.0f, r.w - 48.0f, ui::color::text);
                continue;
            }
            const LogLine &line = state.logs[static_cast<std::size_t>(shown_[static_cast<std::size_t>(i)])];
            const ui::Color c = level_color(line.level);
            list.rounded_rect({r.x + 8.0f, r.y + 10.0f, 5.0f, r.h - 20.0f}, 2.5f, c);
            float x = r.x + 28.0f;
            // The level is written out; the colour is a second cue.
            const std::string label = line.level.empty() ? std::string("info") : line.level;
            x += ui::chip(list, type, x, r.cy(), label,
                          level_rank(line.level) == 0 ? ui::Color::rgb(0xffffff, 0.08f) : c.with_alpha(0.17f),
                          c, 15.0f,
                          level_rank(line.level) == 0 ? ui::Color{0, 0, 0, 0} : c.with_alpha(0.6f)) + 14.0f;
            x = std::max(x, r.x + 28.0f + 78.0f); // a fixed level column
            if (!line.component.empty())
                ui::text_fit(list, type.regular, line.component, x, r.cy() + 7.0f, 19.0f, component_w + 1.0f,
                             current ? ui::color::text2 : ui::color::text3);
            if (component_w > 0.0f)
                x += component_w + 18.0f;
            ui::text_fit(list, type.regular, line.message, x, r.cy() + 8.0f, 21.0f, r.x + r.w - x - 24.0f,
                         ui::color::text);
        }
        list.pop_clip();
    }
    if (ctx_.store.lane_state(Store::lane_admin).busy)
        draw_busy(ctx_, frame.overlay, with_stop_hint(tr("Asking Pegasus DL\xE2\x80\xA6")).c_str());
    confirm_.draw(ctx_, frame);
    list_.draw(ctx_, frame);
}

std::vector<hui::ui::Hint> LogsScreen::hints() const
{
    std::vector<hui::ui::Hint> items = {{hui::ui::Button::cross, tr("Options")},
                                        {hui::ui::Button::triangle, diagnostics_ ? tr("Pegasus log") : tr("Diagnostics")}};
    if (!diagnostics_)
        items.push_back({hui::ui::Button::square, follow_ ? tr("Pause") : tr("Follow")});
    items.push_back({hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1});
    return items;
}

} // namespace orbit
