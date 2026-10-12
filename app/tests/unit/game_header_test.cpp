// Wings TV app - The game page header: hero URL validation and lookup, popularity parsing of
// "hero", the info cards, the rating row, the buttons' focus steps and the layout.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/cover_art.hpp"
#include "orbit/game_header.hpp"
#include "orbit/popularity.hpp"

#include <gtest/gtest.h>

#include <string>

namespace orbit
{
namespace
{

std::string title_doc(const std::string &titles)
{
    return "{\"schemaVersion\":1,\"generatedAt\":\"2026-10-12T00:00:00Z\",\"attribution\":{},\"titles\":{" + titles +
           "}}";
}

std::string title_rec(const char *id, double confidence, const std::string &extra = {})
{
    return std::string("\"") + id + "\":{\"score\":70,\"rating\":88,\"ratingCount\":4300,\"confidence\":" +
           std::to_string(confidence) + ",\"sources\":[\"igdb\"]" + extra + "}";
}

// ---- hero URLs ----

TEST(HeroUrl, OnlyShortHttpsUrlsWithoutCredentialsAreValid)
{
    EXPECT_TRUE(valid_hero_url("https://cdn.example.com/hero/ppsa01234.webp"));
    EXPECT_FALSE(valid_hero_url("http://cdn.example.com/hero.webp"));
    EXPECT_FALSE(valid_hero_url("ftp://cdn.example.com/hero.webp"));
    EXPECT_FALSE(valid_hero_url("//cdn.example.com/hero.webp"));
    EXPECT_FALSE(valid_hero_url(""));
    EXPECT_FALSE(valid_hero_url("https://"));
    EXPECT_FALSE(valid_hero_url("https://user:pw@cdn.example.com/hero.webp"));
    EXPECT_FALSE(valid_hero_url("https://cdn.example.com/a b.webp"));
    EXPECT_FALSE(valid_hero_url("https://cdn.example.com/a\nb.webp"));
    const std::string longest = "https://cdn.example.com/" + std::string(kHeroUrlMax, 'a');
    EXPECT_FALSE(valid_hero_url(longest));
    const std::string fits = "https://cdn.example.com/" + std::string(kHeroUrlMax - 24, 'a');
    EXPECT_EQ(fits.size(), kHeroUrlMax);
    EXPECT_TRUE(valid_hero_url(fits));
}

TEST(HeroUrl, TheRegistryHoldsOnlyValidUrlsAndFeedsThePosterResolver)
{
    hero_urls().clear();
    PosterIndex posters;
    EXPECT_FALSE(hero_urls().set("pkg:1", "http://insecure.example/h.webp"));
    EXPECT_FALSE(hero_urls().set("", "https://cdn.example.com/h.webp"));
    EXPECT_EQ(hero_urls().size(), 0u);
    // Nothing registered: grid tiles (and every page without a hero) resolve nothing.
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::hero), "");
    EXPECT_TRUE(hero_urls().set("pkg:1", "https://cdn.example.com/h.webp"));
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::hero), "https://cdn.example.com/h.webp");
    EXPECT_EQ(posters.resolve("pkg:2", ArtKind::hero), "");
    // The cover is not the hero, and installed-game keys are never resolved here.
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::cover), "");
    EXPECT_EQ(posters.resolve("app:PPSA01234", ArtKind::hero), "");
    hero_urls().clear();
    EXPECT_EQ(posters.resolve("pkg:1", ArtKind::hero), "");
}

TEST(HeroUrl, TheRegistryStaysSmall)
{
    hero_urls().clear();
    for (int i = 0; i < 40; ++i)
        EXPECT_TRUE(hero_urls().set("pkg:" + std::to_string(i), "https://cdn.example.com/h.webp"));
    EXPECT_LE(hero_urls().size(), 8u);
    hero_urls().clear();
}

// ---- the popularity file's "hero" ----

TEST(HeroUrl, ThePopularityIndexKeepsAnHttpsHeroAndIgnoresTheRest)
{
    const std::string text = title_doc(
        title_rec("PPSA00001", 0.9, ",\"hero\":\"https://cdn.example.com/a.webp\"") + "," +
        title_rec("PPSA00002", 0.9, ",\"hero\":\"http://cdn.example.com/b.webp\"") + "," +
        title_rec("PPSA00003", 0.9, ",\"hero\":42") + "," + title_rec("PPSA00004", 0.9) + "," +
        title_rec("PPSA00005", 0.9, ",\"hero\":\"https://cdn.example.com/" + std::string(400, 'x') + "\""));
    PopularityIndex index;
    std::string error;
    ASSERT_TRUE(PopularityIndex::parse(text, &index, &error)) << error;
    EXPECT_EQ(index.find("PPSA00001")->hero, "https://cdn.example.com/a.webp");
    EXPECT_EQ(index.find("PPSA00002")->hero, "");
    EXPECT_EQ(index.find("PPSA00003")->hero, "");
    EXPECT_EQ(index.find("PPSA00004")->hero, "");
    EXPECT_EQ(index.find("PPSA00005")->hero, "");
}

TEST(HeroUrl, AGamesHeroIsTheFirstTitleIdThatHasOneRegardlessOfConfidence)
{
    const std::string text = title_doc(title_rec("PPSA00010", 0.9) + "," +
                                       title_rec("PPSA00011", 0.2, ",\"hero\":\"https://cdn.example.com/low.webp\"") +
                                       "," + title_rec("PPSA00012", 0.9, ",\"hero\":\"https://cdn.example.com/hi.webp\""));
    PopularityIndex index;
    ASSERT_TRUE(PopularityIndex::parse(text, &index, nullptr));
    EXPECT_EQ(index.hero({"PPSA00010", "PPSA00011", "PPSA00012"}), "https://cdn.example.com/low.webp");
    EXPECT_EQ(index.hero({"PPSA00012"}), "https://cdn.example.com/hi.webp");
    EXPECT_EQ(index.hero({"PPSA00010"}), "");
    EXPECT_EQ(index.hero({"PPSA09999"}), "");
    EXPECT_EQ(index.hero({}), "");
    // A low-confidence record still hides its rating: only the art is taken from it.
    EXPECT_EQ(index.best({"PPSA00011"}), nullptr);
    EXPECT_FALSE(header_rating(index.best({"PPSA00011"})).shown);
}

// ---- cards ----

HeaderFacts download_facts()
{
    HeaderFacts f;
    f.regions = {"USA", "EUR"};
    f.region = "USA";
    f.firmware = "12.xx";
    f.fit = FwFit::too_new;
    f.fit_known = true;
    f.format = "exFAT";
    f.version = "v01.055";
    f.source = "Community Mirror";
    f.sources = {"Community Mirror", "Other"};
    f.size_text = "61.2 GB";
    f.tier = LinkTier::debrid;
    f.have_download = true;
    return f;
}

TEST(HeaderCards, ListEveryFactInDisplayOrderWithTheTierColour)
{
    const std::vector<HeaderCard> cards = build_header_cards(download_facts());
    ASSERT_EQ(cards.size(), 7u);
    EXPECT_EQ(cards[0].label, "Region");
    EXPECT_EQ(cards[0].value, "USA");
    EXPECT_EQ(cards[1].label, "Firmware");
    EXPECT_EQ(cards[1].tone, CardTone::fit);
    EXPECT_EQ(cards[1].fit, FwFit::too_new);
    EXPECT_EQ(cards[2].label, "Format");
    EXPECT_EQ(cards[3].label, "Size");
    EXPECT_EQ(cards[4].label, "Version");
    EXPECT_EQ(cards[5].label, "Source");
    EXPECT_EQ(cards[6].label, "Links");
    EXPECT_EQ(cards[6].tone, CardTone::tier);
    EXPECT_EQ(cards[6].tier, LinkTier::debrid);
    EXPECT_EQ(cards[6].value, tier_label(LinkTier::debrid));
}

TEST(HeaderCards, LeaveOutWhatIsUnknownAndFallBackToTheGamesLists)
{
    HeaderFacts f;
    f.regions = {"USA", "EUR", "JPN", "ASIA", "HK"};
    f.sources = {"Community Mirror", "Other", "Third"};
    const std::vector<HeaderCard> cards = build_header_cards(f);
    ASSERT_EQ(cards.size(), 2u); // no download yet: no firmware, format, version, size or tier
    EXPECT_EQ(cards[0].value, "USA, EUR, JPN +2");
    EXPECT_EQ(cards[1].label, "Source");
    EXPECT_EQ(cards[1].value, "Community Mirror +2");
    EXPECT_TRUE(build_header_cards(HeaderFacts{}).empty());
}

TEST(HeaderCards, AFirmwareNoteStandsInWhenTheFirmwareIsNotStated)
{
    HeaderFacts f;
    f.firmware_note = "fw ?";
    const std::vector<HeaderCard> cards = build_header_cards(f);
    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].label, "Firmware");
    EXPECT_EQ(cards[0].value, "fw ?");
    EXPECT_EQ(cards[0].tone, CardTone::plain); // no firmware set: no colour
}

TEST(HeaderCards, LongValuesAreCutOnACharacterBoundary)
{
    HeaderFacts f;
    f.source = std::string(20, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    const std::vector<HeaderCard> cards = build_header_cards(f);
    ASSERT_EQ(cards.size(), 1u);
    const std::string &v = cards[0].value;
    ASSERT_GE(v.size(), 3u);
    EXPECT_EQ(v.substr(v.size() - 3), "\xE2\x80\xA6");
    EXPECT_LE(v.size(), kHeaderValueMax + 3);
    // Whole characters only: every byte before the ellipsis starts or continues a pair.
    std::size_t at = 20;
    while (at + 1 < v.size() - 3)
    {
        EXPECT_EQ(static_cast<unsigned char>(v[at]), 0xC3u);
        at += 2;
    }
}

// ---- rating ----

TEST(HeaderRating, NoRecordShowsNothing)
{
    EXPECT_FALSE(header_rating(nullptr).shown);
}

TEST(HeaderRating, ShowsTheScoreAndTheRatingWithItsCount)
{
    PopRecord r;
    r.score = 87;
    r.rating = 92;
    r.rating_count = 4300;
    HeaderRating h = header_rating(&r);
    ASSERT_TRUE(h.shown);
    EXPECT_EQ(h.score, 87);
    EXPECT_EQ(h.rating_text, "92 (4.3k)");
    r.rating = -1;
    h = header_rating(&r);
    EXPECT_TRUE(h.shown);
    EXPECT_EQ(h.rating_text, "");
    r.rating = 92;
    r.rating_count = -1;
    EXPECT_EQ(header_rating(&r).rating_text, "92");
}

// ---- buttons ----

TEST(HeaderButtons, LeftAndRightStopAtTheEdges)
{
    EXPECT_EQ(header_step(0, hui::Direction::right), 1);
    EXPECT_EQ(header_step(1, hui::Direction::right), 2);
    EXPECT_EQ(header_step(2, hui::Direction::right), 2);
    EXPECT_EQ(header_step(2, hui::Direction::left), 1);
    EXPECT_EQ(header_step(0, hui::Direction::left), 0);
    EXPECT_EQ(header_step(1, hui::Direction::up), 1);
}

TEST(HeaderButtons, TheMoreMenuOffersWhatThePageCanDo)
{
    EXPECT_TRUE(header_more_items(false, false).empty());
    const std::vector<HeaderMore> plain = header_more_items(true, false);
    ASSERT_EQ(plain.size(), 2u);
    EXPECT_EQ(plain[0], HeaderMore::filters);
    EXPECT_EQ(plain[1], HeaderMore::cycle_order);
    const std::vector<HeaderMore> set = header_more_items(true, true);
    ASSERT_EQ(set.size(), 3u);
    EXPECT_EQ(set[1], HeaderMore::clear_filters);
}

// ---- layout ----

TEST(HeaderLayout, CardsWrapAfterSixAndPushTheListDown)
{
    const HeaderLayout none = header_layout(1920.0f, 0, false, false);
    const HeaderLayout six = header_layout(1920.0f, 6, false, false);
    const HeaderLayout seven = header_layout(1920.0f, 7, false, false);
    EXPECT_EQ(none.card_rows, 0);
    EXPECT_EQ(six.card_rows, 1);
    EXPECT_EQ(seven.card_rows, 2);
    EXPECT_GE(six.list_top, none.list_top); // both sit on the big cover's floor
    EXPECT_GT(six.buttons_top, none.buttons_top);
    EXPECT_GT(seven.list_top, six.list_top);
    // Six cards fill the text column exactly.
    EXPECT_NEAR(six.card_w * 6.0f + 12.0f * 5.0f, six.text_w, 0.01f);
    EXPECT_NEAR(six.text_x + six.text_w, 1920.0f - 100.0f, 0.01f);
}

TEST(HeaderLayout, RatingAddsARowAndATwoLineTitleShrinks)
{
    const HeaderLayout plain = header_layout(1920.0f, 4, false, false);
    const HeaderLayout rated = header_layout(1920.0f, 4, true, false);
    EXPECT_EQ(plain.rating_top, 0.0f);
    EXPECT_GT(rated.rating_top, 0.0f);
    EXPECT_GT(rated.buttons_top, plain.buttons_top);
    const HeaderLayout two = header_layout(1920.0f, 4, false, true);
    EXPECT_EQ(two.title_lines, 2);
    EXPECT_LT(two.title_size, plain.title_size);
    EXPECT_GT(two.meta_baseline, plain.meta_baseline);
    EXPECT_LT(two.title_baseline[1], two.cards_top); // the second line stays above the cards
    // The list starts under the buttons and under the big cover.
    EXPECT_GE(rated.list_top, rated.buttons_top + 64.0f);
    EXPECT_GE(plain.list_top, 124.0f + 380.0f);
}

TEST(HeaderState, ResetPutsTheFocusOnInstallWithTheHeaderShown)
{
    GameHeaderState s;
    s.on_header = false;
    s.button = 2;
    s.slide.snap(1.0f);
    s.hero_url = "https://cdn.example.com/h.webp";
    s.hero_resolved = true;
    s.reset();
    EXPECT_TRUE(s.on_header);
    EXPECT_EQ(s.button, 0);
    EXPECT_EQ(s.slide.value, 0.0f);
    EXPECT_TRUE(s.hero_url.empty());
    EXPECT_FALSE(s.hero_resolved);
}

TEST(HeaderState, LeavingTheHeaderSlidesTheListUp)
{
    GameHeaderState s;
    s.on_header = false;
    for (int i = 0; i < 120; ++i)
        s.update(1.0f / 60.0f, true);
    EXPECT_NEAR(s.slide.value, 1.0f, 0.001f);
    EXPECT_NEAR(s.focus[0], 0.0f, 0.001f);
    s.on_header = true;
    for (int i = 0; i < 120; ++i)
        s.update(1.0f / 60.0f, true);
    EXPECT_NEAR(s.slide.value, 0.0f, 0.001f);
    EXPECT_NEAR(s.focus[0], 1.0f, 0.01f);
}

} // namespace
} // namespace orbit
