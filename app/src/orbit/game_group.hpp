// Wings TV app - One game, many packages: grouping the catalogue into games.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same game appears several times: per source, per region SKU (PS5 title
// ids are per region), per edition. The rule (docs/research/links-grouping.md
// section 3, option c):
//   1. packages with one titleId stay together unless their titles give
//      evidence that they differ (other numbers, a bundle/collection/demo word,
//      no shared word: catalogues carry wrong ids and list bundles under each
//      member's id);
//   2. the clusters are then joined across titleIds when their canonical title
//      (normalised, one trailing edition phrase removed) is equal, ignoring spaces.
// No fuzzy matching. Everything keeps the package indexes, so the existing
// (packageId, linkIndex) queue calls still work.

#pragma once

#include "orbit/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// norm_title(): accents folded, case, brackets, versions, punctuation, roman
// numerals ("Quake II" = "quake 2"), "ps4"/"ps5" dropped. '+' is kept.
std::string norm_title(std::string_view title);
// norm_title() minus ONE trailing edition phrase ("deluxe edition", "goty").
std::string loose_title(std::string_view title);
// Two loose titles under one titleId: true unless there is evidence they differ.
bool same_title_id_compatible(std::string_view a, std::string_view b);

// The title to show: [exFAT]-style brackets, Pippo version suffixes and the
// trademark signs removed.
std::string clean_title(std::string_view title);

// The regions a package description states ("Region: EUR", Pippo
// "Tags: PPSA..., v01.027, USA, ..."): "USA" "EUR" "JPN" "ASIA" "HK" "KOR", first-seen order.
std::vector<std::string> description_regions(std::string_view description);

struct GameGroup
{
    std::string id;    // stable across catalogue reloads: "g:" + canonical title without spaces
    std::string title; // the most frequent clean title (ties: more punctuation, then first)
    std::string canon; // the canonical loose title
    std::vector<int> packages; // indexes into the grouped vector, catalogue order
    std::vector<std::string> title_ids; // distinct, upper case, sorted
    std::string cover;       // the first non-empty poster
    std::string description; // the longest
    std::vector<std::string> sources;  // distinct source names, first-seen order
    std::vector<std::string> versions; // distinct package versions, newest first
    std::vector<std::string> regions;  // from the package descriptions (links add theirs on the game page)
    int link_count = 0;      // over all packages (before URL de-duplication)
    double size_bytes = 0.0; // the largest listed package size (0 unknown); never a sum
    bool has_pkg = false;
    bool has_automated = false;
    bool has_supported = false;
};

struct GameIndex
{
    std::vector<GameGroup> games; // in the order of each game's first package
    std::vector<int> game_of;     // per package: index into games
};

// Groups `packages` (any order; the result follows it). Packages without links
// are grouped like the rest.
GameIndex group_games(const std::vector<Package> &packages);

// The ambiguous decisions, for a person to check (and a later override table).
struct ReviewItem
{
    enum class Kind : std::uint8_t
    {
        split, // one titleId whose packages became several games
        join,  // one game made of several titleIds
    } kind = Kind::split;
    std::vector<std::string> title_ids;
    std::vector<std::string> titles; // distinct package titles
    std::vector<std::string> games;  // the game ids involved
    std::string reason; // "different numbers", "bundle word: trilogy", "no shared word", "same title", "edition"
};
std::vector<ReviewItem> review_list(const std::vector<Package> &packages, const GameIndex &index);

} // namespace orbit
