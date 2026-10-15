// Wings TV app - B-20 bad-link flags wired into the Store and Settings > Wings.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/link_flags.hpp"
#include "orbit/screens.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <ctime>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

const std::string kId = link_id("https://host.example/file.rar", "CUSA00001");

Job job(const std::string &id, const std::string &status, const std::string &error = {})
{
    Job j;
    j.id = id;
    j.status = status;
    j.error = error;
    return j;
}

// The app-wide flags for one test (no files written).
struct Flags
{
    LinkFlags flags;
    Flags()
    {
        set_link_flags(&flags, {});
    }
    ~Flags()
    {
        set_link_flags(nullptr, {});
    }
};

// A flagged start, then the job it queued shows up in the next queue read.
void start_flagged(Store &store)
{
    store.post_jobs_for_test({job("old", "completed")});
    store.tick();
    store.set_link_flag_id(kId, "Some Game");
    store.start_link("pkg1", 0, "");
    store.post_jobs_for_test({job("old", "completed"), job("j1", "downloading")});
    store.tick();
    ASSERT_EQ(store.job_flag_id("j1"), kId);
    EXPECT_EQ(store.job_flag_id("old"), "");
}

TEST(LinkFlagsWiring, AFailedJobFlagsItsLink)
{
    Flags f;
    host::FakeBackend backend;
    Store store(backend);
    start_flagged(store);
    store.post_jobs_for_test({job("j1", "failed", "File not found or deleted")});
    store.tick();
    const LinkFlag *flag = f.flags.find(kId, static_cast<std::int64_t>(std::time(nullptr)));
    ASSERT_NE(flag, nullptr);
    EXPECT_EQ(flag->reason, FlagReason::removed);
    EXPECT_EQ(flag->title, "Some Game");
}

TEST(LinkFlagsWiring, CancelledRetryingAndCatalogueMissesAreNotFlagged)
{
    Flags f;
    host::FakeBackend backend;
    Store store(backend);
    start_flagged(store);
    store.post_jobs_for_test({job("j1", "retrying", "HTTP 503")});
    store.tick();
    store.post_jobs_for_test({job("j1", "cancelled", "Cancelled")});
    store.tick();
    store.post_jobs_for_test({job("j1", "failed", "Download link not found.")});
    store.tick();
    EXPECT_EQ(f.flags.size(), 0u);
}

TEST(LinkFlagsWiring, AJobOfAnUnflaggedStartIsNotFlagged)
{
    Flags f;
    host::FakeBackend backend;
    Store store(backend);
    store.post_jobs_for_test({});
    store.tick();
    store.start_link("pkg1", 0, ""); // no set_link_flag_id(): a direct link, say
    store.post_jobs_for_test({job("j2", "failed", "File not found or deleted")});
    store.tick();
    EXPECT_EQ(f.flags.size(), 0u);
}

TEST(LinkFlagsWiring, AMarkAndItsClearRoundTripThroughTheFile)
{
    LinkFlags flags;
    flags.add(kId, FlagReason::by_hand, 1760400000, "Some\nGame");
    LinkFlags read;
    ASSERT_TRUE(read.decode(flags.encode()));
    const LinkFlag *flag = read.find(kId, 1760400000);
    ASSERT_NE(flag, nullptr);
    EXPECT_EQ(flag->reason, FlagReason::by_hand);
    EXPECT_EQ(flag->title, "Some Game");
    flags.add(kId, FlagReason::removed, 1760400100); // no name: keeps the old one
    EXPECT_EQ(flags.find(kId, 1760400100)->title, "Some Game");
    EXPECT_TRUE(flags.clear(kId));
    ASSERT_TRUE(read.decode(flags.encode()));
    EXPECT_EQ(read.size(), 0u);
    // An older file without names still reads.
    ASSERT_TRUE(read.decode(kId + " 2 1760400000\n"));
    EXPECT_EQ(read.size(), 1u);
}

// ---- Settings > Wings ----

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

    Mgr() : backend(host::FakeMode::normal)
    {
        store.poll_once();
        store.load_catalog_once();
        app.show_tab(tab::settings);
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
    }
    void press(Action action)
    {
        frame(Direction::none, hui::action_bit(action));
    }
    void select(const std::string &title)
    {
        const auto rows = app.settings_screen().row_list();
        int row = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].title == title)
                row = static_cast<int>(i);
        ASSERT_GE(row, 0) << title;
        const int at = app.settings_screen().index();
        for (int i = 0; i < std::abs(row - at); ++i)
            frame(row > at ? Direction::down : Direction::up);
        press(Action::confirm);
    }
};

TEST(LinkFlagsWiring, SettingsClearsAllFlagsAfterAsking)
{
    Flags f;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    f.flags.add(kId, FlagReason::removed, now, "Some Game");
    f.flags.add(link_id("https://other.example/b.rar", "CUSA00002"), FlagReason::by_hand, now);
    Mgr m;
    m.app.settings_screen().set_group(SettingsScreen::Group::wings);
    m.frame();
    m.select("Clear all bad-link flags");
    EXPECT_TRUE(m.app.settings_screen().modal());
    m.press(Action::confirm); // the focus starts on Cancel
    EXPECT_EQ(f.flags.size(), 2u);
    m.select("Clear all bad-link flags");
    m.frame(Direction::right);
    m.press(Action::confirm);
    EXPECT_EQ(f.flags.size(), 0u);
}

TEST(LinkFlagsWiring, SettingsListsFlagsAndSelectClearsOne)
{
    Flags f;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    f.flags.add(kId, FlagReason::removed, now, "Some Game");
    Mgr m;
    m.app.settings_screen().set_group(SettingsScreen::Group::wings);
    m.frame();
    m.select("Bad links");
    EXPECT_TRUE(m.app.settings_screen().modal());
    m.press(Action::confirm); // the one flag
    EXPECT_EQ(f.flags.size(), 0u);
}

} // namespace
} // namespace orbit
