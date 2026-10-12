// Wings TV app - Settings editing, Library (APR Emu, delete), Files and Logs on the fake backend.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Destructive flows (delete a game, delete files, APR update) run here only
// against the fake backend; nothing in this file talks to a real Pegasus DL.

#include "fake_backend.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace orbit
{
namespace
{

struct Mgr
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;

    explicit Mgr(bool apr_manifest_ok = true, int start_tab = -1) : backend(host::FakeMode::normal)
    {
        context.app_version = "00.001.000";
        backend.set_apr_manifest_ok(apr_manifest_ok);
        store.poll_once();
        store.load_catalog_once();
        if (start_tab >= 0)
            app.show_tab(start_tab);
        frame();
    }
    void frame(Direction nav = Direction::none, std::uint32_t press = 0)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = press;
        feedback.clear();
        app.update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app.draw(drawn);
        for (const auto &instance : drawn.scene.instances())
            ASSERT_TRUE(std::isfinite(instance.rect[0]) && std::isfinite(instance.rect[1]));
    }
    // Runs what the screens queued and lets the frame loop see the answers; calls
    // may chain (a result opens a panel, a panel starts a call), so go round a few times.
    void work(int rounds = 4)
    {
        for (int r = 0; r < rounds; ++r)
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
    void down(int times = 1)
    {
        for (int i = 0; i < times; ++i)
            frame(Direction::down);
    }
    void right()
    {
        frame(Direction::right);
    }
    // Frames that pass this much time (jobs poll on the clock).
    void seconds(float s)
    {
        for (int i = 0; i < static_cast<int>(s * 60.0f); ++i)
        {
            if (i % 20 == 0)
                work(1);
            else
                frame();
        }
    }
    void refresh_state()
    {
        store.refresh_now();
        store.poll_once();
        work(2);
    }
};

// ---- Settings ----

int settings_row(Mgr &m, const std::string &title)
{
    const auto rows = m.app.settings_screen().row_list();
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].title == title)
            return static_cast<int>(i);
    }
    return -1;
}

void select_setting(Mgr &m, const std::string &title)
{
    const int row = settings_row(m, title);
    ASSERT_GE(row, 0) << title;
    const int at = m.app.settings_screen().index();
    for (int i = 0; i < std::abs(row - at); ++i)
        m.frame(row > at ? Direction::down : Direction::up);
    m.press(Action::confirm);
}

TEST(SettingsEditing, ADebridTokenIsTypedMaskedSentOnceAndNeverShownBack)
{
    Mgr m(true, tab::settings);
    m.app.settings_screen().set_group(SettingsScreen::Group::pegasus);
    EXPECT_EQ(m.store.state().settings.torbox.configured, false);
    select_setting(m, "TorBox");
    EXPECT_TRUE(m.app.settings_screen().modal());
    EXPECT_EQ(m.app.settings_screen().menu_codes().size(), 3u);
    m.press(Action::confirm); // "Set or replace the token"
    ASSERT_TRUE(m.app.settings_screen().prompt_open());
    m.app.settings_screen().type_into_prompt("good-secret-token-123");
    m.down(10);
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(m.backend.last_token(true), "good-secret-token-123");
    EXPECT_EQ(m.backend.last_token(false), ""); // the other service was not touched
    EXPECT_TRUE(m.store.state().settings.torbox.valid);
    EXPECT_TRUE(m.store.state().settings.torbox_ready);
    EXPECT_TRUE(m.app.settings_screen().modal()); // the "ready" notice
    // Nothing the screens show carries the token.
    for (const auto &row : m.app.settings_screen().row_list())
    {
        EXPECT_EQ(row.title.find("good-secret"), std::string::npos);
        EXPECT_EQ(row.detail.find("good-secret"), std::string::npos);
        EXPECT_EQ(row.right.find("good-secret"), std::string::npos);
    }
}

TEST(SettingsEditing, ATokenPegasusRejectsIsReportedAsNotValid)
{
    Mgr m(true, tab::settings);
    m.app.settings_screen().set_group(SettingsScreen::Group::pegasus);
    select_setting(m, "TorBox");
    m.press(Action::confirm);
    m.app.settings_screen().type_into_prompt("nope");
    m.down(10);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.store.state().settings.torbox.configured);
    EXPECT_FALSE(m.store.state().settings.torbox.valid);
    const auto rows = m.app.settings_screen().row_list();
    EXPECT_EQ(rows[static_cast<std::size_t>(settings_row(m, "TorBox"))].right, "Not valid");
}

TEST(SettingsEditing, ClearingATokenAsksFirstAndTheFocusStartsOnKeep)
{
    Mgr m(true, tab::settings);
    m.app.settings_screen().set_group(SettingsScreen::Group::pegasus);
    ASSERT_TRUE(m.store.state().settings.real_debrid.configured);
    select_setting(m, "Real-Debrid");
    m.down(2); // Remove the token
    m.press(Action::confirm);
    EXPECT_TRUE(m.app.settings_screen().modal());
    const int writes = m.backend.settings_writes();
    m.press(Action::confirm); // the default is the safe button
    m.work();
    EXPECT_EQ(m.backend.settings_writes(), writes);
    EXPECT_TRUE(m.store.state().settings.real_debrid.configured);
    // Again, this time choosing Remove.
    select_setting(m, "Real-Debrid");
    m.down(2);
    m.press(Action::confirm);
    m.right();
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(m.backend.settings_writes(), writes + 1);
    EXPECT_FALSE(m.store.state().settings.real_debrid.configured);
    EXPECT_EQ(m.backend.last_token(false), "");
}

TEST(SettingsEditing, ConnectionsAcceptOneToSixtyFourAndRefuseTheRest)
{
    Mgr m(true, tab::settings);
    m.app.settings_screen().set_group(SettingsScreen::Group::pegasus);
    select_setting(m, "Download connections");
    m.down(1); // 4 connections
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(m.store.state().settings.connections, 4);
    // A typed number out of range never reaches Pegasus.
    const int writes = m.backend.settings_writes();
    select_setting(m, "Download connections");
    m.down(8); // "Type a number..."
    m.press(Action::confirm);
    ASSERT_TRUE(m.app.settings_screen().prompt_open());
    m.app.settings_screen().type_into_prompt("9");
    m.app.settings_screen().type_into_prompt("9"); // "499"
    m.down(10);
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(m.backend.settings_writes(), writes);
    EXPECT_EQ(m.store.state().settings.connections, 4);
    EXPECT_TRUE(m.app.settings_screen().modal()); // the "not a valid number" notice
}

TEST(SettingsEditing, TheFolderPickerBrowsesGoesUpAndSavesAWritableFolder)
{
    Mgr m(true, tab::settings);
    m.app.settings_screen().set_group(SettingsScreen::Group::pegasus);
    select_setting(m, "Download folder");
    m.work();
    FolderPicker &picker = m.app.settings_screen().picker();
    ASSERT_TRUE(picker.is_open());
    EXPECT_EQ(picker.path(), "/data/pegasus-dl/downloads");
    EXPECT_EQ(picker.row_label(0), "Use this folder");
    // Make a folder here.
    m.down(1);
    m.press(Action::confirm); // New folder...
    ASSERT_TRUE(picker.prompt_open());
    picker.type_into_prompt("Games");
    m.down(10);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.backend.path_exists("/data/pegasus-dl/downloads/Games"));
    // Up one level, then use it.
    m.press(Action::back);
    m.work();
    EXPECT_EQ(picker.path(), "/data/pegasus-dl");
    m.press(Action::confirm); // Use this folder
    m.work();
    EXPECT_FALSE(picker.is_open());
    EXPECT_EQ(m.store.state().settings.download_root, "/data/pegasus-dl");
}

TEST(SettingsEditing, AReadOnlyLocationCannotBeChosenAsTheDownloadFolder)
{
    Mgr m(true, tab::settings);
    m.app.settings_screen().set_group(SettingsScreen::Group::pegasus);
    FolderPicker &picker = m.app.settings_screen().picker();
    picker.open(m.store, "Download folder", "/system");
    m.work();
    ASSERT_TRUE(picker.is_open());
    EXPECT_EQ(picker.path(), "/system");
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(picker.is_open()); // refused: still there
    EXPECT_NE(m.store.state().settings.download_root, "/system");
}

TEST(SettingsEditing, UpFromADriveWhoseParentCannotBeListedNeverTrapsThePicker)
{
    // B1: the console refuses /mnt. Circle from /mnt/usb0 must still get out.
    Mgr m(true, tab::settings);
    FolderPicker &picker = m.app.settings_screen().picker();
    picker.open(m.store, "Download folder", "/mnt/usb0");
    m.work();
    ASSERT_EQ(picker.path(), "/mnt/usb0");
    m.press(Action::back); // /mnt fails: falls back to the storage locations
    m.work();
    EXPECT_EQ(picker.path(), "");
    EXPECT_TRUE(picker.is_open());
    m.press(Action::back); // and from there Circle closes
    m.work();
    EXPECT_FALSE(picker.is_open());
    // The "Up one level" row does the same.
    picker.open(m.store, "Download folder", "/mnt/usb0");
    m.work();
    m.down(2); // Use this folder, New folder, Up one level
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(picker.path(), "");
}

TEST(SettingsEditing, TheCapabilityPanelShowsWhatPegasusCannotDo)
{
    Mgr m(false, tab::settings); // the APR manifest answers 503
    m.refresh_state();
    bool versions_off = false;
    for (const auto &feature : m.store.state().info.features)
        versions_off = versions_off || (feature.first == "APR Emu versions" && !feature.second);
    EXPECT_TRUE(versions_off);
    select_setting(m, "Capabilities");
    EXPECT_TRUE(m.app.settings_screen().modal());
}

// ---- Library ----

TEST(Library, UpdatingAprEmuConfirmsThenPollsTheJobToTheEnd)
{
    Mgr m(true, tab::library);
    m.refresh_state();
    ASSERT_GE(m.store.state().installed.size(), 3u);
    m.press(Action::confirm); // the menu of the first game
    ASSERT_TRUE(m.app.library_screen().modal());
    EXPECT_EQ(m.app.library_screen().menu_codes().front(), "update");
    m.press(Action::confirm); // Update APR Emu
    m.press(Action::confirm); // confirm
    m.work();
    EXPECT_TRUE(m.app.library_screen().job_active());
    m.seconds(6.0f);
    EXPECT_FALSE(m.app.library_screen().job_active());
    EXPECT_EQ(m.backend.apr_updates(), 1);
    m.refresh_state();
    EXPECT_EQ(m.store.state().installed[0].apr.state, "current");
}

TEST(Library, ADeleteNeedsTheConfirmationAndTheTypedTitleId)
{
    Mgr m(true, tab::library);
    m.refresh_state();
    const std::string target = m.store.state().installed[0].title_id;
    const auto open_delete = [&]
    {
        m.press(Action::confirm);
        const int index = static_cast<int>(
            std::find(m.app.library_screen().menu_codes().begin(),
                      m.app.library_screen().menu_codes().end(), "delete") -
            m.app.library_screen().menu_codes().begin());
        m.down(index);
        m.press(Action::confirm);
    };
    // Declining the first question deletes nothing.
    open_delete();
    m.press(Action::back);
    m.work();
    EXPECT_TRUE(m.backend.deleted_games().empty());
    // Accepting it still asks for the id; a wrong one deletes nothing.
    open_delete();
    m.right();
    m.press(Action::confirm);
    ASSERT_TRUE(m.app.library_screen().prompt_open());
    m.app.library_screen().type_into_prompt("PPSA1");
    m.down(10);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.backend.deleted_games().empty());
    m.press(Action::confirm); // close the notice
    // The right id deletes, and the job is followed to the end.
    open_delete();
    m.right();
    m.press(Action::confirm);
    ASSERT_TRUE(m.app.library_screen().prompt_open());
    m.app.library_screen().type_into_prompt(target);
    m.down(10);
    m.press(Action::confirm);
    m.work();
    m.seconds(6.0f);
    ASSERT_EQ(m.backend.deleted_games().size(), 1u);
    EXPECT_EQ(m.backend.deleted_games()[0], target);
    m.refresh_state();
    for (const InstalledGame &g : m.store.state().installed)
        EXPECT_NE(g.title_id, target);
}

TEST(Library, WhenTheVersionListIsUnavailableUpdatingStillWorks)
{
    Mgr m(false, tab::library); // 503 on /api/apr-emu/versions
    m.refresh_state();
    m.press(Action::confirm);
    ASSERT_TRUE(m.app.library_screen().modal());
    m.down(1); // "Reinstall a specific build": disabled
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.app.library_screen().modal()); // still the menu
    EXPECT_EQ(m.backend.apr_updates(), 0);
    m.press(Action::back);
    // Updating to the latest is a different route and goes ahead.
    m.press(Action::confirm);
    m.press(Action::confirm);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.app.library_screen().job_active());
}

TEST(Library, ReinstallAnExactBuildFromTheList)
{
    Mgr m(true, tab::library);
    m.refresh_state();
    m.press(Action::confirm);
    m.down(1);
    m.press(Action::confirm); // Reinstall...
    m.work();
    ASSERT_TRUE(m.app.library_screen().modal()); // the list of builds
    m.press(Action::confirm);                    // the first build
    m.press(Action::confirm);                    // confirm
    m.work();
    m.seconds(6.0f);
    EXPECT_EQ(m.backend.apr_updates(), 1);
}

TEST(Library, AVersionListThatFailsLateIsExplainedNotSwallowed)
{
    Mgr m(true, tab::library);
    m.refresh_state();
    m.backend.set_apr_manifest_ok(false); // the manifest grew since the probe
    m.press(Action::confirm);
    m.down(1);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.app.library_screen().modal()); // the explanation
    m.press(Action::confirm);
    m.press(Action::confirm); // reopen the menu: the item is now off
    EXPECT_EQ(m.backend.apr_updates(), 0);
}

// ---- Files ---- (files_test.cpp)

// ---- Logs ----

TEST(Logs, LevelFilterAndFollowing)
{
    Mgr m(true, tab::logs);
    m.refresh_state();
    LogsScreen &logs = m.app.logs_screen();
    m.frame();
    EXPECT_GE(logs.shown_count(), 8);
    EXPECT_TRUE(logs.following());
    logs.set_level(LogsScreen::Level::warnings);
    m.frame();
    EXPECT_EQ(logs.shown_count(), 2); // one warn, one error
    logs.set_level(LogsScreen::Level::errors);
    m.frame();
    EXPECT_EQ(logs.shown_count(), 1);
    logs.set_level(LogsScreen::Level::all);
    m.frame();
    m.frame(Direction::up); // moving up pauses following
    EXPECT_FALSE(logs.following());
    m.press(Action::west); // Square toggles it back
    EXPECT_TRUE(logs.following());
}

TEST(Logs, ClearingPegasusLogsAsksFirst)
{
    Mgr m(true, tab::logs);
    m.refresh_state();
    LogsScreen &logs = m.app.logs_screen();
    m.press(Action::confirm); // the options list
    ASSERT_TRUE(logs.modal());
    const auto &codes = logs.menu_codes();
    m.down(static_cast<int>(std::find(codes.begin(), codes.end(), "clear") - codes.begin()));
    m.press(Action::confirm);
    EXPECT_TRUE(logs.modal()); // the confirmation
    m.press(Action::confirm);  // Keep (the safe default)
    m.work();
    EXPECT_EQ(m.backend.logs_cleared(), 0);
    m.press(Action::confirm);
    m.down(static_cast<int>(std::find(codes.begin(), codes.end(), "clear") - codes.begin()));
    m.press(Action::confirm);
    m.right();
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(m.backend.logs_cleared(), 1);
    m.frame();
    EXPECT_EQ(logs.shown_count(), 0);
}

TEST(Logs, WingsDiagnosticsAreAvailableWithoutPegasus)
{
    Mgr m(true, tab::logs);
    LogsScreen &logs = m.app.logs_screen();
    logs.show_diagnostics(true);
    m.frame();
    EXPECT_TRUE(logs.showing_diagnostics());
    m.context.diagnostics_text = "Wings diagnostics\nApp          version 00.001.000\n";
    m.press(Action::north); // Triangle toggles back
    EXPECT_FALSE(logs.showing_diagnostics());
}

} // namespace
} // namespace orbit
