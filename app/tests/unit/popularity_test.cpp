// Wings TV app - Popularity data: parsing, schema checks, game lookup, sorts, the fetch service
// (off => no call, cache fallback) and the local download counter.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_view.hpp"
#include "orbit/popularity.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace orbit
{
namespace
{

std::string fixture()
{
    std::ifstream in(std::string(HUI_SOURCE_DIR) + "/tests/fixtures/popularity_sample.json", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string tmp_dir()
{
    char path[] = "/tmp/wings-pop-XXXXXX";
    return mkdtemp(path) != nullptr ? std::string(path) : std::string("/tmp");
}

std::string doc(const std::string &titles, const std::string &version = "1")
{
    return "{\"schemaVersion\":" + version + ",\"generatedAt\":\"2026-10-12T00:00:00Z\",\"attribution\":{},\"titles\":{" +
           titles + "}}";
}

std::string rec(const char *id, int score, double confidence, int rating = 80, int count = 100)
{
    return std::string("\"") + id + "\":{\"score\":" + std::to_string(score) + ",\"rating\":" + std::to_string(rating) +
           ",\"ratingCount\":" + std::to_string(count) + ",\"confidence\":" + std::to_string(confidence) +
           ",\"sources\":[\"igdb\"],\"fetchedAt\":\"2026-10-12\"}";
}

struct FakeFetcher final : DocFetcher
{
    int calls = 0;
    DocResult result;
    DocResult get(const std::string &, std::size_t) override
    {
        ++calls;
        return result;
    }
};

TEST(Popularity, ParsesTheFixture)
{
    const std::string text = fixture();
    ASSERT_FALSE(text.empty());
    PopularityIndex index;
    std::string error;
    ASSERT_TRUE(PopularityIndex::parse(text, &index, &error)) << error;
    EXPECT_GE(index.size(), 50u);
    const PopRecord *r = index.find("PPSA01285");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->score, 75);
    EXPECT_EQ(r->rating, 84);
    EXPECT_EQ(r->rating_count, 5164);
    EXPECT_EQ(attribution_text(&index).find("IGDB.com") != std::string::npos, true);
    EXPECT_EQ(popularity_line(r), "Popularity 75  \xC2\xB7  Rating 84 (5.2k)");
}

TEST(Popularity, SchemaRejects)
{
    PopularityIndex index;
    std::string error;
    EXPECT_FALSE(PopularityIndex::parse(doc(rec("PPSA00001", 50, 1.0), "2"), &index, &error)); // wrong version
    EXPECT_FALSE(PopularityIndex::parse("{\"schemaVersion\":1}", &index, &error));              // no titles
    EXPECT_FALSE(PopularityIndex::parse("not json", &index, &error));
    EXPECT_FALSE(PopularityIndex::parse(doc(rec("PPSA00001", 500, 1.0)), &index, &error)); // no usable record
    std::string big = doc(rec("PPSA00001", 50, 1.0));
    big.append(kPopularityMaxBytes, ' ');
    EXPECT_FALSE(PopularityIndex::parse(big, &index, &error)); // oversize
    EXPECT_FALSE(valid_popularity_url("http://larazadelsol.github.io/x.json"));
    EXPECT_FALSE(valid_popularity_url("https://user:pw@host.test/x.json"));
    EXPECT_FALSE(valid_popularity_url("https://host.test/a b"));
    EXPECT_FALSE(valid_popularity_url("ftp://host.test/x"));
    EXPECT_TRUE(valid_popularity_url(kPopularityDefaultUrl));
}

TEST(Popularity, GameLookupPicksHighestConfidenceAndHidesLow)
{
    PopularityIndex index;
    ASSERT_TRUE(PopularityIndex::parse(
        doc(rec("PPSA00001", 40, 0.7) + "," + rec("PPSA00002", 90, 0.95) + "," + rec("PPSA00003", 99, 0.5)), &index,
        nullptr));
    const PopRecord *best = index.best({"PPSA00001", "PPSA00002", "PPSA00003"});
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->score, 90);
    EXPECT_EQ(index.best({"PPSA00003"}), nullptr); // 0.5 < 0.6: hidden
    EXPECT_EQ(index.best({"PPSA99999"}), nullptr);
}

TEST(Popularity, SortsPutMissingDataLastWithStableTies)
{
    std::vector<Package> packages;
    const char *ids[] = {"PPSA00001", "PPSA00002", "PPSA00003", "PPSA00004"};
    const char *titles[] = {"Delta Game", "Alpha Game", "Charlie Game", "Bravo Game"};
    for (int i = 0; i < 4; ++i)
    {
        Package p;
        p.id = std::to_string(i + 1);
        p.title = titles[i];
        p.title_id = ids[i];
        packages.push_back(p);
    }
    const GameIndex index = group_games(packages);
    PopularityIndex pop;
    // Delta 60, Alpha 60 (a tie), Charlie unknown, Bravo low confidence (hidden).
    ASSERT_TRUE(PopularityIndex::parse(
        doc(rec("PPSA00001", 60, 1.0, 70, 10) + "," + rec("PPSA00002", 60, 1.0, 90, 5) + "," + rec("PPSA00004", 99, 0.3)),
        &pop, nullptr));
    LocalDownloads downloads;
    PopularityView view{&pop, &downloads};
    CatalogueState state;
    InstalledIndex installed;
    LinkPrefs prefs;
    const auto order = [&](SortMode mode)
    {
        state.sort = mode;
        std::string out;
        for (const GameTile &t : select_games(GridQuery{packages, {}, index, {}, state, "", installed, prefs, nullptr, &view}))
            out += packages[static_cast<std::size_t>(t.art_package)].title.substr(0, 1);
        return out;
    };
    EXPECT_EQ(order(SortMode::popular), "ADBC");  // tie by title, no data (B, C) last by title
    EXPECT_EQ(order(SortMode::rated), "ADBC");    // Alpha 90, Delta 70
    EXPECT_EQ(order(SortMode::reviewed), "DABC"); // Delta 10, Alpha 5
    // Without data the popularity sorts fall back to A to Z; so do the local ones.
    EXPECT_EQ(order(SortMode::my_downloads), "ABCD");
    state.sort = SortMode::popular;
    const GameTile first = select_games(GridQuery{packages, {}, index, {}, state, "", installed, prefs, nullptr})[0];
    EXPECT_EQ(first.popularity, -1);
    EXPECT_EQ(effective_sort(SortMode::popular, nullptr), SortMode::az);
    // The panel offers the extra orders only when the data is there.
    EXPECT_EQ(available_sorts(nullptr).size(), static_cast<std::size_t>(kBasicSortCount));
    EXPECT_EQ(available_sorts(&view).size(), static_cast<std::size_t>(kBasicSortCount) + 3);
    downloads.add({"PPSA00003"});
    EXPECT_EQ(order(SortMode::my_downloads), "CABD");
    EXPECT_EQ(available_sorts(&view).size(), static_cast<std::size_t>(kBasicSortCount) + 4);
    // The badge score is on the tiles.
    state.sort = SortMode::az;
    EXPECT_EQ(select_games(GridQuery{packages, {}, index, {}, state, "", installed, prefs, nullptr, &view})[0].popularity, 60);
}

TEST(Popularity, OffMeansNoFetch)
{
    PopularityHub hub;
    FakeFetcher fetcher;
    PopularityServiceConfig config;
    config.cache_paths = {tmp_dir() + "/pop.v1"};
    PopularityService service(hub, fetcher, config);
    service.apply(false, "");
    service.refresh();
    service.wait_idle();
    EXPECT_EQ(fetcher.calls, 0);
    EXPECT_EQ(hub.index(), nullptr);
    EXPECT_FALSE(hub.enabled());
}

TEST(Popularity, FetchCachesAndFailureFallsBackToTheCache)
{
    const std::string dir = tmp_dir();
    PopularityServiceConfig config;
    config.cache_paths = {dir + "/pop.v1"};
    std::int64_t now = 1000000;
    config.now = [&now] { return now; };
    FakeFetcher fetcher;
    fetcher.result.status = 200;
    fetcher.result.body = fixture();
    {
        PopularityHub hub;
        PopularityService service(hub, fetcher, config);
        service.apply(true, "");
        service.wait_idle();
        EXPECT_EQ(fetcher.calls, 1);
        ASSERT_NE(hub.index(), nullptr);
        EXPECT_EQ(hub.status().updated, now);
        EXPECT_FALSE(hub.status().busy);
    }
    {
        // A new start within the TTL uses the saved copy and does not fetch.
        PopularityHub hub;
        PopularityService service(hub, fetcher, config);
        service.apply(true, "");
        service.wait_idle();
        EXPECT_EQ(fetcher.calls, 1);
        EXPECT_NE(hub.index(), nullptr);
    }
    {
        // Past the TTL the fetch fails: the cache stays in use and the status says so (no popup).
        now += 3 * 24 * 3600;
        fetcher.result = DocResult{};
        fetcher.result.error = "connection refused";
        PopularityHub hub;
        PopularityService service(hub, fetcher, config);
        service.apply(true, "");
        service.wait_idle();
        EXPECT_EQ(fetcher.calls, 2);
        ASSERT_NE(hub.index(), nullptr);
        EXPECT_NE(hub.status().message.find("Could not refresh"), std::string::npos);
        // A wrong URL never reaches the fetcher.
        service.apply(true, "http://insecure.test/x.json");
        service.wait_idle();
        EXPECT_EQ(fetcher.calls, 2);
    }
    {
        // A damaged cache and a failing network: no data, no crash.
        std::ofstream(dir + "/pop.v1", std::ios::binary | std::ios::trunc) << "garbage";
        PopularityHub hub;
        PopularityService service(hub, fetcher, config);
        service.apply(true, "");
        service.wait_idle();
        EXPECT_EQ(hub.index(), nullptr);
        EXPECT_NE(hub.status().message.find("Could not refresh"), std::string::npos);
    }
}

TEST(Popularity, LocalDownloadsPersist)
{
    const std::string path = tmp_dir() + "/downloads.v1";
    {
        PopularityHub hub;
        hub.set_downloads_paths({path});
        hub.count_download({"PPSA00001", "PPSA00002"});
        hub.count_download({"PPSA00001"});
    }
    PopularityHub again;
    again.set_downloads_paths({path});
    EXPECT_EQ(again.downloads().count({"PPSA00001"}), 2);
    EXPECT_EQ(again.downloads().count({"PPSA00002", "PPSA00009"}), 1);
    EXPECT_EQ(again.downloads().count({"PPSA00009"}), 0);
    LocalDownloads damaged;
    EXPECT_FALSE(damaged.load({tmp_dir() + "/missing"}));
    EXPECT_EQ(damaged.size(), 0u);
}

TEST(Popularity, ShortCounts)
{
    EXPECT_EQ(short_count(950), "950");
    EXPECT_EQ(short_count(4300), "4.3k");
    EXPECT_EQ(short_count(12000), "12k");
    EXPECT_EQ(short_count(1200000), "1.2M");
}

} // namespace
} // namespace orbit
