// Wings TV app - Store grid "Tile details" choice: the three modes and how the prefs file keeps it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/prefs.hpp"
#include "orbit/tile_details.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

TEST(TileDetails, DefaultIsOnFocus)
{
    EXPECT_EQ(WingsLocalPrefs{}.tile_details, TileDetails::on_focus);
}

TEST(TileDetails, AlwaysShowsOnEveryTile)
{
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::always, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::always, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(tier_legend_alpha(TileDetails::always, 0.0f), 1.0f);
}

TEST(TileDetails, NeverHidesThemAndTheLegend)
{
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::never, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::never, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(tier_legend_alpha(TileDetails::never, 1.0f), 0.0f);
}

TEST(TileDetails, OnFocusFollowsTheFocusFade)
{
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::on_focus, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::on_focus, 0.4f), 0.4f);
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::on_focus, 1.0f), 1.0f);
    // Spring overshoot never goes past opaque or below hidden.
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::on_focus, 1.2f), 1.0f);
    EXPECT_FLOAT_EQ(tile_detail_alpha(TileDetails::on_focus, -0.1f), 0.0f);
    // The legend only while the focused tile shows its tier.
    EXPECT_FLOAT_EQ(tier_legend_alpha(TileDetails::on_focus, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(tier_legend_alpha(TileDetails::on_focus, 1.0f), 1.0f);
}

TEST(TileDetails, CyclesThroughTheThreeValues)
{
    EXPECT_EQ(next_tile_details(TileDetails::always), TileDetails::on_focus);
    EXPECT_EQ(next_tile_details(TileDetails::on_focus), TileDetails::never);
    EXPECT_EQ(next_tile_details(TileDetails::never), TileDetails::always);
    EXPECT_STREQ(tile_details_label(TileDetails::always), "Always");
    EXPECT_STREQ(tile_details_label(TileDetails::on_focus), "On focus");
    EXPECT_STREQ(tile_details_label(TileDetails::never), "Never");
}

TEST(TileDetails, EachValueSurvivesTheFile)
{
    for (const TileDetails value : {TileDetails::always, TileDetails::on_focus, TileDetails::never})
    {
        WingsLocalPrefs saved;
        saved.tile_details = value;
        saved.region = "EUR";
        WingsLocalPrefs back;
        ASSERT_TRUE(decode_local_prefs(encode_local_prefs(saved), &back));
        EXPECT_EQ(back, saved);
    }
}

TEST(TileDetails, OldFilesWithoutTheKeyLoadTheDefault)
{
    // A file written before the choice existed (same version 1 container payload).
    WingsLocalPrefs out;
    ASSERT_TRUE(decode_local_prefs("firmware=9.xx\nregion=JPN\nformat=\nprefer_debrid=0\npopularity=1\n"
                                   "popularity_url=\n",
                                   &out));
    EXPECT_EQ(out.tile_details, TileDetails::on_focus);
    EXPECT_EQ(out.region, "JPN");
    EXPECT_FALSE(out.prefer_debrid);
    EXPECT_TRUE(out.popularity_enabled);
}

TEST(TileDetails, UnknownValuesKeepTheDefault)
{
    WingsLocalPrefs out;
    ASSERT_TRUE(decode_local_prefs("tile_details=sometimes\n", &out));
    EXPECT_EQ(out.tile_details, TileDetails::on_focus);
    ASSERT_TRUE(decode_local_prefs("tile_details=never\n", &out));
    EXPECT_EQ(out.tile_details, TileDetails::never);
    TileDetails parsed = TileDetails::always;
    EXPECT_FALSE(parse_tile_details("", &parsed));
    EXPECT_EQ(parsed, TileDetails::always);
}

} // namespace
} // namespace orbit
