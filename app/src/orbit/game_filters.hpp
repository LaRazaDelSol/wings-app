// Wings TV app - Game page: default filter seeded from the preferences, and the mirror sort.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure functions over a GamePage (no screen, no I/O). The Filters panel itself
// lives in game_view.* (rows, values, cycling) and store_screen.cpp (drawing).

#pragma once

#include "orbit/game_page.hpp"
#include "orbit/link_facets.hpp"
#include "orbit/ranking.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orbit
{

// ---- the default filter ----

struct FilterSeed
{
    MirrorFilter filter;
    bool dropped_firmware = false; // the preference would have hidden every download: left off
    bool dropped_region = false;
    bool dropped_format = false;
    std::string note; // "Your region (USA) would hide every download, so it is not applied." or ""
    bool dropped() const
    {
        return dropped_firmware || dropped_region || dropped_format;
    }
};

// Firmware ("Runs on my firmware" once a firmware is set and it hides something),
// region and format from the preferences, each only when the page has two or more
// values of it and the preferred one is among them. When the result would show
// nothing, preferences are dropped (format, then region, then firmware) until it
// does, and `note` says so. Never returns a filter that hides every mirror
// unless the page has none.
FilterSeed seed_filter(const GamePage &page, const Facets &facets, const LinkPrefs &prefs);

// ---- the sort of mirror cards within a release ----

enum class MirrorSort : std::uint8_t
{
    best,         // the page's ranking
    newest,       // newest version first, unknown last
    fewest_parts, // single files first, then fewer parts
    host,         // host A-Z
};
constexpr int kMirrorSortCount = 4;

const char *mirror_sort_label(MirrorSort sort);
MirrorSort cycle_mirror_sort(MirrorSort sort, int delta);

// Reorders `mirrors` (FilterResult::mirrors: display order) so that inside each run of
// one release the cards follow `sort`. Sections and releases keep their places; ties keep
// the ranking order. Best is a no-op.
void sort_mirrors(const GamePage &page, std::vector<int> *mirrors, MirrorSort sort);

} // namespace orbit
