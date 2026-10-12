// Wings TV app - Art sources: the Prosperopatches address rules, the two Settings choices and what covers
// resolve to under each. No test uses the network.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/art_sources.hpp"
#include "orbit/cover_art.hpp"
#include "orbit/popularity.hpp"
#include "orbit/prefs.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

const std::string kHash(64, 'a');
const std::string kPic0 = "https://cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/pic0.webp";
const std::string kIcon = "https://cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/icon0.webp";
const std::string kIgdb = "https://images.igdb.com/igdb/image/upload/t_1080p/abc.jpg";

std::shared_ptr<const PopularityIndex> index_with(const std::string &hero_a, const std::string &hero_b)
{
    const auto record = [](const char *id, const std::string &hero)
    {
        return std::string("\"") + id + "\":{\"score\":60,\"confidence\":1.0,\"sources\":[\"igdb\"]" +
               (hero.empty() ? "" : ",\"hero\":\"" + hero + "\"") + "}";
    };
    const std::string doc = "{\"schemaVersion\":1,\"generatedAt\":\"2026-10-12T00:00:00Z\",\"attribution\":{},\"titles\":{" +
                            record("PPSA01234", hero_a) + "," + record("PPSA05678", hero_b) + "}}";
    auto index = std::make_shared<PopularityIndex>();
    std::string error;
    EXPECT_TRUE(PopularityIndex::parse(doc, index.get(), &error)) << error;
    return index;
}

Package package(const char *id, const char *title_id, const char *poster)
{
    Package p;
    p.id = id;
    p.title_id = title_id;
    p.poster_url = poster;
    return p;
}

TEST(ArtSources, OnlyTheExactProsperopatchesShapeIsAccepted)
{
    EXPECT_TRUE(is_prospero_pic0_url(kPic0));
    for (const std::string &bad : {
             std::string(), kIcon, kIgdb,
             "http://cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/pic0.webp",
             "https://cdn.prosperopatches.com.evil.test/titles/PPSA01234_" + kHash + "/pic0.webp",
             "https://evil.test/cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/pic0.webp",
             "https://cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/pic0.webp?x=1",
             "https://cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/pic0.webp/../icon0.webp",
             "https://cdn.prosperopatches.com/titles/CUSA01234_" + kHash + "/pic0.webp",
             "https://cdn.prosperopatches.com/titles/PPSA0123_" + kHash + "/pic0.webp",
             "https://cdn.prosperopatches.com/titles/PPSA01234_" + std::string(64, 'A') + "/pic0.webp",
             "https://cdn.prosperopatches.com/titles/PPSA01234_" + std::string(63, 'a') + "/pic0.webp",
             "https://cdn.prosperopatches.com/titles/PPSA01234_" + std::string(64, 'g') + "/pic0.webp",
             "https://cdn.prosperopatches.com/titles/PPSA01234_" + kHash + "/pic1.webp"})
        EXPECT_FALSE(is_prospero_pic0_url(bad)) << bad;
}

TEST(ArtSources, TheIconIsDerivedNextToThePicture)
{
    EXPECT_EQ(prospero_icon_url(kPic0), kIcon);
    EXPECT_EQ(prospero_icon_url(kIgdb), "");
    EXPECT_EQ(prospero_icon_url(kIcon), ""); // already an icon: not a picture address
    EXPECT_EQ(prospero_icon_url(""), "");
}

TEST(ArtSources, HeroSourceFiltersByAddress)
{
    EXPECT_TRUE(hero_url_allowed(kPic0, HeroSource::all));
    EXPECT_TRUE(hero_url_allowed(kIgdb, HeroSource::all));
    EXPECT_TRUE(hero_url_allowed(kPic0, HeroSource::prospero));
    EXPECT_FALSE(hero_url_allowed(kIgdb, HeroSource::prospero));
    EXPECT_FALSE(hero_url_allowed("https://cdn.cloudflare.steamstatic.com/x/library_hero.jpg", HeroSource::prospero));
}

TEST(ArtSources, ChoicesAreKeptAndDefaultToTodaysBehaviour)
{
    WingsLocalPrefs prefs;
    EXPECT_EQ(prefs.icon_source, IconSource::catalogue);
    EXPECT_EQ(prefs.hero_source, HeroSource::all);
    prefs.icon_source = IconSource::prospero;
    prefs.hero_source = HeroSource::prospero;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_EQ(back.icon_source, IconSource::prospero);
    EXPECT_EQ(back.hero_source, HeroSource::prospero);
    EXPECT_TRUE(back == prefs);
    // Older files and unknown values keep the defaults.
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("region=USA\n", &old));
    EXPECT_EQ(old.icon_source, IconSource::catalogue);
    EXPECT_EQ(old.hero_source, HeroSource::all);
    ASSERT_TRUE(decode_local_prefs("icon_source=banana\nhero_source=\n", &old));
    EXPECT_EQ(old.icon_source, IconSource::catalogue);
    EXPECT_EQ(old.hero_source, HeroSource::all);
}

TEST(ArtSources, CoversResolveToTheCatalogueUnlessProsperopatchesIsChosenAndKnown)
{
    PosterIndex posters;
    const std::vector<Package> packages = {package("1", "PPSA01234", "https://img.example/one.jpg"),
                                           package("2", "PPSA05678", "https://img.example/two.jpg"), // IGDB hero only
                                           package("3", "", "https://img.example/three.jpg"),         // no title id
                                           package("4", "PPSA09999", "https://img.example/four.jpg")}; // not in the data
    ASSERT_TRUE(posters.sync(1, packages));
    const auto index = index_with(kPic0, kIgdb);

    // Default: the catalogue, whatever the data says.
    EXPECT_FALSE(posters.set_icon_source(false, index));
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::cover), "https://img.example/one.jpg");

    // Chosen, but the data is not loaded yet: still the catalogue, and nothing changed.
    EXPECT_FALSE(posters.set_icon_source(true, nullptr));
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::cover), "https://img.example/one.jpg");

    // Chosen and loaded: covers change for titles with Prosperopatches art, not for the rest.
    EXPECT_TRUE(posters.set_icon_source(true, index));
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::cover), kIcon);
    EXPECT_EQ(posters.resolve("pkg:2", ArtKind::cover), "https://img.example/two.jpg");
    EXPECT_EQ(posters.resolve("pkg:3", ArtKind::cover), "https://img.example/three.jpg");
    EXPECT_EQ(posters.resolve("pkg:4", ArtKind::cover), "https://img.example/four.jpg");
    EXPECT_EQ(posters.resolve("pkg:99", ArtKind::cover), "");
    // Asking again with the same data changes nothing; new data does.
    EXPECT_FALSE(posters.set_icon_source(true, index));
    EXPECT_TRUE(posters.set_icon_source(true, index_with(kPic0, kPic0)));
    // Back to the catalogue.
    EXPECT_TRUE(posters.set_icon_source(false, index));
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::cover), "https://img.example/one.jpg");
    // A catalogue that changes keeps the choice.
    ASSERT_TRUE(posters.set_icon_source(true, index));
    ASSERT_TRUE(posters.sync(2, packages));
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::cover), kIcon);
}

} // namespace
} // namespace orbit
