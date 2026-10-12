// Wings TV app - tests for the grid's Region / firmware / browser / single-file filters.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crafted games for each filter, the aggregate against the per-game functions,
// persistence (round trip, an old file), Reset, and the combination with
// search, sort and favourites; then the whole recorded catalogue for speed.

#include "orbit/game_view.hpp"
#include "orbit/grid_filters.hpp"
#include "orbit/pegasus_backend.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

namespace orbit
{
namespace
{

PackageLink link(const std::string &name)
{
    PackageLink l;
    l.name = name;
    l.host = "rootz.so";
    return l;
}

PackageDetail detail_of(std::initializer_list<const char *> names)
{
    PackageDetail d;
    int i = 0;
    for (const char *n : names)
    {
        PackageLink l = link(n);
        l.index = i++;
        d.links.push_back(l);
    }
    return d;
}

// Four games: Alpha (USA, 12.xx, one file, a browser capture), Beta (EUR, 4.xx
// backport, 3 parts, a .pkg), Gamma (USA + JPN, no firmware stated, one file and
// parts, debrid), Delta (links not read yet).
struct Crafted
{
    std::vector<Package> packages;
    std::vector<std::uint8_t> tiers;
    std::vector<PackageFacts> facts;
    GameIndex index;
    InstalledIndex installed;
    LinkPrefs prefs;

    Crafted()
    {
        const char *titles[] = {"Alpha", "Beta", "Gamma", "Delta"};
        packages.resize(4);
        for (std::size_t i = 0; i < 4; ++i)
        {
            packages[i].id = std::to_string(i);
            packages[i].title = titles[i];
            packages[i].title_id = "PPSA0000" + std::to_string(i + 1);
            packages[i].source_name = i % 2 == 0 ? "One" : "Two";
            packages[i].link_count = 1;
        }
        tiers = {pack_tier(LinkTier::supported, false), pack_tier(LinkTier::native_pkg, false),
                 pack_tier(LinkTier::unknown, true), pack_tier(LinkTier::unknown, false)};
        facts.resize(4);
        facts[0] = package_facts(detail_of({"USA 12.xx Base - Rootz"}));
        facts[1] = package_facts(detail_of({"EUR 7.xx BackPort 4.xx - Rootz part 1", "EUR 7.xx BackPort 4.xx - Rootz part 2",
                                            "EUR 7.xx BackPort 4.xx - Rootz part 3"}));
        facts[2] = package_facts(detail_of({"JPN Base - Rootz", "USA Base - Rootz part 1", "USA Base - Rootz part 2"}));
        // facts[3] stays unread.
        index = group_games(packages);
        prefs.firmware = "10.xx";
    }
    std::vector<std::string> titles(const CatalogueState &state, std::string_view query = {}, bool aggregate = true)
    {
        const GridAggregate agg = build_grid_aggregate(packages, tiers, index, facts, prefs.firmware);
        GridQuery q{packages, tiers, index, facts, state, query, installed, prefs, aggregate ? &agg : nullptr};
        std::vector<std::string> out;
        for (const GameTile &t : select_games(q))
            out.push_back(index.games[static_cast<std::size_t>(t.game)].title);
        return out;
    }
};

using Titles = std::vector<std::string>;

TEST(GridFilters, SingleFileFactsComeFromTheReleases)
{
    Crafted c;
    EXPECT_TRUE(c.facts[0].single_file);
    EXPECT_FALSE(c.facts[1].single_file);
    EXPECT_TRUE(c.facts[2].single_file); // the JPN release is one file, the USA one has parts
    EXPECT_FALSE(c.facts[3].known);
    // A DLC alone says nothing about the game.
    EXPECT_FALSE(package_facts(detail_of({"USA DLC - Rootz"})).single_file);
}

TEST(GridFilters, EachFilterAgainstCraftedGames)
{
    Crafted c;
    CatalogueState state;
    EXPECT_EQ(c.titles(state), (Titles{"Alpha", "Beta", "Delta", "Gamma"}));
    state.region = "EUR";
    EXPECT_EQ(c.titles(state), (Titles{"Beta"}));
    state.region = "USA";
    EXPECT_EQ(c.titles(state), (Titles{"Alpha", "Gamma"}));
    state.region = "KOR"; // nobody has it
    EXPECT_TRUE(c.titles(state).empty());
    state.region.clear();
    // Runs on 10.xx: Alpha needs 12.xx; Beta's backport runs; Gamma does not say; Delta unread stays.
    state.runs_on_my_fw = true;
    EXPECT_EQ(c.titles(state), (Titles{"Beta", "Delta", "Gamma"}));
    c.prefs.firmware = "13.xx";
    EXPECT_EQ(c.titles(state), (Titles{"Alpha", "Beta", "Delta", "Gamma"}));
    c.prefs.firmware.clear(); // no firmware: nothing is hidden
    EXPECT_EQ(c.titles(state).size(), 4u);
    c.prefs.firmware = "10.xx";
    state.runs_on_my_fw = false;
    // No browser: a .pkg (Beta) or a debrid link (Gamma).
    state.no_browser = true;
    EXPECT_EQ(c.titles(state), (Titles{"Beta", "Gamma"}));
    state.no_browser = false;
    // Single file: Alpha, Gamma; Delta is unread so it stays.
    state.single_file_only = true;
    EXPECT_EQ(c.titles(state), (Titles{"Alpha", "Delta", "Gamma"}));
    // The aggregate and the per-game functions agree.
    for (const bool fw : {false, true})
    {
        state.runs_on_my_fw = fw;
        EXPECT_EQ(c.titles(state, {}, true), c.titles(state, {}, false));
    }
}

TEST(GridFilters, CountsAndRegionsOnlyForWhatExists)
{
    Crafted c;
    const GridAggregate agg = build_grid_aggregate(c.packages, c.tiers, c.index, c.facts, "10.xx");
    EXPECT_EQ(agg.region_names(), (std::vector<std::string>{"USA", "EUR", "JPN"}));
    EXPECT_EQ(agg.region_count("USA"), 2);
    EXPECT_EQ(agg.region_count("EUR"), 1);
    EXPECT_EQ(agg.region_count("KOR"), 0);
    EXPECT_EQ(agg.runs_on, 2); // Beta, Gamma (Delta is unknown, not counted)
    EXPECT_EQ(agg.no_browser, 2);
    EXPECT_EQ(agg.single_file, 2);
    EXPECT_EQ(build_grid_aggregate(c.packages, c.tiers, c.index, c.facts, "").runs_on, 0);
}

TEST(GridFilters, PersistenceRoundTripOldFileAndReset)
{
    CatalogueState s;
    s.region = "EUR";
    s.runs_on_my_fw = true;
    s.no_browser = true;
    s.single_file_only = true;
    CatalogueState back;
    ASSERT_TRUE(decode_catalogue_state(encode_catalogue_state(s), &back));
    EXPECT_EQ(back.region, "EUR");
    EXPECT_TRUE(back.runs_on_my_fw);
    EXPECT_TRUE(back.no_browser);
    EXPECT_TRUE(back.single_file_only);
    EXPECT_TRUE(back.filtered());
    EXPECT_EQ(filter_summary(back, ""), "EUR, Runs on my firmware, No browser needed, Single file only");
    // A file written before these keys existed.
    CatalogueState old;
    ASSERT_TRUE(decode_catalogue_state("sort=1\ninstalled=1\nfav=PPSA00001\n", &old));
    EXPECT_FALSE(old.single_file_only);
    EXPECT_TRUE(old.region.empty());
    EXPECT_EQ(old.installed, InstalledFilter::installed);
    // Reset clears the new filters, keeps favourites and the sort.
    back.favourites = {"PPSA00001"};
    back.sort = SortMode::za;
    back.reset_filters();
    EXPECT_FALSE(back.filtered());
    EXPECT_EQ(back.favourites.size(), 1u);
    EXPECT_EQ(back.sort, SortMode::za);
    EXPECT_EQ(filter_summary(back, ""), "None");
}

TEST(GridFilters, CombineWithSearchSortAndFavourites)
{
    Crafted c;
    CatalogueState state;
    state.region = "USA";
    EXPECT_EQ(c.titles(state, "gam"), (Titles{"Gamma"}));
    EXPECT_TRUE(c.titles(state, "beta").empty());
    state.single_file_only = true;
    state.sort = SortMode::za;
    EXPECT_EQ(c.titles(state), (Titles{"Gamma", "Alpha"}));
    state.runs_on_my_fw = true; // Alpha drops
    EXPECT_EQ(c.titles(state), (Titles{"Gamma"}));
    state.runs_on_my_fw = false;
    state.favourites_only = true;
    EXPECT_TRUE(c.titles(state).empty());
    state.toggle_favourite("PPSA00001"); // Alpha
    EXPECT_EQ(c.titles(state), (Titles{"Alpha"}));
    EXPECT_EQ(filter_summary(state, "al"), "USA, Single file only, Favourites, Search \"al\"");
}

std::string samples_dir()
{
    if (const char *env = std::getenv("WINGS_SAMPLES"))
        return env;
    return std::string(HUI_SOURCE_DIR) + "/../../recon/samples";
}

TEST(GridFilters, RecordedCatalogueAggregateIsFastAndAgreesWithTheGrid)
{
    std::ifstream file(samples_dir() + "/library.json", std::ios::binary);
    if (!file)
        GTEST_SKIP() << "recon/samples/library.json is not reachable from here";
    std::ostringstream text;
    text << file.rdbuf();
    auto replay = std::make_shared<wings::http::ReplayTransport>();
    replay->add("GET", "/api/health", 200,
                R"({"status":"running","port":6970,"downloadOnly":false,"nativePkgHandoff":true,"version":"1.11.0"})");
    replay->add("GET", "/api/library", 200, text.str());
    PegasusBackend backend(replay, "127.0.0.1:6970");
    const Result<std::vector<Package>> all = backend.library();
    ASSERT_TRUE(all.ok());
    std::vector<PackageFacts> facts;
    for (const Package &p : all.value)
    {
        const Result<PackageDetail> d = backend.package(p.id);
        ASSERT_TRUE(d.ok());
        facts.push_back(package_facts(d.value));
    }
    const GameIndex index = group_games(all.value);
    const std::vector<std::uint8_t> tiers = backend.package_tiers().value;
    LinkPrefs prefs;
    prefs.firmware = "10.xx";

    const auto start = std::chrono::steady_clock::now();
    const GridAggregate agg = build_grid_aggregate(all.value, tiers, index, facts, prefs.firmware);
    const double build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(agg.games.size(), index.games.size());
    EXPECT_GE(agg.region_names().size(), 6u);
    EXPECT_EQ(agg.region_names().front(), "USA");
    EXPECT_GT(agg.runs_on, 0);
    EXPECT_GT(agg.no_browser, 0);
    EXPECT_GT(agg.single_file, 0);
    EXPECT_LT(agg.single_file, static_cast<int>(index.games.size()));

    InstalledIndex installed;
    const auto count = [&](const CatalogueState &state, const GridAggregate *a)
    { return select_games(GridQuery{all.value, tiers, index, facts, state, "", installed, prefs, a}).size(); };
    const auto t1 = std::chrono::steady_clock::now();
    for (int round = 0; round < 20; ++round)
    {
        CatalogueState state;
        state.single_file_only = true;
        EXPECT_EQ(count(state, &agg), count(state, nullptr));
        state.runs_on_my_fw = true;
        EXPECT_EQ(count(state, &agg), count(state, nullptr));
        state.region = "JPN";
        state.no_browser = true;
        EXPECT_EQ(count(state, &agg), count(state, nullptr));
    }
    const double select_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    for (const auto &entry : agg.region_counts)
    {
        CatalogueState state;
        state.region = entry.first;
        EXPECT_EQ(count(state, &agg), static_cast<std::size_t>(entry.second)) << entry.first;
    }
    std::printf("[grid filters] aggregate of %zu games (%zu packages) in %.3f s; 120 filtered selects in %.2f s; "
                "%zu regions, runs %d, no browser %d, single file %d\n",
                index.games.size(), all.value.size(), build_seconds, select_seconds, agg.region_counts.size(),
                agg.runs_on, agg.no_browser, agg.single_file);
    EXPECT_LT(build_seconds, 2.0); // sanitizers included
    EXPECT_LT(select_seconds, 20.0);
}

} // namespace
} // namespace orbit
