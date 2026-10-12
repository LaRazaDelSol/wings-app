// Wings TV app - The grid's per-game filter answers, computed once.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Region, "Runs on my firmware", "No browser needed" and "Single file only"
// each need a pass over a game's packages. build_grid_aggregate() does all of
// them for every game in one pass when the catalogue, its tiers, the link
// facts or the firmware change; select_games() then only reads the result and
// the Utilities panel shows the counts from it.

#pragma once

#include "orbit/game_view.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace orbit
{

struct GameAgg
{
    std::vector<std::string> regions; // game_regions(): known order first
    std::int8_t runs_on = -1;         // game_runs_on()
    bool no_browser = false;          // a link that needs no PS5 browser (a tier that is not capture / unknown, or debrid)
    std::int8_t single_file = -1;     // game_single_file()
};

struct GridAggregate
{
    std::vector<GameAgg> games; // parallel to GameIndex::games
    std::vector<std::pair<std::string, int>> region_counts; // known order first; games per region
    int runs_on = 0;      // games that pass "Runs on my firmware" (0 when no firmware is set)
    int no_browser = 0;
    int single_file = 0;

    std::vector<std::string> region_names() const;
    // Games of a region, 0 when the region does not exist.
    int region_count(const std::string &region) const;
};

GridAggregate build_grid_aggregate(const std::vector<Package> &packages, const std::vector<std::uint8_t> &tiers,
                                   const GameIndex &index, const std::vector<PackageFacts> &facts,
                                   const std::string &firmware);

} // namespace orbit
