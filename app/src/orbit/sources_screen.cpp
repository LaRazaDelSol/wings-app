// Wings TV app - The Sources tab: the catalogues Pegasus DL reads packages from.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flows follow Pegasus DL's own sources panel (recon/pegasus-app.js: add by
// URL, enable, refresh, delete with confirmation, default-source onboarding).

#include "orbit/screens.hpp"

#include "orbit/busy_cancel.hpp"
#include "orbit/model.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace orbit
{

namespace
{
constexpr float kRow = 92.0f;
constexpr float kPitch = 104.0f;
const InputFrame kIdle{};
} // namespace

void SourcesScreen::focus()
{
}

bool SourcesScreen::modal() const
{
    return prompt_.is_open() || confirm_.is_open() || list_.is_open() ||
           ctx_.store.source_action().busy;
}

void SourcesScreen::open_add_url()
{
    TextPrompt::Options options;
    options.max_length = 500;
    options.placeholder = "https://";
    options.done_label = tr("Add source");
    Feedback quiet;
    prompt_.open(quiet, tr("Add a source by address"), "https://", options);
}

void SourcesScreen::open_defaults()
{
    const Snapshot &state = ctx_.store.state();
    if (!state.have_defaults || !state.defaults.available || state.defaults.sources.empty())
    {
        pending_ = Pending::notice;
        confirm_.notice(tr("No suggested sources"),
                        state.defaults.error.empty()
                            ? std::string(tr("Pegasus DL has no suggested sources to offer right now."))
                            : state.defaults.error);
        return;
    }
    std::vector<ListItem> items;
    default_ids_.clear();
    for (const DefaultSource &source : state.defaults.sources)
    {
        if (source.configured)
            continue;
        ListItem item;
        item.label = source.name;
        item.detail = source.description;
        item.right = trn(source.package_count, "{count} package", "{count} packages");
        item.checked = source.default_enabled;
        items.push_back(std::move(item));
        default_ids_.push_back(source.id);
    }
    if (items.empty())
    {
        pending_ = Pending::notice;
        confirm_.notice(tr("All suggested sources are added"),
                        tr("Every source Pegasus DL suggests is already in your list."));
        return;
    }
    list_for_ = ListFor::defaults;
    list_.open(tr("Suggested sources"),
               tr("Pegasus DL does not include any packages by itself. Add the catalogues you want."),
               std::move(items), ListDialog::Mode::multi, tr("Add selected"));
}

void SourcesScreen::open_menu(const Source &source)
{
    std::vector<ListItem> items;
    menu_codes_.clear();
    menu_source_ = source.id;
    const auto add = [&](const char *code, const char *label, const char *detail)
    {
        ListItem item;
        item.label = label;
        item.detail = detail;
        items.push_back(std::move(item));
        menu_codes_.push_back(code);
    };
    add(source.enabled ? "off" : "on", source.enabled ? tr("Turn off") : tr("Turn on"),
        source.enabled ? tr("Its packages leave the Store") : tr("Its packages return to the Store"));
    add("refresh", tr("Refresh now"), tr("Read the catalogue again"));
    if (source.deletable)
        add("delete", tr("Delete"), tr("Remove this source from Pegasus DL"));
    list_for_ = ListFor::menu;
    list_.open(source.name, source.url, std::move(items), ListDialog::Mode::single);
}

void SourcesScreen::run_menu(const std::string &code, const Source &source, Feedback &feedback)
{
    if (code == "on" || code == "off")
        ctx_.store.source_enable(source.id, code == "on");
    else if (code == "refresh")
        ctx_.store.source_refresh(source.id);
    else if (code == "delete")
    {
        pending_ = Pending::delete_source;
        confirm_.open(tr("Delete this source?"),
                      tr("{name} is removed from Pegasus DL together with its {count} packages. "
                         "Downloads already queued are not affected.",
                         {{"name", source.name}, {"count", std::to_string(source.package_count)}}),
                      tr("Delete"), tr("Keep it"), true);
        feedback.play(hui::audio::Cue::modal_open);
    }
}

Intent SourcesScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    const int count = static_cast<int>(state.sources.size());
    index_ = std::clamp(index_, 0, std::max(0, count - 1));

    // Circle on "Asking Pegasus DL...": back at once, the late answer is ignored (B-18).
    if (ctx_.store.source_action().busy && input.is_pressed(Action::back))
    {
        ctx_.store.cancel(Store::lane_sources);
        return intent;
    }
    const bool had_prompt = prompt_.is_open();
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();
    const bool busy = ctx_.store.source_action().busy;

    const TextPrompt::Result typed = prompt_.update(had_prompt ? input : kIdle, dt, feedback);
    if (typed == TextPrompt::Result::accepted)
        ctx_.store.source_add_url(prompt_.text());
    const ConfirmDialog::Result confirmed =
        confirm_.update(had_confirm ? input : kIdle, dt, feedback);
    if (confirmed == ConfirmDialog::Result::confirmed && pending_ == Pending::delete_source)
        ctx_.store.source_delete(menu_source_);
    if (confirmed != ConfirmDialog::Result::none)
        pending_ = Pending::none;
    const ListDialog::Result listed = list_.update(had_list ? input : kIdle, dt, feedback);
    if (listed == ListDialog::Result::chosen && list_for_ == ListFor::menu)
    {
        for (const Source &source : state.sources)
        {
            if (source.id == menu_source_ && list_.chosen() >= 0 &&
                list_.chosen() < static_cast<int>(menu_codes_.size()))
                run_menu(menu_codes_[static_cast<std::size_t>(list_.chosen())], source, feedback);
        }
    }
    else if (listed == ListDialog::Result::confirmed && list_for_ == ListFor::defaults)
    {
        std::vector<std::string> ids;
        for (const int item : list_.checked())
        {
            if (item >= 0 && item < static_cast<int>(default_ids_.size()))
                ids.push_back(default_ids_[static_cast<std::size_t>(item)]);
        }
        ctx_.store.apply_defaults(ids);
    }
    else if (listed == ListDialog::Result::cancelled && list_for_ == ListFor::defaults &&
             state.defaults.onboarding_required)
    {
        // "Not now": Pegasus stops offering the first-run list.
        ctx_.store.dismiss_defaults();
    }

    // What the last source action did.
    const ActionState &action = ctx_.store.source_action();
    if (action.serial != seen_source_action_)
    {
        seen_source_action_ = action.serial;
        if (!action.error.empty())
        {
            pending_ = Pending::notice;
            confirm_.notice(tr("Pegasus DL could not do that"), action.error);
        }
        else if (!action.message.empty())
        {
            intent.toast = tr(action.message.c_str());
        }
    }

    // First run: Pegasus offers its suggested list once; show it when the tab opens.
    if (focused && !onboarding_offered_ && state.online() && state.have_defaults &&
        state.defaults.onboarding_required && state.defaults.available && !state.defaults.sources.empty() &&
        !prompt_.is_open() && !confirm_.is_open() && !list_.is_open())
    {
        onboarding_offered_ = true;
        open_defaults();
        if (list_.is_open())
            feedback.play(hui::audio::Cue::modal_open);
    }

    const bool panel = had_prompt || had_confirm || had_list || busy;
    if (focused && !panel && state.online())
    {
        if (input.nav == Direction::up)
        {
            if (index_ > 0)
            {
                --index_;
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
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            open_menu(state.sources[static_cast<std::size_t>(index_)]);
            feedback.play(hui::audio::Cue::modal_open);
        }
        else if (input.is_pressed(Action::confirm) && count == 0)
        {
            open_defaults();
        }
        if (input.is_pressed(Action::west))
        {
            open_add_url();
            feedback.play(hui::audio::Cue::modal_open);
        }
        if (input.is_pressed(Action::north))
        {
            ctx_.store.refresh_defaults(true);
            open_defaults();
        }
    }
    else if (focused && !state.online() && input.nav == Direction::up)
    {
        intent.kind = Intent::Kind::top_bar;
    }

    row_focus_.resize(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i)
    {
        float &value = row_focus_[static_cast<std::size_t>(i)];
        const float target = focused && i == index_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    scroll_.target = std::max(0.0f, static_cast<float>(index_ + 1) * kPitch -
                                        (layout::kListBottom - layout::kListTop));
    scroll_.update(dt, 11.0f);
    return intent;
}

void SourcesScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    std::string summary;
    if (state.online() && state.have_sources)
        summary = trn(static_cast<long>(state.sources.size()), "{count} source", "{count} sources");
    draw_heading(ctx_, list, tr("Sources"), summary);
    if (!state.online())
    {
        draw_waiting(ctx_, list, 330.0f);
    }
    else if (state.sources.empty())
    {
        const ui::Rect card{ui::kGutter, 300.0f, 1920.0f - 2.0f * ui::kGutter, 230.0f};
        ui::modal(list, card);
        ui::text(list, type.light, tr("No sources yet"), card.x + 48.0f, card.y + 86.0f, 40.0f,
                 ui::color::text);
        ui::paragraph(list, type.regular,
                      tr("A source is a catalogue address Pegasus DL reads packages from. Press "
                         "Cross to pick from the suggested list, or Square to type an address."),
                      card.x + 48.0f, card.y + 136.0f, 24.0f, card.w - 96.0f, 34.0f,
                      ui::color::text2, 3);
    }
    else
    {
        list.push_clip({0.0f, layout::kListTop - 16.0f, 1920.0f,
                        layout::kListBottom - layout::kListTop + 32.0f});
        const float width = 1920.0f - 2.0f * ui::kGutter;
        for (std::size_t i = 0; i < state.sources.size(); ++i)
        {
            const Source &source = state.sources[i];
            const float y = layout::kListTop + static_cast<float>(i) * kPitch - scroll_.value;
            if (y + kRow < layout::kListTop - 20.0f || y > layout::kListBottom + 20.0f)
                continue;
            const ui::Rect r{ui::kGutter, y, width, kRow};
            const float f = i < row_focus_.size() && focused ? row_focus_[i] : 0.0f;
            ui::focus_ring(list, r, 18.0f, f);
            ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.075f + 0.065f * f));
            const float title_w = std::min(type.medium.measure(source.name, 26.0f), 760.0f);
            ui::text_fit(list, type.medium, source.name, r.x + 30.0f, r.y + 40.0f, 26.0f, title_w + 1.0f,
                         source.enabled ? ui::color::text : hui::gfx::mix(ui::color::text3, ui::color::text2, f));
            float bx = r.x + 30.0f + title_w + 16.0f;
            // On the lit (light grey) row the chips darken and the muted inks lift.
            const ui::Color chip_fill = hui::gfx::mix(ui::Color::rgb(0xffffff, 0.14f), ui::Color::rgb(0x000000, 0.42f), f);
            const ui::Color chip_ink = hui::gfx::mix(ui::color::text2, ui::color::text, f);
            if (!source.enabled)
                bx += ui::chip(list, type, bx, r.y + 32.0f, tr("Off"), chip_fill, chip_ink, 15.0f) + 8.0f;
            if (source.managed)
                bx += ui::chip(list, type, bx, r.y + 32.0f, tr("Suggested"), chip_fill, chip_ink, 15.0f) + 8.0f;
            if (source.is_new)
                ui::chip(list, type, bx, r.y + 32.0f, tr("New"), ui::color::action,
                         ui::color::night1, 14.0f);
            std::string detail = source.kind;
            if (!source.status.empty())
                detail += "  \xC2\xB7  " + source.status;
            if (source.last_refresh_at > 0)
                detail += "  \xC2\xB7  " + tr("refreshed {age}",
                                              {{"age", format_age(static_cast<double>(source.last_refresh_at),
                                                                  unix_seconds())}});
            ui::text_fit(list, type.regular, detail, r.x + 30.0f, r.y + 71.0f, 20.0f, r.w - 420.0f,
                         hui::gfx::mix(ui::color::text3, ui::color::text, f));
            ui::text(list, type.regular,
                     trn(source.package_count, "{count} package", "{count} packages"),
                     r.x + r.w - 30.0f, r.y + 54.0f, 22.0f, hui::gfx::mix(ui::color::text2, ui::color::text, f), ui::Align::right);
        }
        list.pop_clip();
    }
    if (ctx_.store.source_action().busy)
        draw_busy(ctx_, frame.overlay, with_stop_hint(tr("Asking Pegasus DL\xE2\x80\xA6")).c_str());
    prompt_.draw(ctx_, frame);
    confirm_.draw(ctx_, frame);
    list_.draw(ctx_, frame);
}

std::vector<hui::ui::Hint> SourcesScreen::hints() const
{
    return {{hui::ui::Button::cross, tr("Manage")},
            {hui::ui::Button::square, tr("Add address")},
            {hui::ui::Button::triangle, tr("Suggested")},
            {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
