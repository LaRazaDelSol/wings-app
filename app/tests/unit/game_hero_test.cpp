// Wings TV app - The game page hero: chips, facts panel rows and the first viewport's geometry.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_hero.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

HeaderFacts full_facts()
{
    HeaderFacts f;
    f.regions = {"USA", "EUR"};
    f.region = "USA";
    f.firmware = "12.xx";
    f.fit = FwFit::runs;
    f.fit_known = true;
    f.format = "exFAT";
    f.version = "01.002.000";
    f.source = "Alpha";
    f.sources = {"Alpha", "Beta", "Gamma"};
    f.size_text = "61.2 GB";
    f.tier = LinkTier::unknown;
    f.have_download = true;
    f.parts = "22 parts (1-22)";
    f.link_count = 22;
    return f;
}

TEST(HeroChips, AreBuiltFromTheFactsInOrderAndOnlyWhenKnown)
{
    const std::vector<hero::Chip> chips = hero::chips(full_facts());
    ASSERT_EQ(chips.size(), 3u); // region, firmware fit, format (tier unknown: no chip)
    EXPECT_EQ(chips[0].text, "USA");
    EXPECT_EQ(chips[1].tone, hero::Tone::fit);
    EXPECT_EQ(chips[1].fit, FwFit::runs);
    EXPECT_EQ(chips[2].text, "exFAT");
    EXPECT_TRUE(hero::chips(HeaderFacts{}).empty());
}

TEST(HeroChips, TheFirmwareChipCarriesAWordNotJustAColour)
{
    HeaderFacts f = full_facts();
    f.fit = FwFit::too_new;
    const std::vector<hero::Chip> chips = hero::chips(f);
    ASSERT_GE(chips.size(), 2u);
    EXPECT_EQ(chips[1].fit, FwFit::too_new);
    EXPECT_NE(chips[1].text.find("12.xx"), std::string::npos);
    f.fit_known = false; // no firmware set: plain "Firmware 12.xx", no semantic colour
    const std::vector<hero::Chip> plain = hero::chips(f);
    ASSERT_GE(plain.size(), 2u);
    EXPECT_EQ(plain[1].tone, hero::Tone::plain);
}

TEST(HeroChips, ChipsNeverWrapTheyDropFromTheEnd)
{
    EXPECT_EQ(hero::chips_that_fit({100.0f, 100.0f, 100.0f}, 1000.0f, 10.0f), 3);
    EXPECT_EQ(hero::chips_that_fit({100.0f, 100.0f, 100.0f}, 215.0f, 10.0f), 2);
    EXPECT_EQ(hero::chips_that_fit({100.0f, 100.0f, 100.0f}, 205.0f, 10.0f), 1);
    EXPECT_EQ(hero::chips_that_fit({300.0f}, 200.0f, 10.0f), 0);
    EXPECT_EQ(hero::chips_that_fit({}, 200.0f, 10.0f), 0);
}

TEST(HeroChips, TheTierChipComesFromTheShownDownload)
{
    HeaderFacts f = full_facts();
    f.tier = LinkTier::supported;
    const std::vector<hero::Chip> chips = hero::chips(f);
    ASSERT_EQ(chips.size(), 4u);
    EXPECT_EQ(chips.back().tone, hero::Tone::tier);
    f.have_download = false; // only the game's own lists: no claim about a link tier
    EXPECT_EQ(hero::chips(f).size(), 3u);
}

TEST(HeroFacts, RowsComeFromDataAndTheLinksRowIsLastUnderTheDivider)
{
    const std::vector<hero::Fact> rows = hero::fact_rows(full_facts());
    ASSERT_EQ(rows.size(), 7u);
    EXPECT_EQ(rows[0].icon, hero::Icon::source);
    EXPECT_EQ(rows[0].value, "Alpha, Beta +1");
    EXPECT_EQ(rows[1].value, "61.2 GB");
    EXPECT_EQ(rows[3].icon, hero::Icon::firmware);
    EXPECT_FALSE(rows[3].accent.empty()); // "Fits": colour with its word
    EXPECT_EQ(rows[3].tone, hero::Tone::fit);
    EXPECT_EQ(rows[5].value, "22 parts (1-22)");
    EXPECT_TRUE(hero::has_links_row(rows));
    EXPECT_EQ(hero::rows_above_divider(rows), 6);
    EXPECT_EQ(rows.back().value, "22 links");
}

TEST(HeroFacts, MissingDataLeavesNoRowAndNothingIsInvented)
{
    HeaderFacts f;
    EXPECT_TRUE(hero::fact_rows(f).empty());
    f.size_text = "4 GB";
    f.link_count = 1;
    const std::vector<hero::Fact> rows = hero::fact_rows(f);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].icon, hero::Icon::size);
    EXPECT_EQ(rows[1].icon, hero::Icon::links);
    EXPECT_TRUE(rows[1].accent.empty()); // no shown download: no tier word
    EXPECT_EQ(hero::rows_above_divider(rows), 1);
}

TEST(HeroLayout, EverythingFitsTheFirstViewportWithoutOverlap)
{
    for (const bool two : {false, true})
    {
        for (const bool rating : {false, true})
        {
            for (int rows = 0; rows <= static_cast<int>(hero::kMaxFactRows); ++rows)
            {
                const hero::Layout l = hero::layout(1920.0f, two, true, rating, true, true, rows, true);
                // The cover is the page identity: about 340 px, lower than the top bar.
                EXPECT_GE(l.icon.w, 320.0f);
                EXPECT_LE(l.icon.w, 360.0f);
                EXPECT_GE(l.icon.y, 160.0f);
                EXPECT_LT(l.icon.y + l.icon.h, l.chips_cy - 18.0f - 20.0f); // clear of the tag chips
                // The title block is centred on the cover's middle or above it, the meta line below it.
                EXPECT_LE(l.title_baseline[0], l.icon.cy() + 24.0f);
                EXPECT_GT(l.meta_baseline, l.title_baseline[two ? 1 : 0]);
                EXPECT_LT(l.meta_baseline, l.icon.y + l.icon.h);
                // The title block ends above the chips and does not run under the panel.
                const float title_bottom = l.meta_baseline + 8.0f;
                EXPECT_LT(title_bottom, l.chips_cy - 18.0f);
                if (title_bottom >= l.panel.y)
                    EXPECT_LE(l.title_x + l.title_w + 20.0f, l.panel.x);
                EXPECT_GE(l.title_w, 700.0f);
                EXPECT_LE(l.title_x + l.title_w, 1920.0f - ui::kGutter);
                // A two-line title still ends above its meta line.
                if (two)
                    EXPECT_LT(l.title_baseline[1], l.meta_baseline - 20.0f);
                // The stack, top to bottom: chips, rating, offer, reason, buttons.
                if (rating)
                {
                    EXPECT_LT(l.chips_cy, l.rating_baseline - 18.0f);
                    EXPECT_LT(l.rating_baseline, l.offer_baseline - 20.0f);
                }
                else
                {
                    EXPECT_EQ(l.rating_baseline, 0.0f);
                    EXPECT_LT(l.chips_cy, l.offer_baseline - 20.0f);
                }
                EXPECT_LT(l.offer_baseline, l.why_baseline - 20.0f);
                EXPECT_LT(l.why_baseline, l.buttons_top);
                // The left stack and the panel do not meet.
                EXPECT_LE(l.stack_x + l.stack_w + hero::kPanelGap, l.panel.x + 0.01f);
                EXPECT_NEAR(l.panel.x + l.panel.w, 1920.0f - 100.0f, 0.01f);
                // The panel sits on the buttons' bottom and holds its rows and the divider.
                EXPECT_NEAR(l.panel.y + l.panel.h, l.buttons_top + hero::kButtonH, 0.01f);
                EXPECT_LT(l.divider_y, l.panel.y + l.panel.h - hero::kRowPitch);
                EXPECT_NEAR(l.divider_y,
                            l.panel.y + hero::kPanelPad + rows * hero::kRowPitch + hero::kDividerGap * 0.5f,
                            0.01f);
                // The next section's label peeks under the buttons; the list starts below the screen's
                // usable area, so nothing of it is drawn over the hero.
                EXPECT_GT(l.label_baseline - 26.0f, l.buttons_top + hero::kButtonH);
                EXPECT_LT(l.label_baseline, hero::kScreenH);
                EXPECT_GE(l.list_top, l.label_baseline);
            }
        }
    }
}

TEST(HeroLayout, TheTitleStopsShortOfATallPanelOnly)
{
    const float full = hero::title_width(1920.0f, 2, true);
    EXPECT_NEAR(full, 1920.0f - 2.0f * ui::kGutter - hero::kIconSize - hero::kIconGap, 0.01f);
    EXPECT_LT(hero::title_width(1920.0f, 6, true), full);
    EXPECT_LE(ui::kGutter + hero::kIconSize + hero::kIconGap + hero::title_width(1920.0f, 6, true),
              1920.0f - ui::kGutter - hero::kPanelW);
}

TEST(HeroLayout, WithoutRowsThereIsNoPanelAndTheStackUsesTheWidth)
{
    const hero::Layout none = hero::layout(1920.0f, false, true, false, false, false, 0, false);
    EXPECT_EQ(none.panel.h, 0.0f);
    EXPECT_EQ(none.rating_baseline, 0.0f);
    EXPECT_EQ(none.offer_baseline, 0.0f);
    EXPECT_EQ(none.divider_y, 0.0f);
    const hero::Layout some = hero::layout(1920.0f, false, true, false, false, false, 2, false);
    EXPECT_GT(none.stack_w, some.stack_w);
    EXPECT_GT(some.panel.h, 0.0f);
    EXPECT_EQ(some.divider_y, 0.0f);
}

} // namespace
} // namespace orbit
