// Wings TV app - tests for the colour system: stable hues, contrast of
// text over the worst wash, and a wash that only ever animates one hue.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/theme.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

using theme::Wash;

TEST(Colour, TabAndSectionHuesAreFixedByTheSpec)
{
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_store), 245.0f);
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_downloads), 190.0f);
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_library), 270.0f);
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_files), 205.0f);
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_sources), 225.0f);
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_settings), 235.0f);
    EXPECT_FLOAT_EQ(theme::tab_hue(theme::place_logs), 215.0f);
    // Settings: Cover art 235, Store grid 247, Link ranking 259, Popularity 223, About 211.
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_cover_art), 235.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_store_grid), 247.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_link_ranking), 259.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_popularity), 223.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_about), 211.0f);
    // Filters: Show 245, Region and firmware 257, Format and host 233, Source 269, Sort 221.
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_filter_show), 245.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_filter_region_fw), 257.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_filter_format_host), 233.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_filter_source), 269.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_filter_sort), 221.0f);
    // Sort menu: Order 245, Popularity 257, Library state 233.
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_sort_order), 245.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_sort_popularity), 257.0f);
    EXPECT_FLOAT_EQ(theme::accent_hue(theme::accent_sort_library), 233.0f);
    // Pure lookups: the same ask gives the same colour, and no section strays more than 24
    // degrees from its place.
    for (int a = 0; a < theme::accent_count; ++a)
    {
        const theme::SectionSpec s = theme::section_spec(a);
        EXPECT_LE(std::fabs(s.step), 24.0f) << a;
        const ui::Color c1 = theme::accent_color(a);
        const ui::Color c2 = theme::accent_color(a);
        EXPECT_FLOAT_EQ(c1.r, c2.r);
        EXPECT_FLOAT_EQ(c1.b, c2.b);
    }
}

TEST(Colour, WashColoursStayBelowTheValueCapAndSemanticsAreAliases)
{
    for (int p = 0; p < theme::place_count; ++p)
    {
        const ui::Color c = theme::tab_color(p);
        EXPECT_LE(std::max({c.r, c.g, c.b}), 0.70f + 1e-4f) << p;
    }
    EXPECT_FLOAT_EQ(theme::semantic::good.g, ui::color::tier_green.g);
    EXPECT_FLOAT_EQ(theme::semantic::caution.r, ui::color::tier_yellow.r);
    EXPECT_FLOAT_EQ(theme::semantic::bad.b, ui::color::tier_red.b);
    EXPECT_LE(theme::kWashTop, theme::kWashMax);
    EXPECT_LE(theme::kWashMax, 0.22f);
}

TEST(Colour, TextHasContrastOverTheWorstWash)
{
    // Body text (text2, the dimmest ink used for sentences) at 4.5:1 or better under every wash.
    EXPECT_GE(theme::worst_wash_contrast(ui::color::text2), theme::kMinTextContrast);
    EXPECT_GE(theme::worst_wash_contrast(ui::color::text), theme::kMinTextContrast);
    // The helper itself: black on white is 21:1, equal colours 1:1.
    EXPECT_NEAR(theme::contrast(ui::Color::rgb(0xffffff), ui::Color::rgb(0x000000)), 21.0f, 0.01f);
    EXPECT_NEAR(theme::contrast(ui::color::text, ui::color::text), 1.0f, 1e-4f);
}

TEST(Colour, WashRetargetsByPlaceThenSectionAndNeverAnimatesTwoHues)
{
    Wash w;
    w.retarget(theme::place_store, theme::accent_none);
    EXPECT_TRUE(w.settled()); // the first aim lands at once
    const ui::Color store = theme::tab_color(theme::place_store);
    EXPECT_NEAR(w.color().b, store.b, 1e-4f);

    // A tab switch: 400 ms, the colour moves from where it is toward the new place only.
    w.retarget(theme::place_settings, theme::accent_none);
    EXPECT_FALSE(w.settled());
    EXPECT_FLOAT_EQ(w.duration, theme::kTabCrossfade);
    w.update(0.2f);
    EXPECT_FALSE(w.settled());
    w.update(0.25f);
    EXPECT_TRUE(w.settled());
    EXPECT_NEAR(w.color().g, theme::tab_color(theme::place_settings).g, 1e-4f);

    // A section within the place: 250 ms drift to the section hue.
    w.retarget(theme::place_settings, theme::accent_about);
    EXPECT_FLOAT_EQ(w.duration, theme::kSectionDrift);
    // Interrupted half way: the new move starts from the colour now (no jump) and the old target
    // is dropped (retargeted, not queued).
    w.update(0.1f);
    const ui::Color mid = w.color();
    w.retarget(theme::place_settings, theme::accent_store_grid);
    EXPECT_NEAR(w.color().r, mid.r, 1e-4f);
    EXPECT_NEAR(w.color().g, mid.g, 1e-4f);
    EXPECT_NEAR(w.color().b, mid.b, 1e-4f);
    EXPECT_EQ(w.accent, theme::accent_store_grid);
    w.update(1.0f);
    const ui::Color want = theme::accent_color(theme::accent_store_grid);
    EXPECT_NEAR(w.color().r, want.r, 1e-4f);
    EXPECT_NEAR(w.color().b, want.b, 1e-4f);

    // An accent of another place is ignored: the place hue is the target.
    w.retarget(theme::place_downloads, theme::accent_store_grid);
    EXPECT_EQ(w.accent, theme::accent_none);
    w.update(1.0f);
    EXPECT_NEAR(w.color().g, theme::tab_color(theme::place_downloads).g, 1e-4f);
}

} // namespace
} // namespace orbit
