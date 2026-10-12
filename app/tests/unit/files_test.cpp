// Wings TV app - The Files tab: navigation (B1), listings (B2), operations and
// cancel (B3), "Where to?", and the rules under them.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything runs against the fake backend (host/fake_backend.*), whose /mnt
// cannot be listed, like the real console's, and whose /mnt/ext0/homebrew is
// the live capture (recon/samples/files/list_ext0_homebrew.json). One test
// replays the real Pegasus samples through PegasusBackend. Nothing here talks
// to a real Pegasus DL.

#include "fake_backend.hpp"
#include "orbit/files_logic.hpp"
#include "orbit/pegasus_backend.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>

#include <unistd.h>

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
    std::vector<std::string> logged;

    Mgr() : backend(host::FakeMode::normal)
    {
        context.app_version = "00.001.000";
        context.log_line = [this](const std::string &line) { logged.push_back(line); };
        store.poll_once();
        store.load_catalog_once();
        app.show_tab(tab::files);
        frame();
        work();
    }
    FilesScreen &files()
    {
        return app.files_screen();
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
    void nav(Direction d, int times = 1)
    {
        for (int i = 0; i < times; ++i)
            frame(d);
    }
    // Frames that pass this much (simulated) time, answering calls now and then.
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
    bool logged_contains(const std::string &text) const
    {
        return std::any_of(logged.begin(), logged.end(),
                           [&](const std::string &l) { return l.find(text) != std::string::npos; });
    }
    std::string focused_name()
    {
        return files().entry_count() > 0 ? files().entry_name(files().index()) : std::string();
    }
    void focus_entry(const std::string &name)
    {
        for (int i = 0; i < files().entry_count(); ++i)
        {
            if (files().entry_name(i) == name)
            {
                const int at = files().index();
                nav(i > at ? Direction::down : Direction::up, std::abs(i - at));
                return;
            }
        }
        ADD_FAILURE() << name << " is not listed in " << files().path();
    }
    void open(const std::string &path)
    {
        files().open_path(path);
        work();
    }
    int action_index(const std::string &code)
    {
        const auto &codes = files().menu_codes();
        return static_cast<int>(std::find(codes.begin(), codes.end(), code) - codes.begin());
    }
    void choose_action(const std::string &code)
    {
        nav(Direction::down, action_index(code));
        press(Action::confirm);
    }
};

FileOp running_op(const char *type, const char *source, const char *destination, double total, double done)
{
    FileOp op;
    op.type = type;
    op.status = "running";
    op.source = source;
    op.destination = destination;
    op.total_items = 1;
    op.total_bytes = total;
    op.done_bytes = done;
    return op;
}

// ---- rules ----

TEST(FilesLogic, PathsGoUpBySegmentToTheRoot)
{
    EXPECT_EQ(files::parent("/mnt/ext0"), "/mnt");
    EXPECT_EQ(files::parent("/mnt"), "/");
    EXPECT_EQ(files::parent("/"), "");
    EXPECT_EQ(files::normalize("//data//x/"), "/data/x");
    EXPECT_EQ(files::normalize(""), "/");
    EXPECT_EQ(files::base_name("/mnt/ext0"), "ext0");
    EXPECT_TRUE(files::inside("/data/homebrew", "/data"));
    EXPECT_FALSE(files::inside("/database", "/data"));
    const auto parts = files::crumbs("/mnt/ext0/homebrew");
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0].path, "/");
    EXPECT_EQ(parts[2].label, "ext0");
    EXPECT_EQ(parts[3].path, "/mnt/ext0/homebrew");
}

TEST(FilesLogic, DrivesFollowPegasusFilterWithTheMainDriveFirstAndAlways)
{
    EXPECT_EQ(files::drives({}).size(), 2u); // Main drive and Root without any locations
    std::vector<StorageMount> raw = {{"/", "/", "", false, 1, 1},          {"/dev", "/dev", "", false, 0, 0},
                                     {"/mnt/usb0", "u", "", true, 2e12, 1e12}, {"/mnt/ext0", "e", "", true, 2.5e11, 2e10},
                                     {"/data", "d", "", true, 6.7e11, 2.9e11}, {"/mnt/ext0x", "x", "", true, 1, 1},
                                     {"/system_tmp", "s", "", true, 1, 1}};
    const auto d = files::drives(raw);
    ASSERT_EQ(d.size(), 4u);
    EXPECT_EQ(d[0].path, "/data");
    EXPECT_EQ(d[0].label, "Main drive");
    EXPECT_DOUBLE_EQ(d[0].free_bytes, 2.9e11);
    EXPECT_EQ(d[1].path, "/mnt/ext0");
    EXPECT_EQ(d[2].path, "/mnt/usb0");
    EXPECT_EQ(d[3].path, "/");
}

TEST(FilesLogic, SizesGoUpToTerabytesWithoutLosingBytes)
{
    EXPECT_EQ(files::format_size(512.0), "512 B");
    EXPECT_EQ(files::format_size(2048.0), "2.0 KB");
    EXPECT_EQ(files::format_size(43.0e6), "43.0 MB");
    EXPECT_EQ(files::format_size(170804641792.0), "170.8 GB");
    EXPECT_EQ(files::format_size(2.0e12), "2.0 TB");
    EXPECT_EQ(files::format_size(18.4e12), "18.4 TB");
    // 170,804,641,792 is far below 2^53: exact in a double.
    EXPECT_EQ(static_cast<std::int64_t>(170804641792.0), 170804641792LL);
}

TEST(FilesLogic, OperationStatesMatchPegasus)
{
    EXPECT_EQ(files::canonical_status("waitingPassword"), "waiting_password");
    EXPECT_EQ(files::canonical_status("waiting_password"), "waiting_password");
    FileOp op;
    op.status = "waiting_password";
    EXPECT_TRUE(files::op_waiting_password(op));
    EXPECT_TRUE(op.active());
    EXPECT_TRUE(files::op_can_cancel(op));
    op.status = "accepted"; // the end of a pkg-install
    EXPECT_FALSE(op.active());
    EXPECT_TRUE(files::op_terminal(op));
    op.status = "queued";
    EXPECT_FALSE(files::op_can_cancel(op));
    op.status = "running";
    op.cancelable = false;
    EXPECT_FALSE(files::op_can_cancel(op));
    op.status = "completed";
    op.total_bytes = 5;
    op.done_bytes = 0; // a same-drive move completes like this
    EXPECT_FLOAT_EQ(files::op_fraction(op), 1.0f);
}

TEST(FilesLogic, TheTrackerKeepsTheLastTenFinishedAndSurvivesARestart)
{
    files::OpsTracker t;
    std::vector<FileOp> ops;
    for (int i = 1; i <= 14; ++i)
    {
        FileOp op = running_op("copy", "/data/a.bin", "/mnt/usb0", 10, 10);
        op.id = std::to_string(i);
        op.status = "completed";
        ops.push_back(op);
    }
    t.take(ops, 1.0);
    EXPECT_EQ(t.finished().size(), 10u);
    // Pegasus restarted: id 1 is a new move; the old copy 1 stays as history.
    FileOp fresh = running_op("move", "/mnt/ext0/x", "/data", 100, 10);
    fresh.id = "1";
    const auto done = t.take({fresh}, 2.0);
    EXPECT_TRUE(done.empty());
    ASSERT_EQ(t.active().size(), 1u);
    EXPECT_EQ(t.active()[0]->op.type, "move");
    // A rate and an ETA from Wings' own samples.
    fresh.done_bytes = 30;
    t.take({fresh}, 4.0);
    EXPECT_GT(files::OpsTracker::eta_seconds(*t.active()[0]), 0.0);
    // It goes; it was not cancelled or finished as far as anyone knows.
    const auto gone = t.take({}, 5.0);
    ASSERT_EQ(gone.size(), 1u);
    EXPECT_EQ(gone[0].status, "gone");
    EXPECT_FALSE(t.any_active());
}

TEST(FilesLogic, DestinationGuards)
{
    using files::Verdict;
    EXPECT_EQ(files::check_destination({"/data/a/x"}, {false}, "/data/a", true, true, -1, 0).verdict,
              Verdict::refused); // already here
    EXPECT_EQ(files::check_destination({"/data/a"}, {true}, "/data/a/b", true, true, -1, 0).verdict,
              Verdict::refused); // into itself
    EXPECT_EQ(files::check_destination({"/data/x"}, {false}, "/system", false, false, -1, 0).verdict,
              Verdict::refused); // read-only
    EXPECT_EQ(files::check_destination({"/mnt/ext0/g"}, {false}, "/data", true, true, 1e9, 1.7e11).verdict,
              Verdict::warn); // not enough free space: allowed, with a warning
    EXPECT_EQ(files::check_destination({"/mnt/ext0/g"}, {false}, "/data", true, true, 3e11, 1.7e11).verdict,
              Verdict::ok);
}

// ---- B1: navigation never traps ----

TEST(Files, StartsOnTheMainDriveWithDriveChips)
{
    Mgr m;
    EXPECT_EQ(m.files().path(), "/data");
    EXPECT_TRUE(m.files().error().empty());
    EXPECT_GE(m.files().entry_count(), 2);
    EXPECT_FALSE(m.files().modal());
}

TEST(Files, UpFromAnExternalDriveFallsBackPastAnUnlistableMntThenTheTabBar)
{
    Mgr m;
    m.open("/mnt/ext0");
    ASSERT_EQ(m.files().path(), "/mnt/ext0");
    m.press(Action::back); // /mnt cannot be listed: "/" with the focus on mnt
    m.work();
    EXPECT_EQ(m.files().path(), "/");
    EXPECT_TRUE(m.files().error().empty());
    EXPECT_EQ(m.focused_name(), "mnt");
    EXPECT_TRUE(m.logged_contains("cannot be listed; trying /"));
    m.press(Action::back); // at the top: the tab bar
    EXPECT_TRUE(m.app.top_bar_focused());
}

TEST(Files, TenHopsAcrossDrivesAllLandWithTheFocusRestored)
{
    Mgr m;
    const auto expect_at = [&](const std::string &path, const std::string &focus)
    {
        m.work();
        EXPECT_EQ(m.files().path(), path);
        EXPECT_TRUE(m.files().error().empty()) << m.files().error();
        EXPECT_FALSE(m.files().loading());
        if (!focus.empty())
            EXPECT_EQ(m.focused_name(), focus);
    };
    m.open("/data/homebrew");
    expect_at("/data/homebrew", "");
    // 1: chips: Up (breadcrumb), Up (chips, on Main drive), Right (ext0), Cross.
    m.nav(Direction::up, 2);
    EXPECT_EQ(m.files().area(), FilesScreen::Area::chips);
    m.nav(Direction::right);
    m.press(Action::confirm);
    expect_at("/mnt/ext0", "");
    m.focus_entry("homebrew"); // 2
    m.press(Action::confirm);
    expect_at("/mnt/ext0/homebrew", "");
    EXPECT_EQ(m.files().entry_count(), 3);
    m.press(Action::back); // 3
    expect_at("/mnt/ext0", "homebrew");
    m.press(Action::back); // 4: past /mnt
    expect_at("/", "mnt");
    m.focus_entry("data"); // 5
    m.press(Action::confirm);
    expect_at("/data", "");
    m.focus_entry("homebrew"); // 6
    m.press(Action::confirm);
    expect_at("/data/homebrew", "");
    m.press(Action::back); // 7
    expect_at("/data", "homebrew");
    m.press(Action::back); // 8
    expect_at("/", "data");
    m.nav(Direction::up, 2); // 9: chips to usb0
    m.nav(Direction::right, 2);
    m.press(Action::confirm);
    expect_at("/mnt/usb0", "");
    m.press(Action::back); // 10
    expect_at("/", "mnt");
    // Main drive from the chips, from anywhere.
    m.nav(Direction::up, 3);
    m.nav(Direction::left, 5);
    m.press(Action::confirm);
    expect_at("/data", "");
}

TEST(Files, CircleClearsTheFilterThenTheMarksBeforeGoingUp)
{
    Mgr m;
    m.open("/mnt/ext0/homebrew");
    m.files().set_filter("exfat");
    EXPECT_EQ(m.files().entry_count(), 2);
    m.press(Action::west);
    EXPECT_EQ(m.files().marked_count(), 1);
    m.press(Action::back);
    EXPECT_TRUE(m.files().filter().empty());
    EXPECT_EQ(m.files().path(), "/mnt/ext0/homebrew");
    m.press(Action::back);
    EXPECT_EQ(m.files().marked_count(), 0);
    EXPECT_EQ(m.files().path(), "/mnt/ext0/homebrew");
    m.press(Action::back);
    m.work();
    EXPECT_EQ(m.files().path(), "/mnt/ext0");
}

// ---- B2: listings are never stale or dropped by something else ----

TEST(Files, TheLargeGamesAreListedWithExactSizesAndSortBySize)
{
    Mgr m;
    m.open("/mnt/ext0/homebrew");
    ASSERT_EQ(m.files().entry_count(), 3);
    bool seen = false;
    for (int i = 0; i < 3; ++i)
    {
        if (m.files().entry_name(i) == "PPSA02015.exfat")
        {
            seen = true;
            EXPECT_EQ(m.files().entry(i).size_bytes, 170804641792.0);
            EXPECT_EQ(files::format_size(m.files().entry(i).size_bytes), "170.8 GB");
        }
    }
    EXPECT_TRUE(seen);
    m.press(Action::r3); // Date
    m.press(Action::r3); // Size, largest first
    EXPECT_EQ(m.files().entry_name(0), "PPSA02015.exfat");
    EXPECT_EQ(m.files().entry_name(2), "PPSA06464.exfat");
}

TEST(Files, AListingThatTimesOutShowsAnErrorForTheNewFolderAndRetryWorks)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    ASSERT_GT(m.files().entry_count(), 0);
    m.backend.set_slow_listing("/mnt/ext0/homebrew", 0, true);
    m.open("/mnt/ext0/homebrew");
    EXPECT_EQ(m.files().path(), "/mnt/ext0/homebrew");
    EXPECT_FALSE(m.files().error().empty());
    EXPECT_EQ(m.files().entry_count(), 0); // never the old folder's rows
    EXPECT_TRUE(m.logged_contains("/mnt/ext0/homebrew error after"));
    m.backend.set_slow_listing("/mnt/ext0/homebrew", 0, false);
    m.press(Action::confirm); // Retry has the focus
    m.work();
    EXPECT_TRUE(m.files().error().empty());
    EXPECT_EQ(m.files().entry_count(), 3);
}

TEST(Files, ASlowListingShowsTheTargetLoadingAndTheInputStaysFree)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.backend.set_slow_listing("/mnt/ext0/homebrew", 40, false);
    m.files().open_path("/mnt/ext0/homebrew");
    for (int i = 0; i < 300; ++i) // five seconds with no answer
        m.frame();
    EXPECT_TRUE(m.files().loading());
    EXPECT_EQ(m.files().path(), "/mnt/ext0/homebrew");
    EXPECT_EQ(m.files().entry_count(), 0);
    EXPECT_FALSE(m.files().modal()); // nothing covers the screen
    m.work();
    EXPECT_FALSE(m.files().loading());
    EXPECT_EQ(m.files().entry_count(), 3);
}

TEST(Files, OnlyANewerFolderRequestDropsAnAnswer)
{
    Mgr m;
    m.files().open_path("/data/homebrew");
    m.files().open_path("/mnt/ext0/homebrew");
    m.work();
    EXPECT_EQ(m.files().path(), "/mnt/ext0/homebrew");
    EXPECT_EQ(m.files().entry_count(), 3);
    EXPECT_TRUE(m.logged_contains("/data/homebrew dropped-stale"));
}

TEST(Files, AnOperationFinishingDoesNotOverrideANewerNavigation)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    const std::string id = m.backend.add_file_op(
        running_op("copy", "/data/pegasus-dl/downloads/notes.txt", "/data/pegasus-dl/downloads/Extracted", 2048, 100));
    m.seconds(6.0f); // the app sees it running
    ASSERT_TRUE(m.files().ops().any_active());
    // Let the next poll be queued, unanswered.
    for (int i = 0; i < 120; ++i)
        m.frame();
    // Open a folder: its listing is queued after that poll. The op then finishes.
    m.focus_entry("Extracted");
    m.press(Action::confirm);
    m.backend.file_cancel(id);
    while (m.store.run_next_action())
    {
    }
    m.frame(); // the poll answer (op finished) and the listing answer arrive in this order
    m.work();
    EXPECT_EQ(m.files().path(), "/data/pegasus-dl/downloads/Extracted");
    ASSERT_EQ(m.files().entry_count(), 1);
    EXPECT_EQ(m.files().entry_name(0), "readme.nfo");
    EXPECT_FALSE(m.files().ops().any_active());
    EXPECT_FALSE(m.logged_contains("Extracted dropped-stale"));
}

// ---- Where to? ----

TEST(Files, WhereToStartsOnTheMainDriveCountsHiddenFilesAndCircleWalksOut)
{
    Mgr m;
    m.open("/mnt/ext0/homebrew");
    m.focus_entry("PPSA02530.ffpfsc");
    m.press(Action::west); // mark, then down
    m.press(Action::west);
    EXPECT_EQ(m.files().marked_count(), 2);
    m.press(Action::north); // actions for the marked set
    ASSERT_TRUE(m.files().modal());
    EXPECT_EQ(m.files().menu_codes().front(), "move");
    m.press(Action::confirm); // Move to...
    m.work();
    DestinationBrowser &dest = m.files().destination();
    ASSERT_TRUE(dest.is_open());
    EXPECT_EQ(dest.path(), "/data/homebrew"); // from an extended drive: the main one
    EXPECT_EQ(dest.hidden_files(), 1);
    EXPECT_TRUE(dest.row_enabled(0));
    EXPECT_NE(dest.row_label(0).find("Move here"), std::string::npos);
    m.press(Action::back);
    m.work();
    EXPECT_EQ(dest.path(), "/data");
    m.press(Action::back);
    m.work();
    EXPECT_EQ(dest.path(), "/");
    m.press(Action::back); // at the top: leaves, the selection stays
    EXPECT_FALSE(dest.is_open());
    EXPECT_EQ(m.files().marked_count(), 2);
}

TEST(Files, WhereToFallsBackPastMntAndRefusesTheSameFolder)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.focus_entry("notes.txt");
    m.press(Action::confirm); // a file: its actions
    m.choose_action("copy");
    m.work();
    DestinationBrowser &dest = m.files().destination();
    ASSERT_TRUE(dest.is_open());
    EXPECT_EQ(dest.path(), "/mnt/ext0"); // from the main drive: the first other drive
    m.press(Action::back); // /mnt cannot be listed
    m.work();
    EXPECT_EQ(dest.path(), "/");
    EXPECT_TRUE(dest.error().empty());
    // The chips sit above the first row: Main drive.
    m.nav(Direction::up, 12);
    EXPECT_TRUE(dest.on_chips());
    m.press(Action::confirm);
    m.work();
    EXPECT_EQ(dest.path(), "/data");
    m.press(Action::back);
    m.press(Action::back); // walking out keeps working while a listing is in flight
    m.work();
    EXPECT_FALSE(dest.is_open());
}

TEST(Files, CopyWithReplaceAsksOnceMoreThenStartsWithoutAnOverlay)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.focus_entry("notes.txt");
    m.press(Action::confirm);
    m.choose_action("copy");
    m.work();
    DestinationBrowser &dest = m.files().destination();
    ASSERT_EQ(dest.path(), "/mnt/ext0");
    m.press(Action::west); // Keep both -> Replace
    EXPECT_EQ(dest.collision(), "replace");
    m.press(Action::north); // Copy here
    EXPECT_FALSE(dest.is_open());
    ASSERT_TRUE(m.files().modal()); // "Replace existing items?"
    m.nav(Direction::right);
    m.press(Action::confirm);
    EXPECT_FALSE(m.files().modal()); // no "Asking Pegasus DL" overlay
    EXPECT_NE(m.files().strip_text().find("Starting the copy"), std::string::npos);
    m.work();
    EXPECT_TRUE(m.backend.path_exists("/mnt/ext0/notes.txt"));
    EXPECT_EQ(m.backend.last_collision(), "replace");
    EXPECT_GE(m.files().op_count(), 1);
}

// ---- operations and B3 (cancel) ----

TEST(Files, TheStripIsAlwaysThereAndSaysHowToCancel)
{
    Mgr m;
    EXPECT_NE(m.files().strip_text().find("No file operations running"), std::string::npos);
    m.backend.add_file_op(running_op("move", "/mnt/ext0/homebrew/PPSA02015.exfat", "/data/homebrew",
                                     170804641792.0, 71.7e9));
    m.seconds(6.0f);
    const std::string strip = m.files().strip_text();
    EXPECT_NE(strip.find("Moving PPSA02015.exfat"), std::string::npos) << strip;
    EXPECT_NE(strip.find("170.8 GB"), std::string::npos) << strip;
    EXPECT_NE(strip.find("Options: Manage / Cancel"), std::string::npos) << strip;
    EXPECT_TRUE(m.files().ops_active());
}

TEST(Files, CancelTakesThreePressesAndAStubbornOperationKeepsTheUiFree)
{
    Mgr m;
    m.backend.add_file_op(running_op("move", "/mnt/ext0/homebrew/PPSA02015.exfat", "/data/homebrew",
                                     170804641792.0, 1e9),
                          true, 0.3); // Pegasus honours the cancel only 0.3 s later
    m.seconds(6.0f);
    ASSERT_TRUE(m.files().ops().any_active());
    m.press(Action::menu); // 1: the operations, newest active first
    EXPECT_TRUE(m.files().ops_panel_open());
    m.press(Action::west); // 2: cancel this one?
    EXPECT_TRUE(m.files().cancel_asked());
    m.press(Action::west); // 3: Square again stops it
    m.work();
    EXPECT_EQ(m.backend.cancelled_ops(), 1);
    m.work();
    const std::string strip = m.files().strip_text();
    EXPECT_NE(strip.find("Cancelling Moving PPSA02015.exfat"), std::string::npos) << strip;
    EXPECT_NE(strip.find("partial file at /data/homebrew"), std::string::npos) << strip;
    // Meanwhile everything answers: Circle closes the panel, L1/R1 switch tabs.
    m.press(Action::back);
    EXPECT_FALSE(m.files().modal());
    m.press(Action::page_next);
    EXPECT_EQ(m.app.current_tab(), tab::files + 1);
    m.press(Action::page_prev);
    usleep(400000);
    m.seconds(4.0f);
    EXPECT_FALSE(m.files().ops().any_active());
    EXPECT_NE(m.files().strip_text().find("Cancelled"), std::string::npos) << m.files().strip_text();
}

TEST(Files, AnOperationPegasusCannotStopSaysSo)
{
    Mgr m;
    FileOp op = running_op("copy", "/data/a.bin", "/mnt/usb0", 1e9, 1e8);
    op.cancelable = false;
    m.backend.add_file_op(op);
    m.seconds(6.0f);
    EXPECT_NE(m.files().strip_text().find("cannot stop"), std::string::npos);
    m.press(Action::menu);
    m.press(Action::west);
    EXPECT_FALSE(m.files().cancel_asked());
    EXPECT_EQ(m.backend.cancelled_ops(), 0);
}

TEST(Files, OperationsArePolledOnOtherTabsAndPollErrorsAreShown)
{
    Mgr m;
    m.backend.add_file_op(running_op("copy", "/data/a.bin", "/mnt/usb0", 1e9, 1e8), false);
    m.seconds(6.0f);
    ASSERT_TRUE(m.files().ops().any_active());
    m.app.show_tab(tab::store);
    m.seconds(8.0f); // the fake finishes it after three polls
    EXPECT_FALSE(m.files().ops().any_active());
    ASSERT_FALSE(m.files().ops().finished().empty());
    EXPECT_EQ(m.files().ops().finished()[0]->op.status, "completed");
    // Pegasus too busy to answer the polls: said after three failures.
    m.app.show_tab(tab::files);
    m.backend.add_file_op(running_op("move", "/data/b.bin", "/mnt/usb0", 1e9, 1e8));
    m.seconds(6.0f);
    m.backend.set_ops_failing(true);
    m.seconds(8.0f);
    EXPECT_GE(m.files().ops().failures(), 3);
    EXPECT_NE(m.files().strip_text().find("Can't read operation status"), std::string::npos);
    EXPECT_TRUE(m.logged_contains("ops poll"));
    m.backend.set_ops_failing(false);
    m.seconds(3.0f);
    EXPECT_EQ(m.files().ops().failures(), 0);
}

TEST(Files, APkgInstallThatEndsAcceptedIsFinished)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.focus_entry("Ember Hollow.pkg");
    m.press(Action::confirm);
    m.choose_action("install");
    m.press(Action::confirm);
    m.work();
    ASSERT_EQ(m.backend.pkg_installs().size(), 1u);
    m.seconds(8.0f);
    EXPECT_FALSE(m.files().ops().any_active()); // "accepted" ends the polling
    ASSERT_FALSE(m.files().ops().finished().empty());
    EXPECT_EQ(m.files().ops().finished()[0]->op.status, "accepted");
}

TEST(Files, AnArchiveWaitingForAPasswordAsksOnceAndAgainAfterAWrongOne)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.focus_entry("Locked Archive.rar");
    m.press(Action::confirm);
    m.choose_action("extract");
    m.work();
    ASSERT_TRUE(m.files().destination().is_open());
    EXPECT_EQ(m.files().destination().path(), "/data/pegasus-dl/downloads");
    m.press(Action::north); // Extract here
    // Changed: the six known passwords are tried first (none is "secret", Pegasus keeps the
    // operation waiting with the same reason, so each one waits out the settle time), then
    // the prompt opens by itself.
    m.seconds(70.0f);
    ASSERT_TRUE(m.files().prompt_open()); // asked by itself (waiting_password)
    m.files().type_into_prompt("wrong");
    m.nav(Direction::down, 10);
    m.press(Action::confirm);
    m.seconds(15.0f);
    ASSERT_TRUE(m.files().prompt_open()); // asked again, with Pegasus's reason
    EXPECT_FALSE(m.backend.path_exists("/data/pegasus-dl/downloads/Locked Archive.rar (extracted)"));
    m.files().type_into_prompt("secret");
    m.nav(Direction::down, 10);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.backend.path_exists("/data/pegasus-dl/downloads/Locked Archive.rar (extracted)"));
}

TEST(Files, DeleteAsksWithTheFocusOnKeep)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.focus_entry("notes.txt");
    m.press(Action::confirm);
    m.choose_action("delete");
    m.press(Action::confirm); // Keep
    m.work();
    EXPECT_TRUE(m.backend.deleted_paths().empty());
    m.press(Action::confirm);
    m.choose_action("delete");
    m.nav(Direction::right);
    m.press(Action::confirm);
    m.work();
    ASSERT_EQ(m.backend.deleted_paths().size(), 1u);
    m.seconds(6.0f); // the finished delete reloads the folder
    for (int i = 0; i < m.files().entry_count(); ++i)
        EXPECT_NE(m.files().entry_name(i), "notes.txt");
}

TEST(Files, RenameAndNewFolder)
{
    Mgr m;
    m.open("/data/pegasus-dl/downloads");
    m.focus_entry("notes.txt");
    m.press(Action::confirm);
    m.choose_action("rename");
    m.files().type_into_prompt("2");
    m.nav(Direction::down, 10);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.backend.path_exists("/data/pegasus-dl/downloads/notes.txt2"));
    EXPECT_EQ(m.focused_name(), "notes.txt2");
    m.press(Action::north);
    m.choose_action("mkdir");
    m.files().type_into_prompt("Games");
    m.nav(Direction::down, 10);
    m.press(Action::confirm);
    m.work();
    EXPECT_TRUE(m.backend.path_exists("/data/pegasus-dl/downloads/Games"));
    EXPECT_EQ(m.focused_name(), "Games");
}

TEST(Files, AReadOnlyFolderDisablesChangesWithAReason)
{
    Mgr m;
    m.open("/system");
    m.press(Action::north);
    const auto &codes = m.files().menu_codes();
    ASSERT_FALSE(codes.empty());
    m.press(Action::confirm); // "Move to..." is first and disabled: nothing opens
    EXPECT_FALSE(m.files().destination().is_open());
}

// ---- the real Pegasus samples ----

std::string samples_dir()
{
    if (const char *env = std::getenv("WINGS_SAMPLES"))
        return env;
    return std::string(HUI_SOURCE_DIR) + "/../../recon/samples";
}

bool read_sample(const std::string &name, std::string *out)
{
    std::ifstream file(samples_dir() + "/" + name, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    *out = text.str();
    return true;
}

TEST(FilesReplay, TheRealListingKeepsEveryByteOfTheLargeGames)
{
    std::string listing;
    if (!read_sample("files/list_ext0_homebrew.json", &listing))
        GTEST_SKIP() << "recon/samples is not reachable from here";
    auto replay = std::make_shared<wings::http::ReplayTransport>();
    replay->add("GET", "/api/health", 200, R"({"status":"running","port":6970,"version":"1.11.0"})");
    replay->add("GET", "/api/queue", 200, R"({"jobs":[],"summary":{}})");
    replay->add("GET", "/api/files/list?path=%2Fmnt%2Fext0%2Fhomebrew", 200, listing);
    PegasusBackend backend(replay, "127.0.0.1:6970");
    const Result<FolderListing> list = backend.file_list("/mnt/ext0/homebrew");
    ASSERT_TRUE(list.ok()) << list.error;
    ASSERT_EQ(list.value.entries.size(), 3u);
    std::vector<FileItem> entries = list.value.entries;
    files::sort_entries(entries, files::SortKey::size, true);
    EXPECT_EQ(entries[0].name, "PPSA02015.exfat");
    EXPECT_EQ(entries[0].size_bytes, 170804641792.0);
    EXPECT_EQ(files::format_size(entries[1].size_bytes), "38.5 GB");
    EXPECT_EQ(files::format_size(entries[2].size_bytes), "4.1 GB");
}

TEST(FilesReplay, PegasusWaitingPasswordReachesTheScreenAndOpensThePrompt)
{
    std::string sample;
    if (!read_sample("mutating/files_move.json", &sample))
        GTEST_SKIP() << "recon/samples is not reachable from here";
    // The real reply of /api/files/move, with operation 6 waiting for a password
    // as Pegasus spells it (fm.js; core test_writes.cpp).
    const std::size_t res = sample.find("\"res\"");
    ASSERT_NE(res, std::string::npos);
    std::string ops = sample.substr(sample.find('{', res));
    ops = ops.substr(0, ops.rfind('}')); // drop the wrapper's closing brace
    const std::size_t running = ops.find("\"status\": \"running\"");
    ASSERT_NE(running, std::string::npos);
    ops.replace(running, std::string("\"status\": \"running\"").size(), "\"status\": \"waiting_password\"");
    auto replay = std::make_shared<wings::http::ReplayTransport>();
    replay->add("GET", "/api/health", 200, R"({"status":"running","port":6970,"version":"1.11.0"})");
    replay->add("GET", "/api/queue", 200, R"({"jobs":[],"summary":{}})");
    replay->add("GET", "/api/files/operations", 200, ops);
    PegasusBackend backend(replay, "127.0.0.1:6970");
    const Result<std::vector<FileOp>> got = backend.file_operations();
    ASSERT_TRUE(got.ok()) << got.error;
    ASSERT_EQ(got.value.size(), 6u);
    EXPECT_EQ(got.value[5].status, "waiting_password");
    EXPECT_TRUE(got.value[5].active());
    EXPECT_TRUE(files::op_waiting_password(got.value[5]));
    EXPECT_FALSE(got.value[1].active()); // a same-drive move completed with doneBytes 0
    EXPECT_FLOAT_EQ(files::op_fraction(got.value[1]), 1.0f);

    // The same reply through the Store and the Files tab: the prompt opens by itself.
    test::TestFonts fonts;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;
    store.poll_once();
    app.show_tab(tab::files);
    // Changed: the known passwords are tried first, so the prompt comes after about a minute
    // of (simulated) time instead of at once.
    for (int round = 0; round < 90; ++round)
    {
        while (store.run_next_action())
        {
        }
        InputFrame input;
        input.connected = true;
        app.update(input, 1.0f, feedback);
    }
    EXPECT_TRUE(app.files_screen().prompt_open());
    EXPECT_NE(app.files_screen().strip_text().find("needs a password"), std::string::npos);
}

} // namespace
} // namespace orbit
