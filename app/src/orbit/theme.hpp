// Wings TV app - The colour system: place, section, meaning.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Colour answers one of four questions and nothing else. Place: one hue per tab, a soft wash
// across the top of the screen and a dot on the active tab. Section: that hue shifted by a fixed
// step per section (settings groups, filter and sort menus). Meaning: the fixed semantic colours
// (good / caution / bad / info), only ever on values, dots and hairlines, always with a word.
// Content: the game page's cover tint, in game_header.cpp. Chrome stays neutral glass and the
// focus ring stays white. Screens never pick a hue: they report a section accent (below) and the
// App owns the one Wash. Pure data and small helpers: nothing is drawn here.

#pragma once

#include "orbit/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace orbit::theme
{

// ---- layer 1: place (the tabs; same order as tab:: in screens.hpp) ----
enum Place : int
{
    place_none = -1,
    place_store = 0,
    place_downloads,
    place_library,
    place_files,
    place_sources,
    place_settings,
    place_logs,
    place_count,
};

// Hue in degrees, cool family 190 to 270.
inline float tab_hue(int place)
{
    static const float table[place_count] = {245.0f, 190.0f, 270.0f, 205.0f, 225.0f, 235.0f, 215.0f};
    return place >= 0 && place < place_count ? table[place] : table[place_store];
}
// Saturation per place: Logs is the quiet, low-chroma one.
inline float tab_saturation(int place)
{
    return place == place_logs ? 0.30f : 0.62f;
}
constexpr float kWashValue = 0.70f; // HSV value of every wash colour (never above 0.70)

// HSV to a colour (hue in degrees, wraps).
inline ui::Color hsv(float hue, float sat, float val, float alpha = 1.0f)
{
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f)
        hue += 360.0f;
    const float h = hue / 60.0f;
    const int sector = static_cast<int>(h) % 6;
    const float f = h - std::floor(h);
    const float p = val * (1.0f - sat);
    const float q = val * (1.0f - sat * f);
    const float t = val * (1.0f - sat * (1.0f - f));
    switch (sector)
    {
    case 0:
        return {val, t, p, alpha};
    case 1:
        return {q, val, p, alpha};
    case 2:
        return {p, val, t, alpha};
    case 3:
        return {p, q, val, alpha};
    case 4:
        return {t, p, val, alpha};
    default:
        return {val, p, q, alpha};
    }
}

// A place's wash colour at an alpha; the one way screens may ask for a hue.
inline ui::Color wash_color(float hue, float alpha, int place = place_store)
{
    return hsv(hue, tab_saturation(place), kWashValue, alpha);
}
inline ui::Color tab_color(int place, float alpha = 1.0f)
{
    return wash_color(tab_hue(place), alpha, place);
}
// The small dot on the active tab: the place hue, lighter so it reads at 10 px.
inline ui::Color tab_dot_color(int place)
{
    return hsv(tab_hue(place), tab_saturation(place) * 0.85f, 0.95f);
}

// ---- layer 2: section (a fixed step from the place hue, by position in its list) ----
// Row::accent values. -1 (none) is a row outside every section.
enum Accent : int
{
    accent_none = -1,
    accent_group_wings = 0, // the "Settings group" row while the Wings group shows
    accent_group_pegasus,
    // Settings, Wings group
    accent_store_grid,
    accent_link_ranking,
    accent_cover_art,
    accent_popularity,
    accent_about,
    // Settings, Pegasus DL group
    accent_downloads,
    accent_accounts,
    accent_network,
    accent_runtime,
    // Store filter menus
    accent_filter_show,
    accent_filter_region_fw,
    accent_filter_format_host,
    accent_filter_source,
    accent_filter_sort,
    // Store sort menu
    accent_sort_order,
    accent_sort_popularity,
    accent_sort_library,
    accent_sort_link,
    accent_count,
};

struct SectionSpec
{
    int place;
    float step; // degrees from the place hue; within +/- 24
};

inline SectionSpec section_spec(int accent)
{
    static const SectionSpec table[accent_count] = {
        {place_settings, 0.0f},   // group_wings
        {place_settings, 0.0f},   // group_pegasus
        {place_settings, 12.0f},  // store_grid
        {place_settings, 24.0f},  // link_ranking
        {place_settings, 0.0f},   // cover_art
        {place_settings, -12.0f}, // popularity
        {place_settings, -24.0f}, // about
        {place_settings, 0.0f},   // downloads
        {place_settings, 12.0f},  // accounts
        {place_settings, 24.0f},  // network
        {place_settings, -12.0f}, // runtime
        {place_store, 0.0f},      // filter: show
        {place_store, 12.0f},     // filter: region and firmware
        {place_store, -12.0f},    // filter: format and host
        {place_store, 24.0f},     // filter: source
        {place_store, -24.0f},    // filter: sort
        {place_store, 0.0f},      // sort menu: order
        {place_store, 12.0f},     // sort menu: popularity
        {place_store, -12.0f},    // sort menu: library state
        {place_store, 24.0f},     // sort menu: link quality and source
    };
    return accent >= 0 && accent < accent_count ? table[accent] : SectionSpec{place_settings, 0.0f};
}
inline int accent_place(int accent)
{
    return accent >= 0 && accent < accent_count ? section_spec(accent).place : place_none;
}
// Hue of a section: place hue plus its step. Stable: a pure lookup.
inline float section_hue(int place, float step)
{
    return tab_hue(place) + step;
}
inline float accent_hue(int accent)
{
    const SectionSpec s = section_spec(accent);
    return section_hue(s.place, s.step);
}
// The accent colour (opaque); out-of-range values give the Settings place hue.
inline ui::Color accent_color(int accent)
{
    const SectionSpec s = section_spec(accent);
    return wash_color(section_hue(s.place, s.step), 1.0f, s.place);
}

// ---- layer 3: meaning (fixed; aliases of the tier colours in ui.hpp) ----
namespace semantic
{
inline const ui::Color good = ui::color::tier_green;     // works / fits / ready / installed
inline const ui::Color caution = ui::color::tier_yellow; // needs something extra
inline const ui::Color bad = ui::color::tier_red;        // will not work / failed
inline const ui::Color info = ui::Color::rgb(0x7aa8ff);  // selected, changed, active filter
inline const ui::Color muted = ui::color::text3;         // not applicable, unknown
} // namespace semantic

// ---- transitions (seconds) ----
constexpr float kTabCrossfade = 0.40f; // switch tab: wash hue and tab dot, ease-out cubic
constexpr float kSectionDrift = 0.25f; // focus into another section: wash hue, ease-out
constexpr float kDotPulse = 0.20f;     // header dot, scale 1.0 to 1.25 to 1.0
constexpr float kSemanticFade = 0.12f; // a semantic state change, linear
constexpr float kPanelIn = 0.18f;      // a panel opens: the screen behind dims to kPanelDim
constexpr float kPanelDim = 0.55f;
inline float ease_out_cubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// ---- the wash and accessibility ----
// Alpha 0.20 at the very top, gone by 45 percent of the height, never above 0.22.
constexpr float kWashTop = 0.20f;
constexpr float kWashBottom = 0.0f;
constexpr float kWashReach = 0.45f; // fraction of the screen height
constexpr float kWashMax = 0.22f;
constexpr float kMinTextContrast = 4.5f;

// WCAG relative luminance and contrast ratio (opaque colours).
inline float luminance(ui::Color c)
{
    const auto lin = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
inline float contrast(ui::Color a, ui::Color b)
{
    const float la = luminance(a);
    const float lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}
// fg (its alpha counts) laid over an opaque bg.
inline ui::Color over(ui::Color fg, ui::Color bg)
{
    return {bg.r + (fg.r - bg.r) * fg.a, bg.g + (fg.g - bg.g) * fg.a, bg.b + (fg.b - bg.b) * fg.a, 1.0f};
}
// The lowest contrast of `text` over the base under any place or section wash at its strongest
// allowed alpha (kWashMax): the spec's "worst wash".
inline float worst_wash_contrast(ui::Color text, ui::Color base = ui::color::night1)
{
    float worst = contrast(text, base);
    for (int a = 0; a < accent_count; ++a)
        worst = std::min(worst, contrast(text, over(accent_color(a).with_alpha(kWashMax), base)));
    for (int p = 0; p < place_count; ++p)
        worst = std::min(worst, contrast(text, over(tab_color(p, kWashMax), base)));
    return worst;
}

// The header row of a section: room above its first row.
constexpr float kHeaderHeight = 46.0f;

// True when row i opens a section: its header is drawn above it.
template <class Rows> bool section_starts(const Rows &rows, std::size_t i)
{
    return i < rows.size() && !rows[i].section.empty() &&
           (i == 0 || rows[i - 1].section != rows[i].section);
}

// How far row i sits below where a header-less list would put it: one header height for
// every section that opens at or before it.
template <class Rows> float section_offset(const Rows &rows, std::size_t i)
{
    float offset = 0.0f;
    for (std::size_t k = 0; k <= i && k < rows.size(); ++k)
        offset += section_starts(rows, k) ? kHeaderHeight : 0.0f;
    return offset;
}

// The one colour behind the screen, owned by the App. It is retargeted by place (the tab) first,
// then by section (the focused row's accent); only one hue is ever in flight, and a retarget
// restarts from wherever the colour is now (interrupted, not queued).
struct Wash
{
    int place = place_none;   // the tab being eased to
    int accent = accent_none; // the section being eased to (-1: the place hue itself)
    ui::Color from = ui::Color{0, 0, 0, 0};
    ui::Color to = ui::Color{0, 0, 0, 0};
    float t = 1.0f; // 0 = from, 1 = to
    float duration = kSectionDrift;

    // Place, then section. The first call lands at once. A place change eases over a tab
    // crossfade; a section change within the place over a section drift. An accent that belongs
    // to another place (or -1) is ignored: the place hue stays.
    void retarget(int next_place, int next_accent)
    {
        const bool ok = next_accent != accent_none && accent_place(next_accent) == next_place;
        const ui::Color want = ok ? accent_color(next_accent) : tab_color(next_place);
        const int want_accent = ok ? next_accent : accent_none;
        if (next_place == place && want_accent == accent)
            return;
        const bool moved_place = next_place != place;
        if (place == place_none)
        {
            from = to = want;
            t = 1.0f;
        }
        else
        {
            from = color();
            to = want;
            t = 0.0f;
            duration = moved_place ? kTabCrossfade : kSectionDrift;
        }
        place = next_place;
        accent = want_accent;
    }
    // A section alone (tests, or a surface with no tab): it brings its own place.
    void retarget(int next_accent)
    {
        if (next_accent == accent_none)
            return;
        retarget(accent_place(next_accent), next_accent);
    }
    void update(float dt)
    {
        if (t < 1.0f)
            t = std::min(1.0f, t + dt / std::max(duration, 0.001f));
    }
    bool settled() const
    {
        return t >= 1.0f;
    }
    // The current opaque colour.
    ui::Color color() const
    {
        return hui::gfx::mix(from, to, ease_out_cubic(t));
    }
    // The wash's top and bottom colours (the hue at kWashTop and fully clear).
    ui::Color top() const
    {
        ui::Color c = color();
        c.a = kWashTop;
        return c;
    }
    ui::Color bottom() const
    {
        ui::Color c = color();
        c.a = kWashBottom;
        return c;
    }
};

} // namespace orbit::theme
