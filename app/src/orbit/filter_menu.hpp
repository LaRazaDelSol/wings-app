// Wings TV app - Filter menus: named groups, an "N filters on" summary and a result count.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure helpers for the Store's grid-filter panel and the game page's link-filter panel. The
// panels stay lists of focusable rows (so D-pad steps, persistence and filter meaning are as
// before); group headers are layout only, drawn above the first row of a group, never a row of
// their own, so the D-pad cannot land on one. Accents come from theme.hpp.

#pragma once

#include "orbit/catalogue_view.hpp"
#include "orbit/game_view.hpp"
#include "orbit/theme.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace orbit
{

// Geometry (virtual 1920 x 1080 px), tighter than the Utilities list so ten rows and four
// headers fit.
constexpr float kMenuPitch = 62.0f;
constexpr float kMenuRow = 54.0f;
constexpr float kMenuHeader = 32.0f;
constexpr float kMenuSummary = 46.0f; // the "N filters on" strip under the title

enum class FilterGroup : std::uint8_t
{
    show,        // what kind of game / download appears
    region_fw,   // region and firmware
    format_host, // how it is delivered
    source,
    sort,
    none, // the Clear row: no header
};
const char *filter_group_name(FilterGroup group); // "" for none
int filter_group_accent(FilterGroup group);       // theme::Accent
FilterGroup link_filter_group(LinkFilterRow row);

// The Sort by menu's groups, in presentation order (SortMode values are persisted and stay
// as they are; only the order the menu lists them in follows these groups).
enum class SortGroup : std::uint8_t
{
    order,      // A to Z, Z to A
    popularity, // needs popularity data (or the app's own downloads)
    library,    // installed / not installed / favourites
    link,       // best link, source
};
SortGroup sort_group(SortMode mode);
const char *sort_group_name(SortGroup group);
int sort_group_accent(SortGroup group); // theme::Accent
// The available modes, regrouped: Order, Popularity, Library state, Link quality / source.
// Modes that are not available are absent, and so is a group left empty.
std::vector<SortMode> sort_menu_order(const std::vector<SortMode> &available);

// Filters currently narrowing the grid / the game's links (sort and search not counted).
int grid_filters_on(const CatalogueState &state);
int link_filters_on(const MirrorFilter &filter);
// Whether one link row differs from its default (the sort counts here: it is marked, not counted).
bool link_row_changed(LinkFilterRow row, const MirrorFilter &filter, MirrorSort sort);

// "No filters on", "1 filter on", "3 filters on".
std::string filters_on_text(int count);
// "12 of 340 games" / "7 of 9 downloads".
std::string result_count_text(int shown, int total, bool downloads);

// First changed row, else the first row (the first group's first control).
int first_focus_index(const std::vector<bool> &changed);

// Row rows[i]'s top, measured from the first row's top without headers: i pitches plus one
// header height for every group that opens at or before it. Rows need a `section` string.
template <class Rows> float menu_row_top(const Rows &rows, std::size_t i)
{
    float headers = 0.0f;
    for (std::size_t k = 0; k <= i && k < rows.size(); ++k)
        headers += theme::section_starts(rows, k) ? kMenuHeader : 0.0f;
    return static_cast<float>(i) * kMenuPitch + headers;
}
// Total height of the rows and their headers.
template <class Rows> float menu_rows_height(const Rows &rows)
{
    return rows.empty() ? 0.0f : menu_row_top(rows, rows.size() - 1) + kMenuRow;
}

} // namespace orbit
