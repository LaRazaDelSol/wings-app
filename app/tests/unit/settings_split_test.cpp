// Wings TV app - Settings split: the Wings group (kept on the console) and the Pegasus DL group.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rules under test: nothing in the Wings group ever calls the Pegasus adapter; every
// Pegasus control maps to exactly one adapter call; what Pegasus does not let us change
// is a read-only row; the link-ranking preferences survive a restart.

#include "fake_backend.hpp"
#include "orbit/prefs.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <cstdlib>

namespace orbit
{
namespace
{

using Group = SettingsScreen::Group;

struct TempDir
{
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/wings-linkprefs-XXXXXX";
        const char *made = mkdtemp(name);
        path = made != nullptr ? made : "/tmp";
    }
    ~TempDir()
    {
        const std::string command = "rm -rf '" + path + "'";
        if (std::system(command.c_str()) != 0)
        {
        }
    }
};

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

    explicit Rig(host::FakeMode mode = host::FakeMode::normal,
                 std::vector<std::string> prefs_paths = {})
        : backend(mode)
    {
        context.app_version = "00.001.000";
        context.wings_prefs_paths = std::move(prefs_paths);
        store.poll_once();
        app = std::make_unique<App>(context);
        app->show_tab(tab::settings);
        frame();
    }
    SettingsScreen &screen()
    {
        return app->settings_screen();
    }
    void frame(Direction nav = Direction::none, std::uint32_t press = 0)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = press;
        feedback.clear();
        app->update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app->draw(drawn);
    }
    void work()
    {
        for (int r = 0; r < 4; ++r)
        {
            while (store.run_next_action())
            {
            }
            frame();
            frame();
        }
    }
    void press(Action action)
    {
        frame(Direction::none, hui::action_bit(action));
    }
    int row_of(const std::string &title)
    {
        const auto rows = screen().row_list();
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].title == title)
                return static_cast<int>(i);
        }
        return -1;
    }
    // Moves to the row and presses Cross.
    void select(const std::string &title)
    {
        const int row = row_of(title);
        ASSERT_GE(row, 0) << title;
        while (screen().index() < row)
            frame(Direction::down);
        while (screen().index() > row)
            frame(Direction::up);
        press(Action::confirm);
    }
    // In an open list dialog: item `item`, then Cross.
    void pick(int item)
    {
        for (int i = 0; i < item; ++i)
            frame(Direction::down);
        press(Action::confirm);
    }
    std::string right_of(const std::string &title)
    {
        const int row = row_of(title);
        return row < 0 ? std::string("<missing>") : screen().row_list()[row].right;
    }
};

TEST(SettingsSplit, WingsIsTheFirstGroupAndTheGroupsShareNoRows)
{
    Rig r;
    EXPECT_EQ(r.screen().group(), Group::wings);
    EXPECT_EQ(r.screen().row_list().front().title, "Settings group");
    EXPECT_GE(r.row_of("Preferred firmware"), 0);
    EXPECT_GE(r.row_of("Reset Store grid"), 0);
    EXPECT_GE(r.row_of("Popularity data"), 0);
    EXPECT_GE(r.row_of("Backend adapter"), 0);
    EXPECT_EQ(r.row_of("Download folder"), -1);
    EXPECT_EQ(r.row_of("TorBox"), -1);
    r.screen().set_group(Group::pegasus);
    EXPECT_GE(r.row_of("Download folder"), 0);
    EXPECT_GE(r.row_of("Download connections"), 0);
    EXPECT_GE(r.row_of("Real-Debrid"), 0);
    EXPECT_GE(r.row_of("DNS helper"), 0);
    EXPECT_GE(r.row_of("Provider resolvers"), 0);
    EXPECT_GE(r.row_of("Download only"), 0);
    EXPECT_EQ(r.row_of("Preferred firmware"), -1);
    EXPECT_EQ(r.row_of("Reset Store grid"), -1);
}

TEST(SettingsSplit, LeftAndRightOnTheHeaderRowSwitchTheGroupOthersRowsDoNot)
{
    Rig r;
    ASSERT_EQ(r.screen().index(), 0);
    r.frame(Direction::right);
    EXPECT_EQ(r.screen().group(), Group::pegasus);
    r.frame(Direction::left);
    EXPECT_EQ(r.screen().group(), Group::wings);
    r.frame(Direction::down);
    r.frame(Direction::right); // a row below the header: no switch
    EXPECT_EQ(r.screen().group(), Group::wings);
    r.frame(Direction::up);
    r.press(Action::confirm); // Cross on the header also switches
    EXPECT_EQ(r.screen().group(), Group::pegasus);
}

TEST(SettingsSplit, EveryPegasusControlIsLabelledAndTheRuntimeBlockIsReadOnly)
{
    Rig r;
    r.screen().set_group(Group::pegasus);
    // The section header and the group row say it: no per-row chip repeats the section.
    for (const char *title : {"Download folder", "Download connections", "Real-Debrid", "TorBox", "DNS helper"})
        EXPECT_TRUE(r.screen().row_list()[r.row_of(title)].badge.empty()) << title;
    // Read-only rows: no control (outlined), under the "Runtime (read-only)" header, Select does nothing.
    const int writes = r.backend.settings_writes();
    const int reads = r.backend.settings_reads();
    for (const char *title : {"Pegasus version", "Download only", "Native .pkg handoff", "Range resume",
                              "Queue survives restarts"})
    {
        const int row = r.row_of(title);
        ASSERT_GE(row, 0) << title;
        EXPECT_TRUE(r.screen().row_list()[row].badge.empty()) << title;
        EXPECT_EQ(r.screen().row_list()[row].section, "Runtime (read-only)") << title;
        EXPECT_TRUE(r.screen().row_list()[row].info) << title;
        r.select(title);
        EXPECT_FALSE(r.screen().modal()) << title;
    }
    r.work();
    EXPECT_EQ(r.backend.settings_writes(), writes);
    EXPECT_EQ(r.backend.settings_reads(), reads);
    EXPECT_EQ(r.right_of("Range resume"), "Yes");
}

TEST(SettingsSplit, ProviderResolversAreAReadOnlyList)
{
    Rig r;
    r.screen().set_group(Group::pegasus);
    const auto rows = r.screen().row_list();
    EXPECT_EQ(rows[r.row_of("Provider resolvers")].right, "3 providers");
    EXPECT_NE(rows[r.row_of("Provider resolvers")].detail.find("BuzzHeavier"), std::string::npos);
    const int writes = r.backend.settings_writes();
    r.select("Provider resolvers");
    ASSERT_TRUE(r.screen().modal());
    r.press(Action::confirm); // choosing a provider changes nothing
    r.work();
    EXPECT_EQ(r.backend.settings_writes(), writes);
}

TEST(SettingsSplit, EachPegasusControlMakesExactlyOneAdapterCall)
{
    Rig r;
    r.screen().set_group(Group::pegasus);
    int writes = r.backend.settings_writes();
    int reads = r.backend.settings_reads();

    // Connections: one settings_update.
    r.select("Download connections");
    ASSERT_TRUE(r.screen().modal());
    r.pick(2); // 8
    r.work();
    EXPECT_EQ(r.backend.settings_writes(), writes + 1);
    EXPECT_EQ(r.backend.settings_reads(), reads);
    EXPECT_EQ(r.store.state().settings.connections, 8);
    ++writes;

    // DNS helper: one settings read, never a write.
    r.select("DNS helper");
    r.work();
    EXPECT_EQ(r.backend.settings_reads(), reads + 1);
    EXPECT_EQ(r.backend.settings_writes(), writes);
    ++reads;
    EXPECT_EQ(r.app->toast(), "DNS helper is running");

    // Token menu "Check the token again" is a debrid refresh, not a settings write.
    r.select("Real-Debrid");
    ASSERT_TRUE(r.screen().modal());
    r.pick(1); // refresh
    r.work();
    EXPECT_EQ(r.backend.settings_writes(), writes);
}

TEST(SettingsSplit, TheWingsGroupNeverCallsPegasusAndWorksOffline)
{
    Rig r(host::FakeMode::offline);
    ASSERT_FALSE(r.store.state().online());
    const int writes = r.backend.settings_writes();
    const int reads = r.backend.settings_reads();
    // Link ranking choices.
    r.select("Preferred firmware");
    ASSERT_TRUE(r.screen().modal());
    r.pick(7); // Any, 4.xx .. : item 7 is 10.xx
    EXPECT_EQ(r.screen().local_prefs().firmware, "10.xx");
    r.select("Preferred region");
    r.pick(2); // EUR
    EXPECT_EQ(r.screen().local_prefs().region, "EUR");
    r.select("Preferred format");
    r.pick(1); // exFAT
    EXPECT_EQ(r.screen().local_prefs().format, "exFAT");
    EXPECT_TRUE(r.screen().local_prefs().prefer_debrid); // default on
    r.select("Prefer debrid links");
    EXPECT_FALSE(r.screen().local_prefs().prefer_debrid);
    EXPECT_EQ(r.right_of("Prefer debrid links"), "Off");
    // Popularity: on by default, remembered when turned off and on again.
    EXPECT_TRUE(r.screen().local_prefs().popularity_enabled);
    EXPECT_EQ(r.right_of("Popularity data"), "On");
    r.select("Popularity data");
    EXPECT_FALSE(r.screen().local_prefs().popularity_enabled);
    r.select("Popularity data");
    EXPECT_TRUE(r.screen().local_prefs().popularity_enabled);
    const int refresh = r.row_of("Refresh popularity data");
    ASSERT_GE(refresh, 0);
    EXPECT_FALSE(r.screen().row_list()[refresh].disabled); // on: the refresh row works
    EXPECT_EQ(r.screen().row_list()[refresh].right, "Refresh");
    r.select("Refresh popularity data"); // no service in tests: a toast, never a popup
    EXPECT_FALSE(r.screen().modal());
    orbit::popularity_hub().set_enabled(false);
    // The popularity address is typed, must be https, and is only stored.
    r.select("Popularity source URL");
    ASSERT_TRUE(r.screen().prompt_open());
    r.screen().type_into_prompt("https://example.test/pop.json");
    for (int i = 0; i < 10; ++i)
        r.frame(Direction::down);
    r.press(Action::confirm);
    r.work();
    EXPECT_EQ(r.screen().local_prefs().popularity_url, "https://example.test/pop.json");
    // About rows stay readable offline.
    EXPECT_EQ(r.right_of("Pegasus DL"), "Not connected");
    // Nothing above reached the adapter.
    EXPECT_EQ(r.backend.settings_writes(), writes);
    EXPECT_EQ(r.backend.settings_reads(), reads);
}

TEST(SettingsSplit, ResetStoreGridAsksThenResetsSortAndFiltersButKeepsFavourites)
{
    Rig r;
    CatalogueState state;
    state.sort = SortMode::za;
    state.favourites_only = true;
    state.favourites = {"PPSA00001"};
    r.app->store_screen().set_catalogue_state(state);
    r.select("Reset Store grid");
    ASSERT_TRUE(r.screen().modal());
    EXPECT_EQ(r.app->store_screen().catalogue_state().sort, SortMode::za); // not yet
    r.press(Action::back);
    EXPECT_EQ(r.app->store_screen().catalogue_state().sort, SortMode::za);
    r.select("Reset Store grid");
    r.press(Action::confirm);
    const CatalogueState after = r.app->store_screen().catalogue_state();
    EXPECT_EQ(after.sort, SortMode::az);
    EXPECT_FALSE(after.favourites_only);
    EXPECT_EQ(after.favourites.size(), 1u);
}

TEST(SettingsSplit, PrefsDefaultsAreTheDocumentedOnes)
{
    const WingsLocalPrefs defaults;
    EXPECT_TRUE(defaults.firmware.empty());
    EXPECT_TRUE(defaults.region.empty());
    EXPECT_TRUE(defaults.format.empty());
    EXPECT_TRUE(defaults.prefer_debrid);
    EXPECT_TRUE(defaults.popularity_enabled);
    EXPECT_TRUE(defaults.popularity_url.empty());
}

TEST(SettingsSplit, PrefsSurviveARestartThroughTheSaveContainer)
{
    TempDir dir;
    const std::vector<std::string> paths = {dir.path + "/linkprefs.v1"};
    WingsLocalPrefs loaded;
    EXPECT_FALSE(load_local_prefs(paths, &loaded));
    EXPECT_EQ(loaded, WingsLocalPrefs{});

    WingsLocalPrefs saved;
    saved.firmware = "9.xx";
    saved.region = "JPN";
    saved.format = "FPKG";
    saved.prefer_debrid = false;
    saved.popularity_enabled = true;
    saved.popularity_url = "https://example.test/a?b=c";
    EXPECT_EQ(save_local_prefs(paths, saved), 1);
    WingsLocalPrefs back;
    ASSERT_TRUE(load_local_prefs(paths, &back));
    EXPECT_EQ(back, saved);

    // A second screen on the same file starts from it; its changes reach the file.
    {
        Rig r(host::FakeMode::normal, paths);
        EXPECT_EQ(r.screen().local_prefs(), saved);
        r.select("Preferred region");
        r.pick(0); // Any
    }
    ASSERT_TRUE(load_local_prefs(paths, &back));
    EXPECT_TRUE(back.region.empty());
    EXPECT_EQ(back.firmware, "9.xx");
    EXPECT_FALSE(back.prefer_debrid);
}

TEST(SettingsSplit, PrefsIgnoreInvalidChoicesAndDamagedFiles)
{
    WingsLocalPrefs out;
    ASSERT_TRUE(decode_local_prefs("firmware=99.xx\nregion=MARS\nformat=zip\nprefer_debrid=0\nfoo=bar\n", &out));
    EXPECT_TRUE(out.firmware.empty());
    EXPECT_TRUE(out.region.empty());
    EXPECT_TRUE(out.format.empty());
    EXPECT_FALSE(out.prefer_debrid);
    EXPECT_FALSE(decode_local_prefs("", &out));

    TempDir dir;
    const std::string bad = dir.path + "/bad.v1";
    const std::string good = dir.path + "/good.v1";
    std::FILE *file = std::fopen(bad.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    std::fputs("HUIL garbage that fails the checksum", file);
    std::fclose(file);
    WingsLocalPrefs one;
    one.format = "exFAT";
    ASSERT_EQ(save_local_prefs({good}, one), 1);
    WingsLocalPrefs loaded;
    ASSERT_TRUE(load_local_prefs({bad, good}, &loaded));
    EXPECT_EQ(loaded.format, "exFAT");
    EXPECT_EQ(save_local_prefs({"/proc/definitely/not/here/p.v1", dir.path + "/ok.v1"}, one), 1);
}

TEST(SettingsSplit, TileDetailsRowCyclesAndIsRemembered)
{
    TempDir dir;
    const std::vector<std::string> paths = {dir.path + "/linkprefs.v1"};
    {
        Rig r(host::FakeMode::offline, paths);
        const int writes = r.backend.settings_writes();
        EXPECT_EQ(r.right_of("Tile details"), "On focus"); // default
        r.select("Tile details");
        EXPECT_EQ(r.screen().local_prefs().tile_details, TileDetails::never);
        EXPECT_EQ(r.right_of("Tile details"), "Never");
        r.select("Tile details");
        EXPECT_EQ(r.right_of("Tile details"), "Always");
        EXPECT_EQ(r.backend.settings_writes(), writes); // Wings only
    }
    WingsLocalPrefs back;
    ASSERT_TRUE(load_local_prefs(paths, &back));
    EXPECT_EQ(back.tile_details, TileDetails::always);
    Rig again(host::FakeMode::offline, paths);
    EXPECT_EQ(again.right_of("Tile details"), "Always");
}

} // namespace
} // namespace orbit
