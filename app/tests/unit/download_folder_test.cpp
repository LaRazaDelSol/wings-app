// Wings TV app - tests of the default download folder and the first-download gate (B-13).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/download_folder.hpp"
#include "orbit/store.hpp"

#include <gtest/gtest.h>

#include <string>

namespace orbit
{
namespace
{

struct Rig
{
    host::FakeBackend backend;
    Store store{backend};

    void pump()
    {
        for (int i = 0; i < 4; ++i)
        {
            while (store.run_next_action())
            {
            }
            store.tick();
        }
    }
    // Reads the settings the way the poll does, then lets the frame see them.
    void learn_settings()
    {
        store.poll_once();
        pump();
    }
};

const char *const fresh = "/data/homebrew/wings-downloads"; // parent exists, folder does not

TEST(DownloadFolderNeed, DecidesFromSettings)
{
    EXPECT_TRUE(folder_need(true, "", false).set_default);
    EXPECT_FALSE(folder_need(true, "/x", false).set_default);
    EXPECT_FALSE(folder_need(true, "", true).set_default);
    const FolderNeed unknown = folder_need(false, "", false);
    EXPECT_FALSE(unknown.set_default);
    EXPECT_TRUE(unknown.root_unknown);
}

TEST(DownloadFolderDefault, EmptyRootGetsTheDefaultOnce)
{
    Rig r;
    r.backend.set_download_root("");
    r.learn_settings();
    ASSERT_TRUE(r.store.state().have_settings);
    ASSERT_TRUE(r.store.state().settings.download_root.empty());

    r.store.ensure_default_download_folder(fresh);
    r.store.ensure_default_download_folder(fresh); // same frame: no second command
    r.pump();
    r.store.ensure_default_download_folder(fresh); // later frames: still nothing
    r.pump();

    ASSERT_EQ(r.backend.mkdir_calls().size(), 1u);
    EXPECT_EQ(r.backend.mkdir_calls()[0], fresh);
    EXPECT_TRUE(r.backend.path_exists(fresh));
    EXPECT_EQ(r.backend.settings_writes(), 1);
    EXPECT_EQ(r.backend.last_root_written(), fresh);
    EXPECT_EQ(r.store.state().settings.download_root, fresh);
    EXPECT_TRUE(r.store.default_folder_status().empty());
}

TEST(DownloadFolderDefault, ConstantDefaultAlreadyExistingIsAccepted)
{
    Rig r;
    r.backend.set_download_root("");
    r.learn_settings();
    r.store.ensure_default_download_folder(); // fake already has this folder: mkdir says 409
    r.pump();
    ASSERT_EQ(r.backend.mkdir_calls().size(), 1u);
    EXPECT_EQ(r.backend.mkdir_calls()[0], download_folder::default_path);
    EXPECT_EQ(r.store.state().settings.download_root, download_folder::default_path);
}

TEST(DownloadFolderDefault, NonEmptyRootIsNeverTouched)
{
    Rig r;
    r.backend.set_download_root("/mnt/usb0/Backups");
    r.learn_settings();
    r.store.ensure_default_download_folder(fresh);
    r.pump();
    EXPECT_TRUE(r.backend.mkdir_calls().empty());
    EXPECT_EQ(r.backend.settings_writes(), 0);
    EXPECT_EQ(r.store.state().settings.download_root, "/mnt/usb0/Backups");
}

TEST(DownloadFolderDefault, UnknownSettingsDoNothing)
{
    Rig r;
    r.store.ensure_default_download_folder(fresh);
    r.pump();
    EXPECT_TRUE(r.backend.mkdir_calls().empty());
    EXPECT_FALSE(r.store.default_folder_tried());
}

TEST(DownloadFolderDefault, MkdirFailureSetsNoRootAndDoesNotRepeat)
{
    Rig r;
    r.backend.set_download_root("");
    r.backend.set_mkdir_fails(true);
    r.learn_settings();
    r.store.ensure_default_download_folder(fresh);
    r.pump();
    r.store.ensure_default_download_folder(fresh);
    r.pump();
    EXPECT_EQ(r.backend.mkdir_calls().size(), 1u);
    EXPECT_EQ(r.backend.settings_writes(), 0);
    EXPECT_FALSE(r.store.default_folder_status().empty());
    EXPECT_TRUE(r.store.state().settings.download_root.empty());
    EXPECT_FALSE(r.store.link_action().busy); // no popup / busy state
}

TEST(FirstDownloadGate, AsksOnceThenNever)
{
    FirstDownloadGate gate;
    EXPECT_EQ(gate.check(false, true), FirstDownloadGate::Verdict::ask);
    EXPECT_EQ(gate.check(false, true), FirstDownloadGate::Verdict::ask); // dismissed: ask again
    gate.answered();
    EXPECT_EQ(gate.check(false, true), FirstDownloadGate::Verdict::proceed);
}

TEST(FirstDownloadGate, ConfirmedFlagOrUnknownSettingsProceed)
{
    FirstDownloadGate gate;
    EXPECT_EQ(gate.check(true, true), FirstDownloadGate::Verdict::proceed);
    EXPECT_EQ(gate.check(false, false), FirstDownloadGate::Verdict::proceed);
    EXPECT_FALSE(needs_first_confirm(false, false));
    EXPECT_TRUE(needs_first_confirm(false, true));
}

TEST(FirstDownloadGate, DeferredDownloadResumes)
{
    FirstDownloadGate gate;
    int started = 0;
    gate.defer([&started] { ++started; });
    auto resume = gate.take_pending();
    ASSERT_TRUE(static_cast<bool>(resume));
    resume();
    EXPECT_EQ(started, 1);
    EXPECT_FALSE(static_cast<bool>(gate.take_pending()));
}

} // namespace
} // namespace orbit
