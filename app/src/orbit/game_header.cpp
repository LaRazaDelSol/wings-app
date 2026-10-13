// Wings TV app - The game page's header: see game_header.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_header.hpp"

#include "orbit/game_filters.hpp"
#include "orbit/game_hero.hpp"
#include "orbit/game_icons.hpp"
#include "orbit/i18n.hpp"
#include "orbit/screens.hpp"
#include "orbit/text_marquee.hpp"
#include "orbit/theme_game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{

constexpr float kPageW = 1920.0f;
constexpr float kFadeSeconds = 0.4f;

// "Long value..." cut on a UTF-8 boundary.
std::string capped(std::string text)
{
    if (text.size() <= kHeaderValueMax)
        return text;
    std::size_t cut = kHeaderValueMax;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    text.resize(cut);
    return text + "\xE2\x80\xA6";
}

std::string joined(const std::vector<std::string> &items, std::size_t most)
{
    std::string out;
    for (std::size_t i = 0; i < items.size() && i < most; ++i)
        out += (out.empty() ? "" : ", ") + items[i];
    if (items.size() > most)
        out += " +" + std::to_string(items.size() - most);
    return out;
}

ui::Color fit_ink(FwFit fit)
{
    switch (fit)
    {
    case FwFit::runs:
        return theme::semantic::good;
    case FwFit::unknown:
        return theme::semantic::caution;
    case FwFit::too_new:
        break;
    }
    return theme::semantic::bad;
}

float fade_in(const Artwork &art)
{
    return std::clamp(static_cast<float>((now_seconds() - art.ready_at) / kFadeSeconds), 0.0f, 1.0f);
}

} // namespace

// ---- hero URLs ----

bool valid_hero_url(std::string_view url)
{
    return url.size() <= kHeroUrlMax && valid_popularity_url(url);
}

namespace
{
const char *const kLowTail = "#low";
}

bool HeroUrls::set(const std::string &art_key, const std::string &url)
{
    if (art_key.empty() || art_key.size() > 64 || !valid_hero_url(url))
        return false;
    Lock lock(mutex_);
    if (urls_.size() >= 8 && urls_.find(art_key) == urls_.end())
        urls_.clear(); // only the open page is ever asked for
    urls_[art_key] = url;
    return true;
}

bool HeroUrls::set_low(const std::string &art_key, const std::string &url)
{
    if (art_key.empty() || art_key.size() > 64 || !valid_hero_url(url))
        return false;
    const std::string key = art_key + kLowTail;
    Lock lock(mutex_);
    if (urls_.size() >= 8 && urls_.find(key) == urls_.end())
        urls_.clear();
    urls_[key] = url;
    return true;
}

std::string HeroUrls::find_low(std::string_view art_key) const
{
    return find(std::string(art_key) + kLowTail);
}

std::string HeroUrls::find(std::string_view art_key) const
{
    Lock lock(mutex_);
    const auto it = urls_.find(art_key);
    return it == urls_.end() ? std::string() : it->second;
}

void HeroUrls::clear()
{
    Lock lock(mutex_);
    urls_.clear();
}

std::size_t HeroUrls::size() const
{
    Lock lock(mutex_);
    return urls_.size();
}

HeroUrls &hero_urls()
{
    static HeroUrls urls;
    return urls;
}

HeroPlacement place_hero(int width, int height, const ui::Rect &band)
{
    HeroPlacement p;
    if (width <= 0 || height <= 0 || band.w <= 0.0f || band.h <= 0.0f)
        return p;
    const float a = static_cast<float>(width) / static_cast<float>(height);
    if (a < kHeroAspectMin || a > kHeroAspectMax)
        return p;
    p.usable = true;
    p.dest = band;
    // Fill the band, cropping what overflows (a bit more from the bottom than the top, where the faces are).
    const float box = band.w / band.h;
    if (a > box)
    {
        p.uv.w = box / a;
        p.uv.x = (1.0f - p.uv.w) * 0.5f;
    }
    else
    {
        p.uv.h = a / box;
        p.uv.y = (1.0f - p.uv.h) * 0.3f;
    }
    return p;
}

// ---- cards ----

std::vector<HeaderCard> build_header_cards(const HeaderFacts &f)
{
    std::vector<HeaderCard> cards;
    const auto add = [&](const char *label, std::string value, CardTone tone = CardTone::plain,
                         LinkTier tier = LinkTier::unknown, FwFit fit = FwFit::runs)
    {
        if (value.empty())
            return;
        HeaderCard card;
        card.label = label;
        card.value = capped(std::move(value));
        card.tone = tone;
        card.tier = tier;
        card.fit = fit;
        cards.push_back(std::move(card));
    };
    add(tr("Region"), !f.region.empty() ? f.region : joined(f.regions, 3));
    std::string firmware = f.firmware;
    if (firmware.empty())
        firmware = f.firmware_note;
    add(tr("Firmware"), firmware, f.fit_known ? CardTone::fit : CardTone::plain, LinkTier::unknown, f.fit);
    add(tr("Format"), f.format);
    add(tr("Size"), f.size_text);
    add(tr("Version"), f.version);
    std::string source = f.source;
    if (source.empty() && !f.sources.empty())
        source = f.sources.front() + (f.sources.size() > 1 ? " +" + std::to_string(f.sources.size() - 1) : "");
    add(tr("Source"), source);
    if (f.have_download)
        add(tr("Links"), tier_label(f.tier), CardTone::tier, f.tier);
    return cards;
}

HeaderFacts header_facts(const GameView &view, const LinkPrefs &prefs, const StartedLinks &started)
{
    HeaderFacts f;
    f.regions = view.regions;
    f.link_count = view.loaded ? view.page.unique_links : 0;
    f.sources = view.sources;
    if (view.listed_size > 0.0)
    {
        f.size_text = format_bytes(view.listed_size);
        f.size_source = view.listed_size_source;
    }
    if (!view.loaded)
        return f;
    int mirror = view.page.best;
    if (mirror < 0 && !view.shown.mirrors.empty())
        mirror = view.shown.mirrors.front();
    if (mirror < 0 || mirror >= static_cast<int>(view.page.mirrors.size()))
        return f;
    const MirrorCard c = mirror_card(view, mirror, prefs, started);
    f.region = c.region;
    f.firmware = c.fw;
    f.firmware_note = c.fw_note;
    f.fit = c.fit;
    f.fit_known = c.fit_known;
    f.format = c.format;
    f.version = c.version;
    f.source = c.source;
    f.parts = c.parts;
    f.tier = c.tier;
    f.have_download = true;
    return f;
}

HeaderRating header_rating(const PopRecord *record)
{
    HeaderRating r;
    if (record == nullptr)
        return r;
    r.shown = true;
    r.score = std::clamp(record->score, 0, 100);
    r.rating = record->rating;
    if (record->rating >= 0)
    {
        r.rating_text = std::to_string(record->rating);
        r.count = record->rating_count;
        if (record->rating_count > 0)
            r.rating_text += " (" + short_count(record->rating_count) + ")";
    }
    return r;
}

// ---- buttons ----

int header_step(int button, hui::Direction nav)
{
    if (nav == hui::Direction::left)
        return std::max(0, button - 1);
    if (nav == hui::Direction::right)
        return std::min(kHeaderButtons - 1, button + 1);
    return button;
}

std::vector<HeaderMore> header_more_items(bool has_downloads, bool filter_active)
{
    std::vector<HeaderMore> items;
    if (has_downloads)
    {
        items.push_back(HeaderMore::filters);
        if (filter_active)
            items.push_back(HeaderMore::clear_filters);
        items.push_back(HeaderMore::cycle_order);
    }
    return items;
}

// ---- layout ----

HeaderLayout header_layout(float width, int cards, bool rating, bool two_line_title)
{
    using namespace ui::game;
    HeaderLayout l;
    l.text_x = ui::kGutter + kIconSize + kTextGap;
    l.text_w = width - ui::kGutter - l.text_x;
    l.title_lines = two_line_title ? 2 : 1;
    l.title_size = two_line_title ? 46.0f : 64.0f;
    l.title_baseline[0] = two_line_title ? 190.0f : 196.0f;
    l.title_baseline[1] = 244.0f;
    l.meta_baseline = two_line_title ? 292.0f : 248.0f;
    l.cards_top = 322.0f;
    l.card_w = (l.text_w - kCardGap * static_cast<float>(kCardsPerRow - 1)) / static_cast<float>(kCardsPerRow);
    l.card_rows = cards <= 0 ? 0 : (cards + kCardsPerRow - 1) / kCardsPerRow;
    float y = l.cards_top;
    if (l.card_rows > 0)
        y += static_cast<float>(l.card_rows) * kCardHeight + static_cast<float>(l.card_rows - 1) * kCardGap + 26.0f;
    if (rating)
    {
        l.rating_top = y;
        y += 54.0f;
    }
    else
    {
        y += 8.0f;
    }
    l.buttons_top = y;
    l.list_top = std::max(y + kButtonHeight + kListGap, kIconTop + kIconSize + kListGap + 20.0f);
    return l;
}

// ---- state ----

void GameHeaderState::reset()
{
    on_header = true;
    button = 0;
    slide.snap(0.0f);
    for (float &f : focus)
        f = 0.0f;
    hero_url.clear();
    hero_low_url.clear();
    hero_key.clear();
    hero_resolved = false;
    hero_cached = false;
    open_s = 0.0f;
    hero_logged = false;
    more.clear();
}

bool hold_cover_wash(bool has_hero_address, bool cached, float since_open_s, bool hero_refused)
{
    return has_hero_address && cached && !hero_refused && since_open_s < kHeroWashHoldS;
}

HeroHooks &hero_hooks()
{
    static HeroHooks hooks;
    return hooks;
}

bool hero_is_cached(const std::string &url)
{
    const HeroHooks &h = hero_hooks();
    return !url.empty() && h.cached && h.cached(url);
}

void GameHeaderState::update(float dt, bool focused)
{
    if (open_s < 3600.0f)
        open_s += dt;
    slide.target = on_header ? 0.0f : 1.0f;
    slide.update(dt, 10.0f);
    for (int i = 0; i < kHeaderButtons; ++i)
    {
        const float target = focused && on_header && i == button ? 1.0f : 0.0f;
        focus[i] += (target - focus[i]) * (1.0f - std::exp(-16.0f * dt));
    }
}

// ---- drawing ----

namespace
{

// The hero geometry for what the page has to show (game_hero.hpp).
hero::Layout hero_layout_for(Context &ctx, const HeaderView &view)
{
    const std::string &title = view.page != nullptr ? view.page->title : std::string();
    const bool ready = view.page != nullptr && !view.loading && !view.failed;
    const std::vector<hero::Fact> facts = ready ? hero::fact_rows(view.facts) : std::vector<hero::Fact>();
    const float title_w = hero::title_width(kPageW, hero::rows_above_divider(facts), hero::has_links_row(facts));
    const bool two = ctx.type.light.measure(title, 64.0f) > title_w;
    const bool offer = ready && view.page->page.best >= 0 && !best_summary(*view.page).empty();
    const bool why = offer && !view.page->page.best_why.empty();
    return hero::layout(kPageW, two, ready && !hero::chips(view.facts).empty(),
                        ready && (view.rating.shown || !view.rating_live.empty()), offer, why,
                        hero::rows_above_divider(facts), hero::has_links_row(facts));
}

ui::Color tone_ink(hero::Tone tone, LinkTier tier, FwFit fit)
{
    if (tone == hero::Tone::tier)
        return tier_color(tier);
    if (tone == hero::Tone::fit)
        return fit_ink(fit);
    return ui::color::text;
}

void draw_fact_icon(hui::gfx::DrawList &list, hero::Icon icon, float cx, float cy, float size, ui::Color ink)
{
    switch (icon)
    {
    case hero::Icon::source:
        ui::icon_source(list, cx, cy, size, ink);
        break;
    case hero::Icon::size:
        ui::icon_disk(list, cx, cy, size, ink);
        break;
    case hero::Icon::version:
        ui::icon_version(list, cx, cy, size, ink);
        break;
    case hero::Icon::firmware:
        ui::icon_chip(list, cx, cy, size, ink);
        break;
    case hero::Icon::region:
        ui::icon_globe(list, cx, cy, size, ink);
        break;
    case hero::Icon::files:
        ui::icon_files(list, cx, cy, size, ink);
        break;
    case hero::Icon::links:
        ui::icon_link(list, cx, cy, size, ink);
        break;
    }
}

} // namespace

HeaderLayout game_header_layout(Context &ctx, const HeaderView &view)
{
    const hero::Layout h = hero_layout_for(ctx, view);
    HeaderLayout l;
    l.text_x = h.title_x;
    l.text_w = h.title_w;
    l.title_size = h.title_size;
    l.title_baseline[0] = h.title_baseline[0];
    l.title_baseline[1] = h.title_baseline[1];
    l.title_lines = h.title_lines;
    l.meta_baseline = h.meta_baseline;
    l.rating_top = h.rating_baseline;
    l.buttons_top = h.buttons_top;
    l.list_top = h.list_top;
    return l;
}

void draw_game_header(Context &ctx, hui::gfx::DrawList &list, const HeaderView &view,
                      const HeaderLayout &l, const GameHeaderState &state, bool focused)
{
    using namespace ui::game;
    const float s = state.slide.value;
    const float alpha = 1.0f - s;
    if (alpha < 0.01f || view.page == nullptr)
        return;
    const ui::Type &type = ctx.type;
    const hero::Layout h = hero_layout_for(ctx, view);
    const float shift = l.list_top - 290.0f; // the compact page list top (store_screen.cpp kPageListTop)
    list.push_opacity(alpha);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -s * shift);

    // ---- the hero art: the whole first viewport ----
    const ui::Rect band{0.0f, 0.0f, kPageW, hero::kScreenH};
    list.gradient_rect(band, 0.0f, indigo, indigo_deep);
    float hero_alpha = 0.0f;
    bool hero_refused = false;
    if (!view.hero_key.empty())
    {
        // The full picture is asked for first and the small one after it: the worker serves the newest
        // request first, so the small one arrives first. Once the full one is there the small one is
        // not asked for any more. A picture of an unusable shape (place_hero) counts as not there.
        const Artwork *full = view.hero_full ? ctx.art.get(view.hero_key, ArtKind::hero) : nullptr;
        const HeroPlacement full_at =
            full != nullptr ? place_hero(full->width, full->height, band) : HeroPlacement{};
        const float full_alpha = full_at.usable ? fade_in(*full) : 0.0f;
        hero_refused = full != nullptr && !full_at.usable;
        // The small one: until the full one is fully faded in (it stays when the full one is refused).
        if (view.hero_low && full_alpha < 1.0f)
        {
            if (const Artwork *low = ctx.art.get(view.hero_key, ArtKind::hero_low))
            {
                const HeroPlacement at = place_hero(low->width, low->height, band);
                if (at.usable)
                {
                    hero_alpha = fade_in(*low);
                    list.image(low->texture, at.dest, at.uv, ui::Color::rgb(0xffffff, hero_alpha), 0.0f);
                }
            }
        }
        if (full_at.usable)
        {
            hero_alpha = std::max(hero_alpha, full_alpha);
            list.image(full->texture, full_at.dest, full_at.uv, ui::Color::rgb(0xffffff, full_alpha), 0.0f);
        }
    }
    // Once per page open: how long until the first frame with the picture on screen (or why none).
    if (!state.hero_logged)
    {
        const HeroHooks &hooks = hero_hooks();
        char msg[96];
        bool say = false;
        if (hero_alpha > 0.0f)
        {
            std::snprintf(msg, sizeof msg, "[WINGS-ART] hero first frame %d ms after page open%s",
                          static_cast<int>(state.open_s * 1000.0f), state.hero_cached ? " (cached)" : "");
            say = true;
        }
        else if (hero_refused || (state.hero_resolved && view.hero_key.empty()))
        {
            std::snprintf(msg, sizeof msg, "[WINGS-ART] hero: no hero%s", hero_refused ? " (refused shape)" : "");
            say = true;
        }
        else if (state.open_s > 8.0f)
        {
            std::snprintf(msg, sizeof msg, "[WINGS-ART] hero: none drawn after 8000 ms");
            say = true;
        }
        if (say)
        {
            state.hero_logged = true;
            if (hooks.log)
                hooks.log(msg);
        }
    }
    const bool hold_wash = hold_cover_wash(view.hero_full, state.hero_cached, state.open_s, hero_refused);
    if (hero_alpha < 1.0f && !view.page->art_key.empty() && !hold_wash)
    {
        // No hero (or not yet): the cover, scaled up, as a blurred wash.
        if (const Artwork *cover = ctx.art.get(view.page->art_key, ArtKind::cover))
        {
            const float a = (1.0f - hero_alpha) * fade_in(*cover);
            if (cover->ambient != 0)
                ui::image_cover(list, cover->ambient, cover->width, cover->height, band,
                                ui::Color::rgb(0xffffff, 0.95f * a));
            ui::image_cover(list, cover->texture, cover->width, cover->height, band,
                            ui::Color::rgb(0xffffff, 0.30f * a), 0.0f, 0.5f, 0.3f);
        }
    }
    // The Store hue veil (colour layer 4), then the dark left-to-right veil under the
    // words, the top veil under the app bar and the fade into the page where the list starts.
    list.rounded_rect(band, 0.0f, blue_wash);
    const ui::Color left_clear = ui::Color::rgb(0x03061a, 0.0f);
    list.gradient_rect_h({0.0f, 0.0f, 1500.0f, hero::kScreenH}, 0.0f, veil_left, left_clear);
    list.gradient_rect({0.0f, 0.0f, kPageW, 170.0f}, 0.0f, veil_top, left_clear);
    list.gradient_rect({0.0f, 820.0f, kPageW, hero::kScreenH - 820.0f}, 0.0f, ui::Color::rgb(0x080808, 0.0f),
                       page_end);

    // ---- the cover (page identity) and the title beside it ----
    list.shadow(h.icon, ui::kArtRadius, 36.0f, ui::Color::rgb(0x000000, 0.55f));
    draw_cover(ctx, list, view.page->art_key, view.page->title, h.icon, ui::kArtRadius);
    const std::string &title = view.page->title;
    if (h.title_lines == 1)
        ui::text_fit(list, type.light, title, h.title_x, h.title_baseline[0], h.title_size, h.title_w,
                     ui::color::text);
    else
        ui::paragraph(list, type.light, title, h.title_x, h.title_baseline[0], h.title_size, h.title_w,
                      h.title_baseline[1] - h.title_baseline[0], ui::color::text, 2);
    if (view.loading || view.failed)
    {
        ui::text_fit(list, type.regular, view.status, h.title_x, h.meta_baseline, 24.0f, h.title_w,
                     ui::color::text2);
        list.pop_transform();
        list.pop_opacity();
        return;
    }
    ui::text_fit(list, type.regular, view.meta_line, h.title_x, h.meta_baseline, 23.0f, h.title_w,
                 ui::color::text2);

    // ---- the tag chips ----
    const std::vector<hero::Chip> chips = hero::chips(view.facts);
    if (h.chips_cy > 0.0f && !chips.empty())
    {
        constexpr float kChipSize = 19.0f;
        constexpr float kChipGap = 10.0f;
        std::vector<float> widths;
        for (const hero::Chip &c : chips)
            widths.push_back(type.semibold.measure(c.text, kChipSize) + 28.0f);
        const int fit = hero::chips_that_fit(widths, h.stack_w, kChipGap);
        float x = h.stack_x;
        for (int i = 0; i < fit; ++i)
        {
            const hero::Chip &c = chips[static_cast<std::size_t>(i)];
            const ui::Color ink = tone_ink(c.tone, c.tier, c.fit);
            const ui::Color edge = c.tone != hero::Tone::plain ? ui::Color{ink.r, ink.g, ink.b, 0.8f}
                                                               : ui::Color::rgb(0xffffff, 0.55f);
            x += ui::chip(list, type, x, h.chips_cy, c.text, ui::Color::rgb(0x000000, 0.28f), ink, kChipSize,
                          edge) +
                 kChipGap;
        }
    }

    // ---- the rating row: score, stars, count (or the live review line) ----
    if (h.rating_baseline > 0.0f)
    {
        const float base = h.rating_baseline;
        float x = h.stack_x;
        if (view.rating.shown)
        {
            if (view.rating.rating >= 0)
            {
                const float value = static_cast<float>(view.rating.rating) / 20.0f;
                char score[16];
                std::snprintf(score, sizeof score, "%.1f", static_cast<double>(value));
                x += ui::text(list, type.semibold, score, x, base, 26.0f, card_value) + 12.0f;
                x += ui::rating_stars(list, x, base - 9.0f, 11.0f, value, card_value,
                                      ui::Color::rgb(0xffffff, 0.28f)) +
                     12.0f;
                if (view.rating.count > 0)
                    x += ui::text(list, type.regular, short_count(view.rating.count), x, base, 21.0f,
                                  card_label) +
                         36.0f;
            }
            x += ui::text(list, type.semibold, tr("Popularity"), x, base, 19.0f, card_label) + 14.0f;
            const ui::Rect track{x, base - 14.0f, 140.0f, 10.0f};
            list.rounded_rect(track, 5.0f, bar_track);
            list.rounded_rect({track.x, track.y,
                               std::max(10.0f, track.w * static_cast<float>(view.rating.score) / 100.0f),
                               track.h},
                              5.0f, bar_fill);
            x += track.w + 14.0f;
            ui::text(list, type.medium, std::to_string(view.rating.score), x, base, 23.0f, card_value);
        }
        else
        {
            ui::text_fit(list, type.regular, view.rating_live, x, base, 22.0f, h.stack_w, card_label);
        }
    }

    // ---- the offer: the Best pick and the reason ----
    if (h.offer_baseline > 0.0f)
        ui::text_fit(list, type.medium, best_summary(*view.page), h.stack_x, h.offer_baseline, 30.0f,
                     h.stack_w, ui::color::text);
    if (h.why_baseline > 0.0f)
        ui::text_fit(list, type.regular, view.page->page.best_why, h.stack_x, h.why_baseline, 21.0f,
                     h.stack_w, ui::color::text2);

    // ---- the buttons ----
    const float by = h.buttons_top;
    const float bh = hero::kButtonH;
    float x = h.stack_x;
    const std::string install = tr("Install");
    const float install_w = std::max(250.0f, ui::button_width(type, install, 24.0f) + 40.0f);
    ui::button(list, type, {x, by, install_w, bh}, install, ui::ButtonKind::primary, state.focus[0], true, 24.0f);
    x += install_w + 18.0f;

    const std::string fav = view.favourite ? std::string(tr("Favourited")) : std::string(tr("Favourite"));
    const float fav_w = type.medium.measure(fav, 22.0f) + 40.0f + 52.0f;
    {
        const ui::Rect r{x, by, fav_w, bh};
        const float f = state.focus[1];
        ui::focus_ring(list, r, bh * 0.5f, f);
        list.rounded_rect(r, bh * 0.5f, hui::gfx::mix(button_fill, button_fill_focus, f));
        if (view.favourite)
            ui::icon_heart_filled(list, r.x + 40.0f, r.cy(), 28.0f, ui::color::text);
        else
            ui::icon_heart(list, r.x + 40.0f, r.cy(), 28.0f, ui::color::text);
        ui::text(list, type.medium, fav, r.x + 70.0f, r.cy() + 8.0f, 22.0f, ui::color::text);
        x += fav_w + 18.0f;
    }
    {
        const ui::Rect r{x, by, bh, bh};
        const float f = state.focus[2];
        ui::focus_ring(list, r, bh * 0.5f, f);
        list.rounded_rect(r, bh * 0.5f, hui::gfx::mix(button_fill, button_fill_focus, f));
        for (int i = -1; i <= 1; ++i)
            list.circle(r.cx() + static_cast<float>(i) * 14.0f, r.cy(), 4.0f, ui::color::text);
    }

    // ---- the facts panel: quiet glass, icon rows, a divider before the links ----
    const std::vector<hero::Fact> facts = hero::fact_rows(view.facts);
    if (h.panel.h > 0.0f && !facts.empty())
    {
        list.bordered_rect(h.panel, 20.0f, ui::color::glass, 1.5f, card_edge);
        const float inner_x = h.panel.x + hero::kPanelPad;
        const float inner_r = h.panel.x + h.panel.w - hero::kPanelPad;
        const float label_x = inner_x + 48.0f;
        const float value_x = label_x + 112.0f;
        const int above = hero::rows_above_divider(facts);
        for (std::size_t i = 0; i < facts.size(); ++i)
        {
            const hero::Fact &fact = facts[i];
            float cy = h.first_row_cy + static_cast<float>(i) * hero::kRowPitch;
            if (static_cast<int>(i) >= above)
                cy += hero::kDividerGap;
            draw_fact_icon(list, fact.icon, inner_x + 18.0f, cy, 26.0f, ui::color::text2);
            ui::text_fit(list, type.regular, fact.label, label_x, cy + 7.0f, 18.0f, 104.0f, ui::color::text3);
            const float accent_w =
                fact.accent.empty() ? 0.0f : type.semibold.measure(fact.accent, 19.0f) + 12.0f;
            const float value_w = inner_r - value_x - accent_w;
            const float natural = type.medium.measure(fact.value, 22.0f);
            float drawn = 0.0f;
            if (natural > value_w)
            {
                // Too wide: scroll the value inside its own rect instead of an ellipsis.
                const float shift = marquee_offset(ctx.clock, natural, value_w);
                list.push_clip({value_x, cy - 22.0f, value_w, 36.0f});
                ui::text(list, type.medium, fact.value, value_x - shift, cy + 8.0f, 22.0f, ui::color::text);
                list.pop_clip();
                drawn = value_w;
            }
            else
            {
                drawn = ui::text(list, type.medium, fact.value, value_x, cy + 8.0f, 22.0f, ui::color::text);
            }
            if (!fact.accent.empty())
                ui::text(list, type.semibold, fact.accent, value_x + drawn + 12.0f, cy + 8.0f, 19.0f,
                         tone_ink(fact.tone, fact.tier, fact.fit));
        }
        if (h.divider_y > 0.0f)
            list.rounded_rect({inner_x, h.divider_y - 0.75f, inner_r - inner_x, 1.5f}, 0.75f, ui::color::line);
    }

    // ---- the next section label peeks at the bottom edge ----
    const float lw = ui::text(list, type.semibold, tr("Downloads"), h.stack_x, h.label_baseline, 26.0f,
                              ui::color::text);
    ui::icon_chevron(list, h.stack_x + lw + 26.0f, h.label_baseline - 9.0f, 18.0f, ui::color::text2);
    (void)focused;
    list.pop_transform();
    list.pop_opacity();
}

} // namespace orbit
