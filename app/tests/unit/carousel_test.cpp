// Wings TV app - The Store carousel: which games, when it shows, the pad rules, the timer, the grid offset.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/carousel.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace orbit
{
namespace
{

using hui::Direction;

GameGroup game(const char *title, std::vector<std::string> ids)
{
    GameGroup g;
    g.title = title;
    g.title_ids = std::move(ids);
    return g;
}

TEST(Carousel, ResolvesIdsInTheFilesOrderAgainstTheOwnCatalogue)
{
    const std::vector<GameGroup> games = {game("A", {"PPSA00001"}), game("B", {"PPSA00002", "PPSA00022"}),
                                          game("C", {"PPSA00003"})};
    // Order follows the file; an id the player does not have is hidden.
    EXPECT_EQ(resolve_carousel({"PPSA00003", "PPSA09999", "PPSA00001"}, games), (std::vector<int>{2, 0}));
    // A game found through two ids is kept once.
    EXPECT_EQ(resolve_carousel({"PPSA00022", "PPSA00002", "PPSA00001"}, games), (std::vector<int>{1, 0}));
    EXPECT_TRUE(resolve_carousel({}, games).empty());
    EXPECT_TRUE(resolve_carousel({"PPSA00001"}, {}).empty());
}

TEST(Carousel, KeepsAtMostEightGames)
{
    std::vector<GameGroup> games;
    std::vector<std::string> ids;
    for (int i = 0; i < 12; ++i)
    {
        const std::string id = "PPSA0000" + std::to_string(i % 10) + std::to_string(i / 10);
        games.push_back(game("G", {id}));
        ids.push_back(id);
    }
    const std::vector<int> got = resolve_carousel(ids, games);
    ASSERT_EQ(got.size(), kCarouselMaxItems);
    EXPECT_EQ(got.front(), 0);
    EXPECT_EQ(got.back(), 7);
}

TEST(Carousel, ShowsOnlyInTheDefaultGridView)
{
    EXPECT_TRUE(carousel_shown(true, 3, true, 100));
    EXPECT_FALSE(carousel_shown(false, 3, true, 100)); // switch off
    EXPECT_FALSE(carousel_shown(true, 0, true, 100));  // nothing resolved
    EXPECT_FALSE(carousel_shown(true, 3, false, 100)); // a search
    EXPECT_FALSE(carousel_shown(true, 3, true, 0));    // empty grid
}

TEST(Carousel, FallsBackToTheMostPopularGames)
{
    // Keys: -1 = no data. The three largest, largest first; ties keep the catalogue order.
    EXPECT_EQ(pick_most_popular({10, -1, 70, 70, 5, 40}, 3), (std::vector<int>{2, 3, 5}));
    // Nothing known: the first ones.
    EXPECT_EQ(pick_most_popular({-1, -1, -1, -1}, 3), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(pick_most_popular({4, 9}, 5), (std::vector<int>{1, 0}));
    EXPECT_TRUE(pick_most_popular({}, 5).empty());
}

TEST(Carousel, WrapsSlides)
{
    EXPECT_EQ(carousel_wrap(0, -1, 4), 3);
    EXPECT_EQ(carousel_wrap(3, 1, 4), 0);
    EXPECT_EQ(carousel_wrap(2, 1, 4), 3);
    EXPECT_EQ(carousel_wrap(0, 1, 0), 0);
}

TEST(Carousel, ButtonsAreTheFocusStopsAndTheEdgesChangeSlide)
{
    // Install -> Favourite on Right, back on Left.
    CarouselStep s = carousel_step(1, kCarouselView, 3, Direction::right);
    EXPECT_TRUE(s.moved);
    EXPECT_FALSE(s.slide_changed);
    EXPECT_EQ(s.button, kCarouselFavourite);
    EXPECT_EQ(s.slide, 1);
    s = carousel_step(1, kCarouselFavourite, 3, Direction::left);
    EXPECT_EQ(s.button, kCarouselView);
    EXPECT_EQ(s.slide, 1);
    // The edge buttons change the slide and keep the button.
    s = carousel_step(1, kCarouselFavourite, 3, Direction::right);
    EXPECT_TRUE(s.slide_changed);
    EXPECT_EQ(s.slide, 2);
    EXPECT_EQ(s.button, kCarouselFavourite);
    s = carousel_step(1, kCarouselView, 3, Direction::left);
    EXPECT_EQ(s.slide, 0);
    EXPECT_EQ(s.button, kCarouselView);
    // Wraps both ways.
    EXPECT_EQ(carousel_step(2, kCarouselFavourite, 3, Direction::right).slide, 0);
    EXPECT_EQ(carousel_step(0, kCarouselView, 3, Direction::left).slide, 2);
}

TEST(Carousel, OneSlideRefusesTheEdgesAndOtherDirectionsDoNothing)
{
    CarouselStep s = carousel_step(0, kCarouselFavourite, 1, Direction::right);
    EXPECT_FALSE(s.moved);
    EXPECT_EQ(s.slide, 0);
    s = carousel_step(0, kCarouselView, 1, Direction::left);
    EXPECT_FALSE(s.moved);
    for (const Direction d : {Direction::up, Direction::down, Direction::none})
    {
        s = carousel_step(1, kCarouselView, 3, d);
        EXPECT_FALSE(s.moved);
        EXPECT_EQ(s.slide, 1);
        EXPECT_EQ(s.button, kCarouselView);
    }
}

TEST(Carousel, ClockAdvancesEveryEightSecondsAndPausesAndRestarts)
{
    CarouselClock clock;
    for (int i = 0; i < 7; ++i)
        EXPECT_FALSE(clock.update(1.0f, false, 4));
    EXPECT_TRUE(clock.update(1.0f, false, 4));
    EXPECT_FLOAT_EQ(clock.elapsed(), 0.0f);
    // Paused (focus, dialog, off screen): the count starts again afterwards.
    EXPECT_FALSE(clock.update(5.0f, false, 4));
    EXPECT_FALSE(clock.update(100.0f, true, 4));
    EXPECT_FALSE(clock.update(5.0f, false, 4));
    EXPECT_TRUE(clock.update(5.0f, false, 4));
    // One slide never advances.
    EXPECT_FALSE(clock.update(100.0f, false, 1));
    clock.restart();
    EXPECT_FLOAT_EQ(clock.elapsed(), 0.0f);
}

TEST(Carousel, GridScrollWithoutCarouselIsTheGridsOwnRule)
{
    constexpr float pitch = 330.5f;
    for (int row = 0; row < 40; ++row)
        EXPECT_FLOAT_EQ(grid_scroll_target(row, pitch, 0.0f), std::max(0.0f, static_cast<float>(row - 1) * pitch));
}

TEST(Carousel, GridScrollWithCarouselKeepsTheFocusedRowOnScreen)
{
    constexpr float pitch = 330.5f;
    constexpr float top = 240.0f;
    constexpr float bottom = 1000.0f;
    // Row 0 scrolls the hero away: the header and the first row sit where they do without a carousel.
    EXPECT_FLOAT_EQ(grid_scroll_target(0, pitch, kCarouselExtra), kCarouselExtra);
    for (int row = 0; row < 40; ++row)
    {
        const float scroll = grid_scroll_target(row, pitch, kCarouselExtra);
        const float y = top + kCarouselExtra + static_cast<float>(row) * pitch - scroll;
        // The same place as without a carousel.
        EXPECT_FLOAT_EQ(y, top + static_cast<float>(row) * pitch - grid_scroll_target(row, pitch, 0.0f)) << row;
        EXPECT_GE(y, top - 1.0f) << row;
        EXPECT_LE(y + 306.0f, bottom) << row;
    }
}

TEST(Carousel, HeroAtRestShowsTheGridHeaderAndTheTopOfTheFirstRow)
{
    // With the focus on the hero (scroll 0) the first row's tiles start under the words and well above the hint
    // bar (it covers from 960), so a good part of them peeks in; the header sits just above them.
    constexpr float top = 240.0f;
    const float first_row = top + kCarouselExtra;
    EXPECT_GT(first_row, 800.0f);
    EXPECT_LT(first_row, 960.0f - 100.0f);
    // The hero's picture is taller than Orbit's (which ends near 640) and ends before the tiles.
    EXPECT_GT(kCarouselArtH, 700.0f);
    EXPECT_LE(kCarouselArtH, first_row);
}

TEST(Carousel, HeroFadesAsThePageSlidesUp)
{
    EXPECT_FLOAT_EQ(carousel_fade(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(carousel_fade(kCarouselExtra), 0.0f);
    EXPECT_FLOAT_EQ(carousel_fade(kCarouselExtra * 3.0f), 0.0f);
    EXPECT_FLOAT_EQ(carousel_fade(-5.0f), 1.0f);
    EXPECT_GT(carousel_fade(kCarouselExtra * 0.2f), carousel_fade(kCarouselExtra * 0.4f));
}

} // namespace
} // namespace orbit
