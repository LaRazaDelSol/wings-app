// Wings TV app - The grid's per-game filter answers, computed once.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/grid_filters.hpp"

#include <map>

namespace orbit
{

std::vector<std::string> GridAggregate::region_names() const
{
    std::vector<std::string> out;
    out.reserve(region_counts.size());
    for (const auto &entry : region_counts)
        out.push_back(entry.first);
    return out;
}

int GridAggregate::region_count(const std::string &region) const
{
    for (const auto &entry : region_counts)
    {
        if (entry.first == region)
            return entry.second;
    }
    return 0;
}

GridAggregate build_grid_aggregate(const std::vector<Package> &packages, const std::vector<std::uint8_t> &tiers,
                                   const GameIndex &index, const std::vector<PackageFacts> &facts,
                                   const std::string &firmware)
{
    GridAggregate out;
    out.games.resize(index.games.size());
    std::map<std::string, int> per_region;
    for (std::size_t gi = 0; gi < index.games.size(); ++gi)
    {
        const GameGroup &g = index.games[gi];
        GameAgg &a = out.games[gi];
        a.regions = game_regions(g, facts);
        a.runs_on = static_cast<std::int8_t>(game_runs_on(g, facts, firmware));
        a.single_file = static_cast<std::int8_t>(game_single_file(g, facts));
        for (const int p : g.packages)
        {
            if (p < 0 || p >= static_cast<int>(packages.size()))
                continue;
            const std::uint8_t packed = tier_of(packages, tiers, static_cast<std::size_t>(p));
            a.no_browser = a.no_browser || no_browser_tier(packed_tier(packed)) || packed_via_debrid(packed);
        }
        for (const std::string &r : a.regions)
            ++per_region[r];
        out.runs_on += a.runs_on == 1 ? 1 : 0;
        out.no_browser += a.no_browser ? 1 : 0;
        out.single_file += a.single_file == 1 ? 1 : 0;
    }
    std::vector<std::string> names;
    for (const auto &entry : per_region)
        names.push_back(entry.first);
    sort_regions(&names);
    for (const std::string &n : names)
        out.region_counts.emplace_back(n, per_region[n]);
    return out;
}

} // namespace orbit
