// Wings TV app - tests for the grouped filter menus: headers, summary text, result count, focus.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/filter_menu.hpp"
#include "orbit/popularity.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

PopularityView &pop_view_on()
{
    static PopularityIndex index;
    static PopularityView view{&index, nullptr};
    return view;
}

struct R
{
    std::string section;
};

std::vector<R> rows_of(std::initializer_list<const char *> sections)
{
    std::vector<R> out;
    for (const char *s : sections)
        out.push_back({s});
    return out;
}

TEST(FilterMenu, HeadersAreLayoutOnlyAndPushLaterRowsDown)
{
    const auto rows = rows_of({"Show", "Show", "Region", "Region", "Source", ""});
    EXPECT_TRUE(theme::section_starts(rows, 0));
    EXPECT_FALSE(theme::section_starts(rows, 1));
    EXPECT_TRUE(theme::section_starts(rows, 2));
    EXPECT_TRUE(theme::section_starts(rows, 4));
    EXPECT_FALSE(theme::section_starts(rows, 5)); // the Clear row has no header
    // Row i sits i pitches down plus one header for each group opened so far: a header is
    // never a row, so stepping i -> i + 1 (the D-pad) cannot land on one.
    EXPECT_FLOAT_EQ(menu_row_top(rows, 0), kMenuHeader);
    EXPECT_FLOAT_EQ(menu_row_top(rows, 1), kMenuPitch + kMenuHeader);
    EXPECT_FLOAT_EQ(menu_row_top(rows, 2), 2 * kMenuPitch + 2 * kMenuHeader);
    EXPECT_FLOAT_EQ(menu_row_top(rows, 5), 5 * kMenuPitch + 3 * kMenuHeader);
    EXPECT_FLOAT_EQ(menu_rows_height(rows), 5 * kMenuPitch + 3 * kMenuHeader + kMenuRow);
    EXPECT_FLOAT_EQ(menu_rows_height(std::vector<R>{}), 0.0f);
}

TEST(FilterMenu, SummaryAndCountText)
{
    EXPECT_EQ(filters_on_text(0), "No filters on");
    EXPECT_EQ(filters_on_text(1), "1 filter on");
    EXPECT_EQ(filters_on_text(3), "3 filters on");
    EXPECT_EQ(result_count_text(12, 340, false), "12 of 340 games");
    EXPECT_EQ(result_count_text(7, 9, true), "7 of 9 downloads");
}

TEST(FilterMenu, CountsMatchFiltered)
{
    CatalogueState s;
    EXPECT_EQ(grid_filters_on(s), 0);
    EXPECT_FALSE(s.filtered());
    s.region = "USA";
    s.runs_on_my_fw = true;
    s.installed = InstalledFilter::installed;
    EXPECT_EQ(grid_filters_on(s), 3);
    EXPECT_TRUE(s.filtered());
    s.sort = SortMode::installed_first; // the sort is not a filter
    EXPECT_EQ(grid_filters_on(s), 3);
    s.reset_filters();
    EXPECT_EQ(grid_filters_on(s), 0);

    MirrorFilter f;
    EXPECT_EQ(link_filters_on(f), 0);
    f.host = "Akirabox";
    f.section = 1;
    f.single_file = true;
    EXPECT_EQ(link_filters_on(f), 3);
    EXPECT_EQ(link_filters_on(f) > 0, f.active());
    EXPECT_FALSE(link_row_changed(LinkFilterRow::region, f, MirrorSort::best));
    EXPECT_TRUE(link_row_changed(LinkFilterRow::host, f, MirrorSort::best));
    EXPECT_TRUE(link_row_changed(LinkFilterRow::sort, f, MirrorSort::host));
    EXPECT_FALSE(link_row_changed(LinkFilterRow::reset, f, MirrorSort::host));
}

TEST(FilterMenu, FocusStartsOnTheFirstChangedRowElseTheFirst)
{
    EXPECT_EQ(first_focus_index({false, false, false}), 0);
    EXPECT_EQ(first_focus_index({false, true, true}), 1);
    EXPECT_EQ(first_focus_index({}), 0);
}

TEST(FilterMenu, EveryLinkRowHasAGroupAndTheGroupsAreContiguousInPanelOrder)
{
    Facets facets;
    facets.total = 10;
    facets.regions = {{"USA", 5}, {"EUR", 5}};
    facets.formats = {{"exFAT", 5}, {"FPKG", 5}};
    facets.hosts = {{"A", 5}, {"B", 5}};
    facets.sources = {{"S1", 5}, {"S2", 5}};
    facets.sections = {{"0", 5}, {"1", 5}};
    facets.no_browser = 3;
    facets.single_file = 3;
    LinkPrefs prefs;
    const auto rows = link_filter_rows(facets, MirrorFilter{}, prefs);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back(), LinkFilterRow::reset);
    std::vector<FilterGroup> seen;
    for (const LinkFilterRow row : rows)
    {
        const FilterGroup g = link_filter_group(row);
        if (row == LinkFilterRow::reset)
        {
            EXPECT_EQ(g, FilterGroup::none);
            continue;
        }
        EXPECT_STRNE(filter_group_name(g), "");
        EXPECT_NE(filter_group_accent(g), theme::accent_none);
        // A group never comes back after another one started: one header per group.
        if (seen.empty() || seen.back() != g)
        {
            for (const FilterGroup earlier : seen)
                EXPECT_NE(earlier, g);
            seen.push_back(g);
        }
    }
}

TEST(SortMenu, GroupsFollowTheUsersOrderAndKeepTheSavedNumbers)
{
    // Every mode, as available_sorts() offers them with popularity data on and downloads known.
    const std::vector<SortMode> all = {SortMode::az,
                                       SortMode::za,
                                       SortMode::installed_first,
                                       SortMode::not_installed_first,
                                       SortMode::favourites_first,
                                       SortMode::tier_first,
                                       SortMode::source,
                                       SortMode::popular,
                                       SortMode::rated,
                                       SortMode::reviewed,
                                       SortMode::my_downloads};
    const std::vector<SortMode> menu = sort_menu_order(all);
    ASSERT_EQ(menu.size(), all.size());
    // A to Z, Z to A, then the popularity orders directly, then library state, then link / source.
    EXPECT_EQ(menu[0], SortMode::az);
    EXPECT_EQ(menu[1], SortMode::za);
    EXPECT_EQ(menu[2], SortMode::popular);
    EXPECT_EQ(menu[3], SortMode::rated);
    EXPECT_EQ(menu[4], SortMode::reviewed);
    EXPECT_EQ(menu[5], SortMode::my_downloads);
    EXPECT_EQ(menu[6], SortMode::installed_first);
    EXPECT_EQ(menu[7], SortMode::not_installed_first);
    EXPECT_EQ(menu[8], SortMode::favourites_first);
    EXPECT_EQ(menu[9], SortMode::tier_first);
    EXPECT_EQ(menu[10], SortMode::source);
    // Presentation only: the persisted numbers are untouched.
    EXPECT_EQ(static_cast<int>(SortMode::az), 0);
    EXPECT_EQ(static_cast<int>(SortMode::popular), 7);
    EXPECT_EQ(static_cast<int>(SortMode::my_downloads), 10);
}

TEST(SortMenu, HeadersOpenOncePerGroupAndNeverAreRows)
{
    const std::vector<SortMode> menu = sort_menu_order(available_sorts(&pop_view_on()));
    std::vector<R> rows;
    for (const SortMode mode : menu)
        rows.push_back({sort_group_name(sort_group(mode))});
    ASSERT_EQ(rows.size(), menu.size());
    // Four headers (Order, Popularity, Library state, Link quality & source), each opening once.
    int headers = 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
        headers += theme::section_starts(rows, i) ? 1 : 0;
    EXPECT_EQ(headers, 4);
    // Row i is i pitches plus the headers opened so far: the D-pad's i -> i + 1 skips a header.
    for (std::size_t i = 1; i < rows.size(); ++i)
    {
        const float gap = menu_row_top(rows, i) - menu_row_top(rows, i - 1);
        EXPECT_FLOAT_EQ(gap, kMenuPitch + (theme::section_starts(rows, i) ? kMenuHeader : 0.0f));
    }
    // Every group has its own accent, all in the Store family.
    EXPECT_NE(sort_group_accent(SortGroup::order), sort_group_accent(SortGroup::popularity));
    EXPECT_NE(sort_group_accent(SortGroup::library), sort_group_accent(SortGroup::link));
}

TEST(SortMenu, PopularityGroupIsAbsentWhenThatDataIsOff)
{
    const std::vector<SortMode> menu = sort_menu_order(available_sorts(nullptr));
    ASSERT_EQ(menu.size(), static_cast<std::size_t>(kBasicSortCount));
    for (const SortMode mode : menu)
        EXPECT_NE(sort_group(mode), SortGroup::popularity) << sort_label(mode);
    EXPECT_EQ(menu[0], SortMode::az);
    EXPECT_EQ(menu[1], SortMode::za);
    EXPECT_EQ(menu[2], SortMode::installed_first); // library state follows Order directly
}

} // namespace
} // namespace orbit
