// Wings TV app - Filters and facets for a game page's mirrors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/research/links-grouping.md section 4.2: show a facet only when it has
// two or more values on this page. Order: runs on my firmware (links that do
// not state one stay, marked "fw ?"), no browser needed, region, single file
// only, format, then source / host / kind in an overflow menu. Pure functions
// over a GamePage; counts are mirrors (cards), not links.

#pragma once

#include "orbit/game_page.hpp"
#include "orbit/ranking.hpp"

#include <string>
#include <vector>

namespace orbit
{

struct FacetValue
{
    std::string value; // what MirrorFilter takes: "USA", "?" (not stated), "exFAT", "Akirabox", ...
    int count = 0;     // mirrors with it
};

struct Facets
{
    std::vector<FacetValue> regions; // known first (USA EUR JPN ASIA HK KOR order), "?" last
    std::vector<FacetValue> formats; // "exFAT" "FPKG" "PKG" "Dump" "FFPFS", "?" last
    std::vector<FacetValue> hosts;   // most mirrors first
    std::vector<FacetValue> sources; // page source order
    std::vector<FacetValue> sections; // "Game" "Updates" "DLC" "Other" (value = section index)
    int fw_runs = 0;    // with LinkPrefs::firmware set: mirrors that run
    int fw_unknown = 0; // ... that do not say
    int fw_too_new = 0; // ... that need newer firmware
    int no_browser = 0; // native .pkg, debrid or automated
    int single_file = 0;
    int total = 0;

    // Two or more values: worth showing.
    bool show_regions() const;
    bool show_formats() const; // two or more KNOWN formats
    bool show_firmware() const;   // a firmware is set and something would be hidden or unknown
    bool show_no_browser() const; // some, not all
    bool show_single_file() const;
};

struct MirrorFilter
{
    bool runs_on_my_fw = false; // hide mirrors that need a newer firmware (unknown stays)
    bool no_browser = false;    // native .pkg, debrid, automated only
    bool single_file = false;
    std::string region; // "" any, "?" not stated, else exact
    std::string format; // "" any, "?" not stated, else format_label()
    std::string host;   // "" any
    std::string source; // "" any
    int section = -1;   // -1 any, else static_cast<int>(Section)
    bool active() const;
};

Facets page_facets(const GamePage &page, const LinkPrefs &prefs);

bool mirror_passes(const GamePage &page, int mirror, const MirrorFilter &filter, const LinkPrefs &prefs);

struct FilterResult
{
    std::vector<int> mirrors;  // shown, in page order (release order, then mirror order)
    int hidden_too_new = 0;    // hidden by "runs on my firmware" (for "3 hidden (need 12.xx)")
    std::string lowest_needed; // the lowest firmware those hidden ones need ("12.xx"), or ""
    int hidden_hosts = 0;      // hidden by LinkPrefs::hidden_hosts
};
// Every mirror of the page in display order (sections, releases, mirrors) that passes.
FilterResult filter_mirrors(const GamePage &page, const MirrorFilter &filter, const LinkPrefs &prefs);

// True when the tier needs no browser by Wings' classification (native .pkg, debrid, automated).
bool no_browser_tier(LinkTier tier);

} // namespace orbit
