// Wings TV app - Settings sections: named headers between the rows and the accent wash.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headers are layout only: they never take a row index (so D-pad navigation, acts and
// the existing tests are untouched); each section has one accent; the screen's wash eases
// to the focused row's accent.

#include "fake_backend.hpp"
#include "orbit/screens.hpp"
#include "orbit/theme.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <set>

namespace orbit
{
namespace
{

using Group = SettingsScreen::Group;

struct Rig
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    std::unique_ptr<App> app;
    Feedback feedback;

    Rig()
    {
        context.app_version = "00.001.000";
        store.poll_once();
        app = std::make_unique<App>(context);
        app->show_tab(tab::settings);
        frame(Direction::none, 6); // the tab change settles
    }
    SettingsScreen &screen()
    {
        return app->settings_screen();
    }
    void frame(Direction nav = Direction::none, int times = 1)
    {
        for (int i = 0; i < times; ++i)
        {
            InputFrame input;
            input.connected = true;
            input.nav = i == 0 ? nav : Direction::none;
            feedback.clear();
            app->update(input, 1.0f / 60.0f, feedback);
            Frame drawn;
            app->draw(drawn);
        }
    }
};

// Header names in order, one per run of rows that share a section.
std::vector<std::string> headers_of(const std::vector<ListScreen::Row> &rows)
{
    std::vector<std::string> names;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (theme::section_starts(rows, i))
            names.push_back(rows[i].section);
    }
    return names;
}

TEST(SettingsSections, WingsRowsFallIntoNamedSectionsAndTheGroupRowHasNone)
{
    Rig r;
    const auto rows = r.screen().row_list();
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front().title, "Settings group");
    EXPECT_TRUE(rows.front().section.empty()); // the switch row sits above every header
    for (std::size_t i = 1; i < rows.size(); ++i)
        EXPECT_FALSE(rows[i].section.empty()) << rows[i].title;
    const auto names = headers_of(rows);
    EXPECT_GE(names.size(), 4u);
    EXPECT_EQ(std::set<std::string>(names.begin(), names.end()).size(), names.size()); // each once
}

TEST(SettingsSections, PegasusRowsFallIntoNamedSections)
{
    Rig r;
    r.screen().set_group(Group::pegasus);
    const auto rows = r.screen().row_list();
    const auto names = headers_of(rows);
    ASSERT_GE(names.size(), 3u);
    EXPECT_EQ(rows.back().section, "Runtime (read-only)");
    for (std::size_t i = 1; i < rows.size(); ++i)
        EXPECT_GE(rows[i].accent, 0) << rows[i].title;
}

TEST(SettingsSections, HeadersAreNotRowsSoNavigationVisitsEveryRowOnce)
{
    Rig r;
    const int count = static_cast<int>(r.screen().row_list().size());
    int visited = 1; // index 0
    for (int i = 0; i < count + 5; ++i)
    {
        const int before = r.screen().index();
        r.frame(Direction::down);
        const int after = r.screen().index();
        EXPECT_LE(after - before, 1); // never skips a row, never lands on a header
        if (after != before)
            ++visited;
    }
    EXPECT_EQ(visited, count);
    EXPECT_EQ(r.screen().index(), count - 1);
}

TEST(SettingsSections, HeaderOffsetsAddOneHeaderHeightPerSectionStart)
{
    std::vector<ListScreen::Row> rows(5);
    rows[0].title = "switch";
    for (int i = 1; i < 3; ++i)
        rows[static_cast<std::size_t>(i)].section = "A";
    for (int i = 3; i < 5; ++i)
        rows[static_cast<std::size_t>(i)].section = "B";
    EXPECT_FALSE(theme::section_starts(rows, 0));
    EXPECT_TRUE(theme::section_starts(rows, 1));
    EXPECT_FALSE(theme::section_starts(rows, 2));
    EXPECT_TRUE(theme::section_starts(rows, 3));
    EXPECT_FLOAT_EQ(theme::section_offset(rows, 0), 0.0f);
    EXPECT_FLOAT_EQ(theme::section_offset(rows, 2), theme::kHeaderHeight);
    EXPECT_FLOAT_EQ(theme::section_offset(rows, 4), 2.0f * theme::kHeaderHeight);
}

TEST(SettingsSections, FocusedRowsAccentFollowsItsSectionAndWashEasesThere)
{
    Rig r;
    EXPECT_EQ(r.screen().focus_accent(), theme::accent_group_wings);
    r.frame(Direction::down);
    const int first = r.screen().focus_accent();
    EXPECT_NE(first, theme::accent_group_wings);
    const auto rows = r.screen().row_list();
    EXPECT_EQ(first, rows[1].accent);
    // Walk to a row of another section: the accent changes with it.
    int other = -1;
    for (std::size_t i = 2; i < rows.size(); ++i)
    {
        if (rows[i].accent != rows[1].accent)
        {
            other = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GT(other, 0);
    while (r.screen().index() < other)
        r.frame(Direction::down);
    EXPECT_EQ(r.screen().focus_accent(), rows[static_cast<std::size_t>(other)].accent);
    // The App owns the wash; the screen only reports the section (section_accent). A wash
    // aimed at that accent arrives after the ease.
    EXPECT_EQ(r.screen().section_accent(), r.screen().focus_accent());
    theme::Wash wash;
    wash.retarget(theme::place_settings, theme::accent_group_wings);
    wash.retarget(theme::place_settings, r.screen().section_accent());
    for (int i = 0; i < 60; ++i)
        wash.update(1.0f / 60.0f);
    EXPECT_EQ(wash.accent, r.screen().focus_accent());
    const ui::Color want = theme::accent_color(wash.accent);
    const ui::Color got = wash.color();
    EXPECT_NEAR(got.r, want.r, 0.02f);
    EXPECT_NEAR(got.g, want.g, 0.02f);
    EXPECT_NEAR(got.b, want.b, 0.02f);
}

TEST(SettingsSections, SwitchingGroupReportsTheOtherGroupsAccent)
{
    Rig r;
    r.frame(Direction::none, 5);
    EXPECT_EQ(r.screen().section_accent(), theme::accent_group_wings);
    r.frame(Direction::right);
    EXPECT_EQ(r.screen().group(), Group::pegasus);
    r.frame(Direction::none, 2);
    EXPECT_EQ(r.screen().section_accent(), theme::accent_group_pegasus);
}

TEST(SettingsSections, EveryAccentIsAQuietLowAlphaWash)
{
    for (int a = 0; a < theme::accent_count; ++a)
    {
        const ui::Color c = theme::accent_color(a);
        EXPECT_LT(c.r, 0.55f) << a; // the cool family 190 to 295 degrees: no red-heavy colour
        EXPECT_GT(c.b, c.r) << a;
    }
    EXPECT_LE(theme::kWashTop, theme::kWashMax);
    EXPECT_LT(theme::kWashBottom, theme::kWashTop);
}

} // namespace
} // namespace orbit
