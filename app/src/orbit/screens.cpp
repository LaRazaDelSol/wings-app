// Wings TV app - The placeholder screens behind each tab.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 1: every tab renders from the Store's snapshot, so the shell can be
// exercised end to end against a fake backend. Layout and drawing helpers are
// Orbit Store's (GPL-3.0-or-later); the screens themselves are new.

#include "orbit/screens.hpp"

#include "orbit/diagnostics.hpp"
#include "orbit/editorial.hpp"
#include "orbit/i18n.hpp"
#include "orbit/model.hpp"
#include "orbit/pegasus_manager.hpp"
#include "orbit/pegasus_version.hpp"
#include "orbit/title_export.hpp"
#include "orbit/wings_update.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace orbit
{

namespace
{

constexpr float kListTop = layout::kListTop;
constexpr float kListBottom = layout::kListBottom;
constexpr float kRowHeight = 92.0f;
constexpr float kRowGap = 12.0f;
constexpr float kRowPitch = kRowHeight + kRowGap;

} // namespace

// ---- ListScreen ----

void ListScreen::focus()
{
    on_focus();
}

Intent ListScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    on_frame(focused);
    const bool usable = !needs_pegasus() || ctx_.store.state().online();
    const std::vector<Row> items = usable ? rows() : std::vector<Row>{};
    const int count = static_cast<int>(items.size());
    index_ = std::clamp(index_, 0, std::max(0, count - 1));
    if (focused)
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
            on_confirm(index_, feedback, intent);
    }
    focus_accent_ = count > 0 ? items[static_cast<std::size_t>(index_)].accent : -1;
    row_focus_.resize(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i)
    {
        float &value = row_focus_[static_cast<std::size_t>(i)];
        const float target = focused && i == index_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    const float visible = kListBottom - kListTop;
    // Section headers add room above the rows that open a section (nav indices are unchanged).
    const float headers =
        count > 0 ? theme::section_offset(items, static_cast<std::size_t>(index_)) : 0.0f;
    scroll_.target = std::max(0.0f, static_cast<float>(index_ + 1) * kRowPitch + headers - visible);
    scroll_.update(dt, 11.0f);
    return intent;
}

void ListScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    draw_heading(ctx_, list, title(), subtitle());
    if (needs_pegasus() && !ctx_.store.state().online())
    {
        draw_waiting(ctx_, list, 330.0f);
        return;
    }
    const std::vector<Row> items = rows();
    if (items.empty())
    {
        ui::text(list, type.regular, empty_text(), ui::kGutter, kListTop + 40.0f, 26.0f,
                 ui::color::text2);
        return;
    }
    list.push_clip({0.0f, kListTop - 16.0f, 1920.0f, kListBottom - kListTop + 32.0f});
    const float width = 1920.0f - 2.0f * ui::kGutter;
    float headers = 0.0f; // room taken by the section headers above row i
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const Row &row = items[i];
        const bool opens = theme::section_starts(items, i);
        if (opens)
            headers += theme::kHeaderHeight;
        const float y = kListTop + static_cast<float>(i) * kRowPitch + headers - scroll_.value;
        if (opens && y - theme::kHeaderHeight + 40.0f > kListTop - 20.0f && y < kListBottom + 20.0f)
        {
            // A small non-focusable header: an accent dot and the section's name.
            const float hy = y - theme::kHeaderHeight;
            list.circle(ui::kGutter + 14.0f, hy + 24.0f, 6.0f, theme::accent_color(row.accent));
            ui::text_fit(list, type.semibold, row.section, ui::kGutter + 34.0f, hy + 31.0f, 20.0f,
                         width - 60.0f, ui::color::text2);
        }
        if (y + kRowHeight < kListTop - 20.0f || y > kListBottom + 20.0f)
            continue;
        const ui::Rect r{ui::kGutter, y, width, kRowHeight};
        const float f = i < row_focus_.size() && focused ? row_focus_[i] : 0.0f;
        ui::focus_ring(list, r, 18.0f, f);
        if (row.info)
        {
            // Read-only: an outline instead of the filled control surface.
            ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.02f + 0.10f * f));
            list.bordered_rect(r, 18.0f, ui::Color{0, 0, 0, 0}, 1.5f, ui::Color::rgb(0xffffff, 0.14f * (1.0f - f)));
        }
        else
        {
            ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.075f + 0.065f * f));
        }
        // The lit row turns light grey: lift the muted inks so nothing fades into it.
        const ui::Color dim = hui::gfx::mix(ui::color::text3, ui::color::text, f);
        const ui::Color soft = hui::gfx::mix(ui::color::text2, ui::color::text, f);

        float x = r.x + 30.0f;
        if (!row.art_key.empty())
        {
            draw_cover(ctx_, list, row.art_key, row.title, {r.x + 14.0f, r.y + 14.0f, 64.0f, 64.0f},
                       10.0f);
            x = r.x + 100.0f;
        }
        const float right_room = row.progress >= 0.0f ? 420.0f : 330.0f;
        const float title_width = std::min(
            type.medium.measure(row.title, 26.0f), r.x + r.w - right_room - x - 20.0f);
        ui::text_fit(list, type.medium, row.title, x, r.y + 40.0f, 26.0f, title_width + 1.0f,
                     row.disabled ? dim : ui::color::text);
        if (!row.badge.empty())
            ui::chip(list, type, x + title_width + 16.0f, r.y + 32.0f, row.badge,
                     hui::gfx::mix(ui::Color::rgb(0xffffff, 0.16f), ui::Color::rgb(0x000000, 0.42f), f), soft,
                     16.0f);
        if (!row.detail.empty())
            ui::text_fit(list, type.regular, row.detail, x, r.y + 71.0f, 20.0f,
                         r.x + r.w - right_room - x - 20.0f, row.info ? soft : dim);
        if (!row.segments.empty())
        {
            // A Left/Right switch: every choice shown, the current one a solid pill, arrows outside.
            float sx = r.x + r.w - 30.0f;
            for (std::size_t s = row.segments.size(); s-- > 0;)
            {
                const std::string &label = row.segments[s];
                const float w = type.semibold.measure(label, 20.0f) + 36.0f;
                sx -= w;
                const ui::Rect pill{sx, r.y + 14.0f, w, 40.0f};
                const bool on = static_cast<int>(s) == row.segment;
                if (on)
                    list.rounded_rect(pill, 20.0f, ui::color::text);
                ui::text(list, type.semibold, label, pill.cx(), pill.cy() + 7.0f, 20.0f,
                         on ? ui::color::night1 : soft, ui::Align::center);
                sx -= 8.0f;
            }
            const float ay = r.y + 34.0f;
            list.line(sx - 14.0f, ay, sx - 6.0f, ay - 8.0f, 2.5f, soft);
            list.line(sx - 14.0f, ay, sx - 6.0f, ay + 8.0f, 2.5f, soft);
            const float ex = r.x + r.w - 12.0f;
            list.line(ex, ay, ex - 8.0f, ay - 8.0f, 2.5f, soft);
            list.line(ex, ay, ex - 8.0f, ay + 8.0f, 2.5f, soft);
        }
        else if (!row.right.empty())
            ui::text_fit(list, type.regular, row.right, r.x + r.w - 30.0f, r.y + 40.0f, 22.0f,
                         right_room - 30.0f, row.disabled ? dim : soft, ui::Align::right);
        if (row.progress >= 0.0f)
            ui::progress_bar(list, {r.x + r.w - 30.0f - 360.0f, r.y + 58.0f, 360.0f, 8.0f},
                             row.progress, ui::color::action);
    }
    list.pop_clip();
}

std::vector<hui::ui::Hint> ListScreen::hints() const
{
    std::vector<hui::ui::Hint> items;
    if (const char *label = confirm_label())
        items.push_back({hui::ui::Button::cross, label});
    items.push_back({hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1});
    return items;
}

// ---- Settings ----

namespace
{

std::string debrid_detail(const DebridView &d, bool known)
{
    if (!known)
        return tr("Waiting for Pegasus DL");
    if (!d.configured)
        return tr("No token set. Press Select to add one. The token is stored by Pegasus DL.");
    if (!d.valid)
        return d.error.empty() ? std::string(tr("Pegasus DL did not accept the token. Press Select to replace it or check it again."))
                               : d.error;
    std::string text = d.account_type.empty() ? std::string(tr("Account ready")) : d.account_type;
    if (d.premium)
        text += "  \xC2\xB7  " + std::string(tr("premium"));
    if (d.hosters > 0 || d.domains > 0)
        text += "  \xC2\xB7  " + tr("{hosters} hosters, {domains} domains",
                                    {{"hosters", std::to_string(d.hosters)},
                                     {"domains", std::to_string(d.domains)}});
    if (d.last_checked_at > 0)
        text += "  \xC2\xB7  " + tr("checked {age}",
                                    {{"age", format_age(static_cast<double>(d.last_checked_at),
                                                        unix_seconds())}});
    return text;
}

std::string debrid_right(const DebridView &d)
{
    if (!d.configured)
        return tr("Not set");
    return d.valid ? std::string(tr("Ready")) : std::string(tr("Not valid"));
}

} // namespace

void SettingsScreen::on_frame(bool)
{
    if (ctx_.cover_art != nullptr)
        art_ = ctx_.cover_art->status();
    // The release list is read at most every 6 hours, and only while this group is on screen.
    if (group_ == Group::pegasus)
        pegasus_manager().auto_check();
}

std::vector<ListScreen::Row> SettingsScreen::rows() const
{
    return build(nullptr);
}

SettingsScreen::SettingsScreen(Context &context) : ListScreen(context)
{
    if (!ctx_.wings_prefs_paths.empty())
    {
        load_local_prefs(ctx_.wings_prefs_paths, &prefs_);
        popularity_hub().apply_prefs(prefs_.popularity_enabled, prefs_.popularity_url);
        editorial_hub().apply_prefs(prefs_.notes_enabled, prefs_.carousel_enabled);
        wings_update().set_enabled(prefs_.update_check);
    }
}

void SettingsScreen::store_local_prefs()
{
    if (!ctx_.wings_prefs_paths.empty())
        save_local_prefs(ctx_.wings_prefs_paths, prefs_);
    popularity_hub().apply_prefs(prefs_.popularity_enabled, prefs_.popularity_url);
    editorial_hub().apply_prefs(prefs_.notes_enabled, prefs_.carousel_enabled);
    wings_update().set_enabled(prefs_.update_check);
}

std::vector<ListScreen::Row> SettingsScreen::build(std::vector<Act> *acts) const
{
    std::vector<Row> out;
    if (acts != nullptr)
        acts->clear();
    // The first row is the group header: Left or Right (or Select) switches group.
    {
        Row row;
        const bool wings = group_ == Group::wings;
        row.title = tr("Settings group");
        row.detail = wings ? tr("Kept on this console by Wings only. Nothing here changes Pegasus DL. "
                                "Left or Right shows the Pegasus DL group.")
                           : tr("These change Pegasus DL itself, on this console. "
                                "Left or Right shows the Wings group.");
        row.right = wings ? std::string(tr("Wings")) : std::string(tr("Pegasus DL"));
        row.segments = {tr("Wings"), tr("Pegasus DL")};
        row.segment = wings ? 0 : 1;
        row.accent = wings ? theme::accent_group_wings : theme::accent_group_pegasus;
        out.push_back(std::move(row));
        if (acts != nullptr)
            acts->push_back(Act::group);
    }
    if (group_ == Group::wings)
        build_wings(out, acts);
    else
        build_pegasus(out, acts);
    return out;
}

void SettingsScreen::build_wings(std::vector<Row> &out, std::vector<Act> *acts) const
{
    const Snapshot &state = ctx_.store.state();
    std::string section; // the section the next rows fall under (a header opens it)
    int accent = theme::accent_none;
    const auto begin = [&](const char *name, int accent_id)
    {
        section = tr(name);
        accent = accent_id;
    };
    const auto add = [&](const char *title, std::string detail, std::string right,
                         Act act = Act::none, bool disabled = false)
    {
        Row row;
        row.section = section;
        row.accent = accent;
        row.title = tr(title);
        row.detail = std::move(detail);
        row.right = std::move(right);
        row.disabled = disabled;
        row.info = act == Act::none && !disabled;
        out.push_back(std::move(row));
        if (acts != nullptr)
            acts->push_back(act);
    };
    // Cover art: store posters are https links the app fetches itself.
    begin("Cover art", theme::accent_cover_art);
    if (ctx_.cover_art == nullptr)
    {
        add("Cover art", tr("Store covers are not part of this build"), std::string("-"),
            Act::none);
    }
    else
    {
        add("Download store covers (DNS-over-HTTPS)", tr(kDohPrivacySentence),
            art_.doh ? std::string(tr("On")) : std::string(tr("Off")), Act::doh);
        // The service's own sentence is the state line.
        add("Cover art status", art_.detail.empty() ? std::string(tr("Not started")) : art_.detail,
            art_.working ? std::string(tr("Working"))
                         : (art_.doh ? std::string(tr("Not working")) : std::string(tr("Off"))),
            Act::none);
        add("Clear art cache", tr("Delete the covers downloaded to this console"), std::string(),
            Act::clear_cache);
        add("Retry failed covers", tr("Ask again for covers that could not be downloaded"),
            std::string(), Act::retry_failed);
    }
    // Art sources: where cover icons and the 16:9 game-page art come from (art_sources.hpp).
    begin("Art sources", theme::accent_cover_art);
    add("Cover icons from",
        tr("Where the covers on Store tiles come from. Prosperopatches uses each title's PS5 icon (needs Popularity "
           "data on); a title it has no art for keeps its usual cover."),
        prefs_.icon_source == IconSource::prospero ? std::string(tr("Prosperopatches")) : std::string(tr("Catalogue")),
        Act::icon_source);
    add("16:9 game art from",
        tr("The wide picture behind a game's page. All sources: Prosperopatches first, then IGDB and Steam. "
           "Prosperopatches only: the PS5's own background and nothing else (needs Popularity data on)."),
        prefs_.hero_source == HeroSource::prospero ? std::string(tr("Prosperopatches only"))
                                                   : std::string(tr("All sources")),
        Act::hero_source);
    // Store grid defaults.
    begin("Store grid", theme::accent_store_grid);
    add("Reset Store grid", tr("Back to A to Z with no filters. Your favourites stay."),
        std::string(), Act::reset_grid);
    add("Tile details", tr("When Store tiles show their link tier and regions"),
        std::string(tr(tile_details_label(prefs_.tile_details))), Act::tile_details);
    // Link ranking: which link Wings lists first. Kept by Wings; Pegasus is not asked.
    begin("Link ranking", theme::accent_link_ranking);
    const auto any_or = [](const std::string &value)
    { return value.empty() ? std::string(tr("Any")) : value; };
    add("Preferred firmware", tr("Links that fit this console firmware are listed first"),
        any_or(prefs_.firmware), Act::pref_firmware);
    add("Preferred region", tr("Links for this region are listed first"), any_or(prefs_.region),
        Act::pref_region);
    add("Preferred format", tr("Links in this format are listed first"), any_or(prefs_.format),
        Act::pref_format);
    add("Prefer debrid links", tr("List links a ready debrid account takes ahead of the others"),
        prefs_.prefer_debrid ? std::string(tr("On")) : std::string(tr("Off")), Act::pref_debrid);
    // Popularity data: on by default; a small file is fetched over https (popularity.hpp).
    begin("Popularity", theme::accent_popularity);
    add("Popularity data",
        tr("On by default. Wings downloads one small file of scores and ratings over https "
           "from the address below (it needs Cover art / DNS-over-HTTPS on). Turn it off to stop."),
        prefs_.popularity_enabled ? std::string(tr("On")) : std::string(tr("Off")),
        Act::popularity);
    add("Popularity source URL", tr("Where the popularity numbers come from (https only)"),
        prefs_.popularity_url.empty() ? std::string(tr("Default")) : prefs_.popularity_url,
        Act::popularity_url);
    {
        const PopularityStatus pop = popularity_hub().status();
        std::string line = pop.message.empty() ? std::string(tr("Not fetched yet")) : pop.message;
        if (pop.count > 0)
            line += "  ·  " + std::string(tr("Last updated")) + " " +
                    (pop.updated > 0 ? format_utc(pop.updated).substr(0, 10) : std::string("-")) + "  ·  " +
                    std::to_string(pop.count) + " " + tr("titles");
        add("Refresh popularity data", line, pop.busy ? std::string(tr("Working...")) : std::string(tr("Refresh")),
            Act::popularity_refresh, !prefs_.popularity_enabled || pop.busy);
        add("Popularity credits", attribution_text(popularity_hub().index().get()), std::string("-"), Act::none, true);
        add("Export title list",
            tr("Writes the catalogue's titles to /data/wings/titles.json, for building a popularity index on a PC"),
            state.packages.empty() ? std::string("-") : std::string(tr("Export")), Act::export_titles,
            state.packages.empty());
    }
    // ShadowMount only registers a new game when it scans (shadowmount_scan.hpp).
    begin("Downloads", theme::accent_downloads);
    add("After a download finishes",
        tr("Ask ShadowMount to scan for the new game. It waits while Wings is open. Wings never closes "
           "itself."),
        prefs_.after_download == AfterDownload::scan ? std::string(tr("Ask ShadowMount to scan"))
                                                     : std::string(tr("Do nothing")),
        Act::after_download);
    // Updates: a read-only look at the project's latest release (wings_update.hpp). Wings never installs itself.
    begin("Updates", theme::accent_about);
    add("Check for Wings updates",
        tr("When Wings starts it asks GitHub if a newer release exists (needs Cover art on). It only tells you: "
           "it never downloads or installs anything."),
        prefs_.update_check ? std::string(tr("On")) : std::string(tr("Off")), Act::update_check);
    {
        const WingsUpdateStatus up = wings_update().status();
        const WingsUpdateText words = wings_update_text(up);
        add("Wings update", words.detail, words.value, Act::wings_update, !up.attached);
    }
    // Editorial: short notes about games and the store carousel (editorial.hpp), both on by default.
    begin("Store extras", theme::accent_store_grid);
    add("Known-problem notes", tr("Show a short warning on a game's page when the project site lists one"),
        prefs_.notes_enabled ? std::string(tr("On")) : std::string(tr("Off")), Act::notes);
    add("Featured carousel", tr("Show the project's featured games above the Store grid"),
        prefs_.carousel_enabled ? std::string(tr("On")) : std::string(tr("Off")), Act::carousel);
    // Diagnostics and About: what Wings knows about itself and about Pegasus DL (read-only).
    begin("About", theme::accent_about);
    const bool known = state.online() && state.have_info;
    {
        const std::string &mine = ctx_.app_label.empty() ? ctx_.app_version : ctx_.app_label;
        add("Wings", tr("This app"), mine.empty() ? std::string("-") : mine, Act::none);
    }
    add("Pegasus DL", state.endpoint,
        known ? state.info.version : std::string(tr("Not connected")), Act::none);
    add("Compatibility", tr("How well this version of Wings knows that Pegasus DL"),
        known && !state.info.compat.empty() ? state.info.compat : std::string("-"), Act::none);
    {
        // Link only: the version Pegasus reported, against the range Wings was tested with.
        const PegasusVersionFit fit = known ? pegasus_version_fit(state.info.version) : PegasusVersionFit::unknown;
        const std::string note = pegasus_version_note(fit);
        add("Pegasus DL version check",
            note.empty() ? tr("Tested with Pegasus DL {range}", {{"range", pegasus_tested_range_text()}})
                         : note + "  ·  " +
                               tr("Tested with Pegasus DL {range}", {{"range", pegasus_tested_range_text()}}),
            fit == PegasusVersionFit::tested     ? std::string(tr("Tested"))
            : fit == PegasusVersionFit::untested ? std::string(tr("Untested"))
                                                 : std::string("-"),
            Act::none);
    }
    {
        int on = 0;
        for (const auto &feature : state.info.features)
            on += feature.second ? 1 : 0;
        add("Capabilities", tr("What this Pegasus DL offers; Select shows the list"),
            known && !state.info.features.empty()
                ? tr("{on} of {total} available",
                     {{"on", std::to_string(on)},
                      {"total", std::to_string(state.info.features.size())}})
                : std::string("-"),
            Act::capabilities);
    }
    add("Backend adapter", tr("Which code talks to Pegasus"),
        state.have_info && !state.info.adapter.empty() ? state.info.adapter : std::string("-"),
        Act::none);
    if (known && !state.info.notes.empty())
    {
        add("Features turned off", state.info.notes.front(),
            trn(static_cast<long>(state.info.notes.size()), "{count} note", "{count} notes"),
            Act::none);
    }
}

void SettingsScreen::build_pegasus(std::vector<Row> &out, std::vector<Act> *acts) const
{
    const Snapshot &state = ctx_.store.state();
    const std::string read_only = tr("Pegasus DL runtime (read-only)");
    std::string section;
    int accent = theme::accent_none;
    const auto begin = [&](const char *name, int accent_id)
    {
        section = tr(name);
        accent = accent_id;
    };
    const auto add = [&](const char *title, std::string detail, std::string right,
                         Act act = Act::none, std::string tag = {})
    {
        Row row;
        row.section = section;
        row.accent = accent;
        row.title = tr(title);
        row.detail = std::move(detail);
        row.right = std::move(right);
        row.badge = std::move(tag);
        row.info = act == Act::none;
        out.push_back(std::move(row));
        if (acts != nullptr)
            acts->push_back(act);
    };
    if (!state.have_settings)
    {
        begin("Downloads", theme::accent_downloads);
        add("Download folder and connections", tr("Shown once Pegasus DL answers"), "-");
        begin("Accounts", theme::accent_accounts);
        add("Real-Debrid and TorBox accounts", tr("Shown once Pegasus DL answers"), "-");
        begin("DNS and providers", theme::accent_network);
        add("DNS helper and providers", tr("Shown once Pegasus DL answers"), "-");
        build_pegasus_manager(out, acts);
        return;
    }
    const SettingsView &s = state.settings;
    begin("Downloads", theme::accent_downloads);
    {
        // Why no default folder could be set is a dim sentence after the path (empty: nothing to say).
        const std::string &why = ctx_.store.default_folder_status();
        std::string detail = s.download_root.empty() ? std::string("-") : s.download_root;
        if (!why.empty())
            detail = s.download_root.empty() ? why : detail + "  Â·  " + why;
        add("Download folder", detail, tr("Change"), Act::folder);
    }
    add("Download connections",
        tr("Parallel connections Pegasus DL opens for each download (1 to 64)"),
        std::to_string(s.connections), Act::connections);
    begin("Accounts", theme::accent_accounts);
    add("Real-Debrid", debrid_detail(s.real_debrid, true), debrid_right(s.real_debrid),
        Act::real_debrid);
    add("TorBox", debrid_detail(s.torbox, true), debrid_right(s.torbox), Act::torbox);
    begin("DNS and providers", theme::accent_network);
    std::string dns = s.dns_helper_running ? std::string(tr("Running")) : std::string(tr("Stopped"));
    std::string dns_detail;
    if (!s.dns_present)
        dns_detail = tr("Pegasus DL did not report a DNS helper");
    else
    {
        dns_detail = s.dns_available ? tr("Available") : tr("Not available");
        if (s.dns_managed)
            dns_detail += "  \xC2\xB7  " + std::string(tr("managed by Pegasus DL"));
        if (!s.dns_primary.empty())
            dns_detail += "  \xC2\xB7  " + tr("recommended DNS {address}", {{"address", s.dns_primary}});
        if (!s.dns_error.empty())
            dns_detail += "  \xC2\xB7  " + s.dns_error;
    }
    add("DNS helper", dns_detail + "  \xC2\xB7  " + tr("Select checks it again"), dns,
        Act::dns_refresh);
    {
        std::string names;
        for (const std::string &name : s.providers)
            names += (names.empty() ? "" : ", ") + name;
        add("Provider resolvers",
            names.empty() ? std::string(tr("Pegasus DL did not list any"))
                          : names + "  \xC2\xB7  " +
                                tr("fixed by Pegasus DL; Wings cannot change them"),
            trn(static_cast<long>(s.providers.size()), "{count} provider", "{count} providers"),
            Act::providers, read_only);
    }
    build_pegasus_manager(out, acts);
    // The runtime: shown for information. Pegasus DL offers no way to change these.
    begin("Runtime (read-only)", theme::accent_runtime);
    const std::string fixed = tr("Set by Pegasus DL; Wings cannot change this");
    const auto yes_no = [](bool value)
    { return value ? std::string(tr("Yes")) : std::string(tr("No")); };
    const bool known = state.online() && state.have_info;
    add("Pegasus version", fixed, known ? state.info.version : std::string("-"), Act::none);
    add("Download only", fixed, yes_no(s.download_only), Act::none);
    add("Native .pkg handoff", fixed,
        known ? yes_no(state.info.native_pkg_handoff) : std::string("-"), Act::none);
    add("Range resume", fixed, yes_no(s.range_resume), Act::none);
    add("Queue survives restarts", fixed, yes_no(s.queue_persists), Act::none);
}

void SettingsScreen::send(const SettingsPatch &patch, const std::string &done_text)
{
    // The patch (it may carry a token) lives only in this closure and in the
    // request to Pegasus; nothing here logs or stores it.
    ctx_.store.call<SettingsView>(
        Store::lane_admin, [patch](Backend &b) { return b.settings_update(patch); },
        [this, done_text, patch](Result<SettingsView> &&r)
        {
            if (!r.ok())
            {
                pending_ = Pending::notice;
                confirm_.notice(tr("Pegasus DL did not accept that"), r.error);
                return;
            }
            ctx_.store.apply_settings(r.value);
            if (patch.has_real_debrid || patch.has_torbox)
            {
                const bool torbox = patch.has_torbox;
                const std::string &token = torbox ? patch.torbox_token : patch.real_debrid_token;
                const DebridView &view = torbox ? r.value.torbox : r.value.real_debrid;
                const std::string name = torbox ? "TorBox" : "Real-Debrid";
                if (token.empty())
                {
                    toast_ = tr("{service} token removed", {{"service", name}});
                }
                else if (view.valid)
                {
                    pending_ = Pending::notice;
                    confirm_.notice(tr("{service} is ready", {{"service", name}}),
                                    debrid_detail(view, true));
                }
                else
                {
                    pending_ = Pending::notice;
                    confirm_.notice(tr("{service} token saved, but it is not valid", {{"service", name}}),
                                    debrid_detail(view, true));
                }
                return;
            }
            toast_ = done_text;
        });
}

void SettingsScreen::open_token_menu(bool torbox)
{
    const Snapshot &state = ctx_.store.state();
    const DebridView &view = torbox ? state.settings.torbox : state.settings.real_debrid;
    token_torbox_ = torbox;
    std::vector<ListItem> items;
    menu_codes_.clear();
    const auto add = [&](const char *code, const char *label, std::string detail, bool enabled)
    {
        ListItem item;
        item.label = tr(label);
        item.detail = std::move(detail);
        item.enabled = enabled;
        items.push_back(std::move(item));
        menu_codes_.push_back(code);
    };
    add("set", "Set or replace the token",
        tr("Type it with the on-screen keyboard; it is shown as dots"), true);
    add("refresh", "Check the token again", tr("Ask the service whether it is still valid"),
        view.configured);
    add("clear", "Remove the token", tr("Pegasus DL forgets the account"), view.configured);
    list_for_ = ListFor::token_menu;
    list_.open(torbox ? "TorBox" : "Real-Debrid", debrid_detail(view, true), std::move(items),
               ListDialog::Mode::single);
}

void SettingsScreen::on_confirm(int row, Feedback &feedback, Intent &intent)
{
    std::vector<Act> acts;
    build(&acts);
    if (row < 0 || row >= static_cast<int>(acts.size()))
        return;
    const Snapshot &state = ctx_.store.state();
    switch (acts[static_cast<std::size_t>(row)])
    {
    case Act::doh:
        if (ctx_.cover_art == nullptr)
            break;
        if (art_.doh)
        {
            ctx_.cover_art->set_doh(false);
            intent.toast = tr("Cover downloads turned off");
        }
        else
        {
            // An opt-in with a privacy cost: say it, then ask.
            pending_ = Pending::doh_on;
            confirm_.open(tr("Download store covers?"),
                          std::string(tr(kDohPrivacySentence)) + " " +
                              tr("Nothing else is sent. You can turn this off again here; covers "
                                 "already downloaded keep showing."),
                          tr("Turn on"), tr("Cancel"), false);
        }
        break;
    case Act::clear_cache:
        if (ctx_.cover_art != nullptr)
        {
            ctx_.cover_art->clear_cache();
            intent.toast = tr("Art cache cleared");
        }
        break;
    case Act::retry_failed:
        if (ctx_.cover_art != nullptr)
        {
            ctx_.cover_art->retry_failed();
            intent.toast = tr("Retrying failed covers");
        }
        break;
    case Act::capabilities:
    {
        std::vector<ListItem> items;
        for (const auto &feature : state.info.features)
        {
            ListItem item;
            item.label = feature.first;
            item.right = feature.second ? std::string(tr("Available")) : std::string(tr("Off"));
            items.push_back(std::move(item));
        }
        for (const std::string &note : state.info.notes)
        {
            ListItem item;
            item.label = tr("Note");
            item.detail = note;
            items.push_back(std::move(item));
        }
        if (items.empty())
            break;
        list_for_ = ListFor::capabilities;
        list_.open(tr("What Pegasus DL offers"),
                   state.info.version + "  \xC2\xB7  " + state.info.compat + "  \xC2\xB7  " +
                       state.info.adapter,
                   std::move(items), ListDialog::Mode::single);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    }
    case Act::folder:
        if (state.have_settings)
        {
            picker_.open(ctx_.store, tr("Download folder"), state.settings.download_root);
            feedback.play(hui::audio::Cue::modal_open);
        }
        break;
    case Act::connections:
    {
        std::vector<ListItem> items;
        for (const int value : {2, 4, 8, 16, 24, 32, 48, 64})
        {
            ListItem item;
            item.label = tr("{count} connections", {{"count", std::to_string(value)}});
            item.right = value == state.settings.connections ? std::string(tr("Current"))
                                                             : std::string();
            items.push_back(std::move(item));
        }
        ListItem custom;
        custom.label = tr("Type a number\xE2\x80\xA6");
        custom.detail = tr("Any whole number from 1 to 64");
        items.push_back(std::move(custom));
        list_for_ = ListFor::connections;
        list_.open(tr("Download connections"),
                   tr("More connections can be faster; some hosts refuse too many."),
                   std::move(items), ListDialog::Mode::single);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    }
    case Act::real_debrid:
    case Act::torbox:
        if (state.have_settings)
        {
            open_token_menu(acts[static_cast<std::size_t>(row)] == Act::torbox);
            feedback.play(hui::audio::Cue::modal_open);
        }
        break;
    case Act::group:
        set_group(group_ == Group::wings ? Group::pegasus : Group::wings);
        feedback.play(hui::audio::Cue::focus);
        break;
    case Act::reset_grid:
        pending_ = Pending::reset_grid;
        confirm_.open(tr("Reset the Store grid?"),
                      tr("The Store goes back to A to Z with no filters. Your favourites stay. "
                         "This changes only Wings."),
                      tr("Reset"), tr("Cancel"), false);
        break;
    case Act::pref_firmware:
        open_choice(ListFor::firmware, "Preferred firmware", kFirmwareChoices,
                    kFirmwareChoiceCount, prefs_.firmware);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    case Act::pref_region:
        open_choice(ListFor::region, "Preferred region", kRegionChoices, kRegionChoiceCount,
                    prefs_.region);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    case Act::pref_format:
        open_choice(ListFor::format, "Preferred format", kFormatChoices, kFormatChoiceCount,
                    prefs_.format);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    case Act::pref_debrid:
        prefs_.prefer_debrid = !prefs_.prefer_debrid;
        store_local_prefs();
        intent.toast = prefs_.prefer_debrid ? tr("Debrid links are listed first")
                                            : tr("Debrid links are no longer listed first");
        break;
    case Act::popularity:
        prefs_.popularity_enabled = !prefs_.popularity_enabled;
        store_local_prefs();
        intent.toast = prefs_.popularity_enabled ? tr("Popularity data turned on")
                                                 : tr("Popularity data turned off");
        break;
    case Act::tile_details:
        prefs_.tile_details = next_tile_details(prefs_.tile_details);
        store_local_prefs();
        break;
    case Act::icon_source:
        prefs_.icon_source = prefs_.icon_source == IconSource::catalogue ? IconSource::prospero : IconSource::catalogue;
        store_local_prefs();
        intent.toast = prefs_.icon_source == IconSource::prospero ? tr("Covers now come from Prosperopatches")
                                                                  : tr("Covers now come from the catalogue");
        break;
    case Act::hero_source:
        prefs_.hero_source = prefs_.hero_source == HeroSource::all ? HeroSource::prospero : HeroSource::all;
        store_local_prefs();
        intent.toast = prefs_.hero_source == HeroSource::prospero ? tr("Game pages use Prosperopatches art only")
                                                                  : tr("Game pages use every art source");
        break;
    case Act::update_check:
        prefs_.update_check = !prefs_.update_check;
        store_local_prefs();
        intent.toast = prefs_.update_check ? tr("Wings will check for updates when it starts")
                                           : tr("Wings will not check for updates");
        break;
    case Act::notes:
        prefs_.notes_enabled = !prefs_.notes_enabled;
        store_local_prefs();
        intent.toast = prefs_.notes_enabled ? tr("Game pages show known-problem notes")
                                            : tr("Game pages will not show notes");
        break;
    case Act::carousel:
        prefs_.carousel_enabled = !prefs_.carousel_enabled;
        store_local_prefs();
        intent.toast = prefs_.carousel_enabled ? tr("The Store shows the featured carousel")
                                               : tr("The Store will not show the carousel");
        break;
    case Act::wings_update:
    {
        const WingsUpdateStatus up = wings_update().status();
        if (!up.attached || up.busy)
            break;
        if (up.advice == WingsAdvice::newer)
        {
            // How to install it by hand: a scrolling list (a confirm dialog would cut the steps off).
            const WingsUpdateGuide guide = wings_update_guide(up);
            std::vector<ListItem> items;
            for (const WingsUpdateStep &step : guide.steps)
            {
                ListItem item;
                item.label = step.label;
                item.detail = step.detail;
                items.push_back(std::move(item));
            }
            list_for_ = ListFor::wings_update;
            list_.open(guide.title, guide.subtitle, std::move(items), ListDialog::Mode::single);
            feedback.play(hui::audio::Cue::modal_open);
        }
        else
        {
            // The row says why when the network is not ready.
            intent.toast = wings_update().check_now() ? tr("Checking for a newer Wings...")
                                                      : tr("Could not check for updates");
        }
        break;
    }
    case Act::pegasus_startup:
        // Ask -> Auto-load -> I load it myself -> Ask.
        prefs_.pegasus_start = prefs_.pegasus_start == PegasusStartMode::ask        ? PegasusStartMode::autoload
                               : prefs_.pegasus_start == PegasusStartMode::autoload ? PegasusStartMode::manual
                                                                                    : PegasusStartMode::ask;
        store_local_prefs();
        break;
    case Act::after_download:
        prefs_.after_download = prefs_.after_download == AfterDownload::scan ? AfterDownload::nothing
                                                                             : AfterDownload::scan;
        store_local_prefs();
        break;
    case Act::pegasus_check:
        if (pegasus_manager().check_now())
            intent.toast = tr("Checking the official releases...");
        break;
    case Act::pegasus_cancel:
        pegasus_manager().cancel();
        break;
    case Act::pegasus_download:
    {
        const PegasusStatus pm = pegasus_manager().status();
        if (!pm.have_latest || !pm.latest.downloadable)
            break;
        char size[32];
        std::snprintf(size, sizeof size, "%.1f MB", static_cast<double>(pm.latest.size) / (1024.0 * 1024.0));
        pending_ = Pending::pegasus_download;
        confirm_.open(tr("Download Pegasus DL {tag}?", {{"tag", pm.latest.tag}}),
                      tr("Wings will download Pegasus-dl.elf ({size}) from {url}, the author's official "
                         "release page. Wings checks it against the SHA-256 GitHub publishes and saves it "
                         "to {path}. It never starts it for you. Nothing is downloaded until you press Download.",
                         {{"size", size},
                          {"url", pegasus_download_url(pm.latest.tag)},
                          {"path", pegasus_manager().file_path(pm.latest.tag)}}),
                      tr("Download"), tr("Cancel"), false);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    }
    case Act::pegasus_send:
    {
        if (state.online())
            break;
        pending_ = Pending::pegasus_send;
        confirm_.open(tr("Send to the payload loader? (Experimental)"),
                      tr("Wings will send the downloaded file straight to 127.0.0.1 port {port}, where a "
                         "payload loader normally listens. This is experimental and unverified: Wings cannot "
                         "tell whether the loader accepted it. If it fails, send the file the way you "
                         "usually do.",
                         {{"port", std::to_string(kPegasusLoaderPort)}}),
                      tr("Send"), tr("Cancel"), false);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    }
    case Act::popularity_refresh:
        popularity_hub().request_refresh();
        intent.toast = tr("Updating popularity data...");
        break;
    case Act::export_titles:
    {
        const TitleExport list = build_title_export(ctx_.store.state().packages);
        const int written = list.count > 0
                                ? write_title_export(title_export_paths(ctx_.wings_prefs_paths), list.json)
                                : 0;
        if (written == 0)
            intent.toast = tr("Could not write the title list");
        else if (list.capped)
            intent.toast = tr("Exported {count} titles (list capped)", {{"count", std::to_string(list.count)}});
        else
            intent.toast = tr("Exported {count} titles", {{"count", std::to_string(list.count)}});
        break;
    }
    case Act::popularity_url:
    {
        TextPrompt::Options options;
        options.max_length = 300;
        options.placeholder = "https://";
        options.done_label = tr("Save");
        typing_ = Typing::popularity_url;
        prompt_.open(feedback, tr("Popularity source URL"), prefs_.popularity_url, options);
        break;
    }
    case Act::dns_refresh:
        // One read of Pegasus DL's settings, the same call its own Refresh button makes.
        ctx_.store.call<SettingsView>(
            Store::lane_admin, [](Backend &b) { return b.settings(); },
            [this](Result<SettingsView> &&r)
            {
                if (!r.ok())
                {
                    pending_ = Pending::notice;
                    confirm_.notice(tr("Pegasus DL did not answer"), r.error);
                    return;
                }
                ctx_.store.apply_settings(r.value);
                toast_ = r.value.dns_helper_running ? tr("DNS helper is running")
                                                    : tr("DNS helper is not running");
            });
        break;
    case Act::providers:
    {
        std::vector<ListItem> items;
        for (const std::string &name : state.settings.providers)
        {
            ListItem item;
            item.label = name;
            items.push_back(std::move(item));
        }
        if (items.empty())
            break;
        list_for_ = ListFor::providers;
        list_.open(tr("Provider resolvers"),
                   tr("Links from these hosts are resolved by Pegasus DL itself. Wings cannot change this list."),
                   std::move(items), ListDialog::Mode::single);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    }
    case Act::none:
        break;
    }
}

void SettingsScreen::open_choice(ListFor which, const char *title, const char *const *choices,
                                 int count, const std::string &current)
{
    std::vector<ListItem> items;
    ListItem any;
    any.label = tr("Any");
    any.right = current.empty() ? std::string(tr("Current")) : std::string();
    items.push_back(std::move(any));
    for (int i = 0; i < count; ++i)
    {
        ListItem item;
        item.label = choices[i];
        item.right = current == choices[i] ? std::string(tr("Current")) : std::string();
        items.push_back(std::move(item));
    }
    list_for_ = which;
    list_.open(tr(title), tr("Kept by Wings on this console. Pegasus DL is not asked."),
               std::move(items), ListDialog::Mode::single);
}

Intent SettingsScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    static const InputFrame idle{};
    Intent intent;
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();
    const bool had_prompt = prompt_.is_open();
    const bool had_picker = picker_.is_open();
    const bool busy = ctx_.store.lane_state(Store::lane_admin).busy;

    // Typed values: a connection count, a token.
    const TextPrompt::Result typed = prompt_.update(had_prompt ? input : idle, dt, feedback);
    if (typed == TextPrompt::Result::accepted)
    {
        if (typing_ == Typing::connections)
        {
            const std::string text = prompt_.text();
            char *end = nullptr;
            const long value = std::strtol(text.c_str(), &end, 10);
            if (text.empty() || end == nullptr || *end != '\0' || value < 1 || value > 64)
            {
                pending_ = Pending::notice;
                confirm_.notice(tr("Not a valid number"), tr("Enter a whole number from 1 to 64."));
            }
            else
            {
                SettingsPatch patch;
                patch.has_connections = true;
                patch.connections = static_cast<int>(value);
                send(patch, tr("Download connections saved"));
            }
        }
        else if (typing_ == Typing::token)
        {
            SettingsPatch patch;
            if (token_torbox_)
            {
                patch.has_torbox = true;
                patch.torbox_token = prompt_.text();
            }
            else
            {
                patch.has_real_debrid = true;
                patch.real_debrid_token = prompt_.text();
            }
            send(patch, std::string());
        }
    }
    if (typed == TextPrompt::Result::accepted && typing_ == Typing::popularity_url)
    {
        std::string text = prompt_.text();
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        if (!text.empty() && text.compare(0, 8, "https://") != 0)
        {
            pending_ = Pending::notice;
            confirm_.notice(tr("Not a web address"), tr("Enter an address that starts with https://"));
        }
        else
        {
            prefs_.popularity_url = text;
            store_local_prefs();
            toast_ = tr("Popularity source saved");
        }
    }
    if (typed != TextPrompt::Result::none)
        typing_ = Typing::none;

    const ConfirmDialog::Result result = confirm_.update(had_confirm ? input : idle, dt, feedback);
    if (result == ConfirmDialog::Result::confirmed)
    {
        if (pending_ == Pending::doh_on && ctx_.cover_art != nullptr)
        {
            ctx_.cover_art->set_doh(true);
            intent.toast = tr("Cover downloads turned on");
        }
        else if (pending_ == Pending::pegasus_download)
        {
            if (pegasus_manager().download_latest())
                intent.toast = tr("Downloading Pegasus DL...");
        }
        else if (pending_ == Pending::pegasus_send)
        {
            if (pegasus_manager().send_to_loader(ctx_.store.state().online()))
                intent.toast = tr("Sending to the payload loader...");
        }
        else if (pending_ == Pending::reset_grid)
        {
            if (ctx_.reset_store_grid)
                ctx_.reset_store_grid();
            intent.toast = tr("Store grid reset");
        }
        else if (pending_ == Pending::clear_token)
        {
            SettingsPatch patch;
            if (token_torbox_)
            {
                patch.has_torbox = true;
            }
            else
            {
                patch.has_real_debrid = true;
            }
            send(patch, std::string());
        }
    }
    if (result != ConfirmDialog::Result::none)
        pending_ = Pending::none;

    const ListDialog::Result listed = list_.update(had_list ? input : idle, dt, feedback);
    if (listed == ListDialog::Result::chosen)
    {
        const int chosen = list_.chosen();
        if (list_for_ == ListFor::connections)
        {
            const int values[] = {2, 4, 8, 16, 24, 32, 48, 64};
            if (chosen >= 0 && chosen < 8)
            {
                SettingsPatch patch;
                patch.has_connections = true;
                patch.connections = values[chosen];
                send(patch, tr("Download connections saved"));
            }
            else
            {
                TextPrompt::Options options;
                options.max_length = 3;
                options.placeholder = "16";
                options.done_label = tr("Save");
                typing_ = Typing::connections;
                prompt_.open(feedback, tr("Download connections (1 to 64)"),
                             std::to_string(ctx_.store.state().settings.connections), options);
            }
        }
        else if (list_for_ == ListFor::firmware || list_for_ == ListFor::region ||
                 list_for_ == ListFor::format)
        {
            const char *const *choices = list_for_ == ListFor::firmware ? kFirmwareChoices
                                         : list_for_ == ListFor::region ? kRegionChoices
                                                                        : kFormatChoices;
            const int count = list_for_ == ListFor::firmware ? kFirmwareChoiceCount
                              : list_for_ == ListFor::region ? kRegionChoiceCount
                                                             : kFormatChoiceCount;
            std::string &target = list_for_ == ListFor::firmware ? prefs_.firmware
                                  : list_for_ == ListFor::region ? prefs_.region
                                                                 : prefs_.format;
            if (chosen >= 0 && chosen <= count)
            {
                target = chosen == 0 ? std::string() : std::string(choices[chosen - 1]);
                store_local_prefs();
                toast_ = tr("Link preference saved");
            }
        }
        else if (list_for_ == ListFor::token_menu && chosen >= 0 &&
                 chosen < static_cast<int>(menu_codes_.size()))
        {
            const std::string &code = menu_codes_[static_cast<std::size_t>(chosen)];
            const std::string name = token_torbox_ ? "TorBox" : "Real-Debrid";
            if (code == "set")
            {
                TextPrompt::Options options;
                options.max_length = 300;
                options.password = true;
                options.placeholder = tr("Token");
                options.done_label = tr("Save token");
                typing_ = Typing::token;
                prompt_.open(feedback, tr("{service} token", {{"service", name}}), "", options);
            }
            else if (code == "refresh")
            {
                const std::string service = token_torbox_ ? "torbox" : "real-debrid";
                ctx_.store.call<SettingsView>(
                    Store::lane_admin, [service](Backend &b) { return b.debrid_refresh(service); },
                    [this, name](Result<SettingsView> &&r)
                    {
                        if (!r.ok())
                        {
                            pending_ = Pending::notice;
                            confirm_.notice(tr("Could not check the token"), r.error);
                            return;
                        }
                        ctx_.store.apply_settings(r.value);
                        const DebridView &view = token_torbox_ ? r.value.torbox : r.value.real_debrid;
                        pending_ = Pending::notice;
                        confirm_.notice(view.valid ? tr("{service} is ready", {{"service", name}})
                                                   : tr("{service} is not valid", {{"service", name}}),
                                        debrid_detail(view, true));
                    });
            }
            else if (code == "clear")
            {
                pending_ = Pending::clear_token;
                confirm_.open(tr("Remove the {service} token?", {{"service", name}}),
                              tr("Pegasus DL forgets the account. Links that need {service} go back "
                                 "to the other download routes. You can add the token again.",
                                 {{"service", name}}),
                              tr("Remove"), tr("Keep it"), true);
            }
        }
    }

    const FolderPicker::Result picked =
        picker_.update(ctx_.store, had_picker ? input : idle, dt, feedback);
    if (picked == FolderPicker::Result::chosen)
    {
        SettingsPatch patch;
        patch.has_root = true;
        patch.root = picker_.chosen();
        send(patch, tr("Download folder saved"));
    }

    const bool panel = had_confirm || had_list || had_prompt || had_picker || busy;
    bool switched = false;
    if (focused && !panel && index_ == 0 &&
        (input.nav == Direction::left || input.nav == Direction::right))
    {
        set_group(group_ == Group::wings ? Group::pegasus : Group::wings);
        feedback.play(hui::audio::Cue::focus);
        switched = true;
    }
    const Intent inner = ListScreen::update(panel || switched ? idle : input, dt, feedback, focused);
    if (intent.toast.empty() && !toast_.empty())
    {
        intent.toast = toast_;
    }
    toast_.clear();
    if (intent.toast.empty())
        intent = inner;
    return intent;
}

void SettingsScreen::draw(Frame &frame, bool focused) const
{
    ListScreen::draw(frame, focused && !modal());
    Context &ctx = ctx_;
    if (ctx_.store.lane_state(Store::lane_admin).busy)
        draw_busy(ctx_, frame.overlay, tr("Asking Pegasus DL\xE2\x80\xA6"));
    prompt_.draw(ctx, frame);
    list_.draw(ctx, frame);
    picker_.draw(ctx, frame);
    confirm_.draw(ctx, frame);
}

} // namespace orbit
