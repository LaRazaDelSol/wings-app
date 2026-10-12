// Wings TV app - Live Steam review fallback: normalising, matching, response parsing, cache TTLs, rate limit and
// the service (gates, one request at a time, no network). Fixtures are recorded response shapes.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/steam_reviews.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <fstream>
#include <iterator>
#include <memory>

namespace orbit
{
namespace
{

std::string fixture(const char *name)
{
    std::ifstream in(std::string(HUI_SOURCE_DIR) + "/tests/fixtures/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string tmp_file()
{
    char path[] = "/tmp/wings-steam-XXXXXX";
    return mkdtemp(path) != nullptr ? std::string(path) + "/steam.v1" : std::string("/tmp/steam.v1");
}

// Answers by URL; records every request.
struct ScriptedFetcher final : DocFetcher
{
    std::vector<std::string> urls;
    DocResult search, reviews;
    DocResult get(const std::string &url, std::size_t) override
    {
        urls.push_back(url);
        return url.find("/api/storesearch/") != std::string::npos ? search : reviews;
    }
};

DocResult ok(std::string body)
{
    DocResult r;
    r.status = 200;
    r.body = std::move(body);
    return r;
}

std::shared_ptr<const PopularityIndex> index_with(const char *id)
{
    auto index = std::make_shared<PopularityIndex>();
    const std::string doc = std::string("{\"schemaVersion\":1,\"generatedAt\":\"2026-10-12T00:00:00Z\",") +
                            "\"attribution\":{},\"titles\":{\"" + id +
                            "\":{\"score\":80,\"rating\":80,\"ratingCount\":10,\"confidence\":0.9,"
                            "\"sources\":[\"igdb\"],\"fetchedAt\":\"2026-10-12\"}}}";
    std::string why;
    EXPECT_TRUE(PopularityIndex::parse(doc, index.get(), &why)) << why;
    return index;
}

struct Rig
{
    PopularityHub hub;
    ScriptedFetcher fetcher;
    std::vector<double> sleeps;
    double mono = 100.0;
    std::int64_t now = 1800000000;
    std::string path = tmp_file();
    std::unique_ptr<SteamReviewService> service;
    Rig()
    {
        SteamReviewConfig c;
        c.cache_paths = {path};
        c.now = [this] { return now; };
        c.mono = [this] { return mono; };
        c.sleep_s = [this](double s)
        {
            sleeps.push_back(s);
            mono += s;
        };
        service = std::make_unique<SteamReviewService>(hub, fetcher, c);
        fetcher.search = ok(fixture("steam_storesearch.json"));
        fetcher.reviews = ok(fixture("steam_appreviews.json"));
        hub.set_enabled(true);
        hub.set_index(index_with("PPSA00000_00"));
    }
};

const std::vector<std::string> kIds = {"PPSA01234_00"};

TEST(SteamReviews, NormalisesLikeTheMatcher)
{
    EXPECT_EQ(normalize_title("The Last of Us\xE2\x84\xA2 Part II [PPSA01] v1.05 @DUPLEX").key, "last of us part 2");
    EXPECT_EQ(normalize_title("Marvel's Spider-Man Remastered").key, "marvels spider man");
    EXPECT_EQ(normalize_title("Marvel's Spider-Man Remastered").editions, "remastered");
    EXPECT_EQ(normalize_title("Tom & Jerry").key, "tom and jerry");
    EXPECT_EQ(normalize_title("Elden Ring (USA) PS5").key, "elden ring");
    EXPECT_EQ(normalize_title("").key, "");
}

TEST(SteamReviews, AcceptsOnlyAConfidentExactMatch)
{
    std::vector<SteamCandidate> c;
    ASSERT_TRUE(parse_store_search(fixture("steam_storesearch.json"), &c));
    ASSERT_EQ(c.size(), 3u); // the "sub" item is dropped
    long id = 0;
    EXPECT_TRUE(choose_appid("ELDEN RING", c, &id));
    EXPECT_EQ(id, 1245620);
    EXPECT_TRUE(choose_appid("Elden Ring\xE2\x84\xA2 v1.10", c, &id));
    EXPECT_FALSE(choose_appid("Elden", c, &id)); // no fuzzy guess
    EXPECT_FALSE(choose_appid("Elden Ring Nightreign", c, &id));
    EXPECT_FALSE(choose_appid("Elden Ring Deluxe Edition", c, &id)); // edition sets differ
    // two different apps with the same name: ambiguous
    c.push_back({7, "Elden Ring"});
    EXPECT_FALSE(choose_appid("Elden Ring", c, &id));
    EXPECT_FALSE(choose_appid("", c, &id));
    std::vector<SteamCandidate> bad;
    EXPECT_FALSE(parse_store_search("{\"items\":5}", &bad));
    EXPECT_FALSE(parse_store_search("not json", &bad));
}

TEST(SteamReviews, ParsesTheSummary)
{
    SteamSummary s;
    ASSERT_TRUE(parse_review_summary(fixture("steam_appreviews.json"), &s));
    EXPECT_EQ(s.rating, 91);
    EXPECT_EQ(s.count, 1158474);
    EXPECT_EQ(live_rating_line(s), "Steam reviews, live: 91% positive (1.2M)");
    EXPECT_FALSE(parse_review_summary(fixture("steam_appreviews_few.json"), &s)); // < 10 votes
    EXPECT_FALSE(parse_review_summary("{\"success\":0}", &s));
    EXPECT_FALSE(parse_review_summary("{\"success\":1,\"query_summary\":{}}", &s));
}

TEST(SteamReviews, UrlsAreEncoded)
{
    EXPECT_EQ(steam_search_url("Marvel's Spider-Man & Co v1.05"),
              "https://store.steampowered.com/api/storesearch/?term=marvels%20spider%20man%20and%20co&cc=us&l=en");
    EXPECT_EQ(steam_reviews_url(1245620),
              "https://store.steampowered.com/appreviews/1245620?json=1&num_per_page=0&language=all&purchase_type=all");
}

TEST(SteamReviews, CacheTtlsAndRoundTrip)
{
    LiveReviewCache cache;
    SteamSummary s;
    s.rating = 90;
    s.count = 500;
    cache.put_ok("A", s, 5, 1000);
    cache.put_fail("B", 1000);
    SteamSummary got;
    EXPECT_EQ(cache.state("A", 1000 + kSteamOkTtlS - 1, &got), LiveReviewCache::State::good);
    EXPECT_EQ(got.rating, 90);
    EXPECT_EQ(cache.state("A", 1000 + kSteamOkTtlS), LiveReviewCache::State::unknown);
    EXPECT_EQ(cache.state("B", 1000 + kSteamFailTtlS - 1), LiveReviewCache::State::failed);
    EXPECT_EQ(cache.state("B", 1000 + kSteamFailTtlS), LiveReviewCache::State::unknown);
    EXPECT_EQ(cache.state("C", 1000), LiveReviewCache::State::unknown);
    EXPECT_EQ(cache.state("A", 500), LiveReviewCache::State::unknown); // clock went back
    LiveReviewCache copy;
    EXPECT_TRUE(copy.decode(cache.encode() + "garbage\nX|1|200|50|1|1\n"));
    EXPECT_EQ(copy.size(), 2u); // damaged and out-of-range lines are skipped
    const std::string path = tmp_file();
    EXPECT_EQ(cache.save({path}), 1);
    LiveReviewCache loaded;
    EXPECT_TRUE(loaded.load({path}));
    EXPECT_EQ(loaded.state("A", 1001), LiveReviewCache::State::good);
}

TEST(SteamReviews, RateLimiter)
{
    RateLimiter r(2.0);
    EXPECT_EQ(r.wait_needed(10.0), 0.0);
    r.note_request(10.0);
    EXPECT_DOUBLE_EQ(r.wait_needed(10.5), 1.5);
    EXPECT_EQ(r.wait_needed(12.0), 0.0);
    EXPECT_EQ(r.wait_needed(5.0), 0.0); // clock went back: not a stall
}

TEST(SteamReviews, ServiceFetchesOnceAndCaches)
{
    Rig rig;
    EXPECT_TRUE(rig.service->request(kIds, "Elden Ring"));
    rig.service->wait_idle();
    ASSERT_EQ(rig.fetcher.urls.size(), 2u);
    EXPECT_NE(rig.fetcher.urls[1].find("/appreviews/1245620"), std::string::npos);
    ASSERT_EQ(rig.sleeps.size(), 1u); // the second request waited out the interval
    EXPECT_DOUBLE_EQ(rig.sleeps[0], kSteamMinIntervalS);
    SteamSummary s;
    ASSERT_TRUE(rig.service->lookup(kIds, &s));
    EXPECT_EQ(s.rating, 91);
    EXPECT_FALSE(rig.service->request(kIds, "Elden Ring")); // cached: no second job
    EXPECT_EQ(rig.fetcher.urls.size(), 2u);
}

TEST(SteamReviews, ServiceGates)
{
    Rig rig;
    rig.hub.set_enabled(false);
    EXPECT_FALSE(rig.service->request(kIds, "Elden Ring"));
    rig.hub.set_enabled(true);
    EXPECT_FALSE(rig.service->request({"PPSA00000_00"}, "Elden Ring")); // the index knows it
    rig.hub.set_index(nullptr);
    EXPECT_FALSE(rig.service->request(kIds, "Elden Ring")); // index not loaded yet
    rig.service->wait_idle();
    EXPECT_TRUE(rig.fetcher.urls.empty());
}

TEST(SteamReviews, FailuresAreNegativeCachedAndOfflineIsNot)
{
    {
        Rig rig;
        EXPECT_TRUE(rig.service->request(kIds, "Some Obscure Game"));
        rig.service->wait_idle();
        EXPECT_EQ(rig.fetcher.urls.size(), 1u); // no match: no review request
        SteamSummary s;
        EXPECT_FALSE(rig.service->lookup(kIds, &s));
        EXPECT_FALSE(rig.service->request(kIds, "Some Obscure Game"));
        rig.now += kSteamFailTtlS - 1;
        EXPECT_FALSE(rig.service->request(kIds, "Some Obscure Game"));
        rig.now += 1;
        EXPECT_TRUE(rig.service->request(kIds, "Some Obscure Game")); // a day later
        rig.service->wait_idle();
    }
    {
        Rig rig;
        rig.fetcher.search.status = 0;
        rig.fetcher.search.error = "dns";
        EXPECT_TRUE(rig.service->request(kIds, "Elden Ring"));
        rig.service->wait_idle();
        EXPECT_EQ(rig.service->cache().size(), 0u);             // nothing saved
        EXPECT_FALSE(rig.service->request(kIds, "Elden Ring")); // short back-off
        rig.now += kSteamNetBackoffS;
        EXPECT_TRUE(rig.service->request(kIds, "Elden Ring"));
        rig.service->wait_idle();
    }
}

} // namespace
} // namespace orbit
