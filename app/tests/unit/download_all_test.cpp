// Wings TV app - tests of "Download all parts": sequence, skip, stop on failure, cancel, saved record.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "fake_backend.hpp"
#include "orbit/download_all.hpp"
#include "orbit/game_view.hpp"
#include "orbit/store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

struct TempDir
{
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/wings-queued-XXXXXX";
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

// A Store on the fake backend, driven by hand: the series calls start_link()
// and every outcome comes back through tick(), as on the frame thread.
struct Series
{
    host::FakeBackend backend;
    Store store{backend};
    DownloadAll all;
    StartedLinks queued;
    std::vector<LinkRef> order; // the calls start_link() received, in order
    int finished_calls = 0;
    std::function<void(const LinkRef &)> on_start; // test hook

    std::vector<LinkRef> parts(int count) const
    {
        std::vector<LinkRef> refs;
        for (int i = 0; i < count; ++i)
            refs.push_back(LinkRef{"pkg25", i});
        return refs;
    }
    void run(const std::vector<LinkRef> &refs)
    {
        std::vector<int> numbers;
        for (std::size_t i = 0; i < refs.size(); ++i)
            numbers.push_back(static_cast<int>(i) + 1);
        all.begin(
            refs, numbers, "VikingFile",
            [this](const LinkRef &ref, DownloadAll::Done done)
            {
                order.push_back(ref);
                if (on_start)
                    on_start(ref);
                store.start_link(ref.package_id, ref.link_index, "", std::move(done));
            },
            [this](const LinkRef &ref) { queued.add(ref); }, [this] { ++finished_calls; });
        for (int round = 0; round < 400 && all.active; ++round)
        {
            while (store.run_next_action())
            {
            }
            store.tick();
        }
    }
};

std::vector<PackageDetail> starfall_details(host::FakeBackend &backend)
{
    std::vector<PackageDetail> out;
    for (const char *id : {"pkg25", "pkg26", "pkg27"})
        out.push_back(backend.package(id).value);
    return out;
}

TEST(DownloadAll, QueuesEveryPartInOrderOneAtATime)
{
    Series s;
    s.run(s.parts(22));
    EXPECT_FALSE(s.all.active);
    EXPECT_EQ(s.all.end, DownloadAll::End::finished);
    EXPECT_EQ(s.all.ok, 22);
    EXPECT_EQ(s.finished_calls, 1);
    EXPECT_EQ(s.backend.started_links(), 22);
    ASSERT_EQ(s.order.size(), 22u);
    for (int i = 0; i < 22; ++i)
        EXPECT_EQ(s.order[static_cast<std::size_t>(i)].link_index, i);
    EXPECT_EQ(s.queued.keys.size(), 22u);
    EXPECT_EQ(s.all.toast(), "22 parts queued from VikingFile");
}

TEST(DownloadAll, NeverStartsTheNextPartBeforeThePreviousOneEnded)
{
    Series s;
    int in_flight = 0;
    int most = 0;
    s.on_start = [&](const LinkRef &)
    {
        ++in_flight;
        most = std::max(most, in_flight);
    };
    // The part counts as ended once the series moves on: start_link() of the
    // next one is only reached from the previous one's callback.
    s.run(s.parts(5));
    EXPECT_EQ(s.all.ok, 5);
    EXPECT_EQ(s.order.size(), 5u);
    // While part k ran, no later part had been asked for.
    EXPECT_EQ(in_flight, 5);
    EXPECT_EQ(most, 5);
    // Directly: a start that never answers holds the series at part 1.
    DownloadAll held;
    std::vector<LinkRef> asked;
    held.begin(
        s.parts(3), {1, 2, 3}, "H", [&](const LinkRef &ref, DownloadAll::Done) { asked.push_back(ref); }, {}, {});
    EXPECT_TRUE(held.active);
    EXPECT_EQ(asked.size(), 1u);
}

TEST(DownloadAll, SecondPressSkipsWhatWasQueued)
{
    host::FakeBackend backend;
    GameView v;
    load_game_view(&v, starfall_details(backend), {}, LinkPrefs{});
    int viking = -1;
    for (std::size_t m = 0; m < v.page.mirrors.size(); ++m)
        if (v.page.mirrors[m].host == "VikingFile" && v.page.mirrors[m].parts() == 22)
            viking = static_cast<int>(m);
    ASSERT_GE(viking, 0);
    Series s;
    StartedLinks started;
    PartsPlan plan = plan_parts(v, viking, started);
    ASSERT_EQ(plan.refs.size(), 22u);
    s.run({plan.refs.begin(), plan.refs.begin() + 10});
    started = s.queued;
    plan = plan_parts(v, viking, started);
    EXPECT_EQ(plan.refs.size(), 12u);
    EXPECT_EQ(plan.skipped, 10);
    EXPECT_EQ(plan.refs.front().link_index, 10);
    EXPECT_EQ(mirror_card(v, viking, LinkPrefs{}, started).started, 10);
    s.order.clear();
    s.run(plan.refs);
    EXPECT_EQ(s.order.size(), 12u);
    EXPECT_EQ(s.queued.keys.size(), 22u);
    EXPECT_EQ(plan_parts(v, viking, s.queued).refs.size(), 0u);
}

TEST(DownloadAll, FailureAtPartKStopsAndReportsKMinusOne)
{
    Series s;
    s.backend.set_failing_link("pkg25", 6); // part 7
    s.run(s.parts(22));
    EXPECT_EQ(s.all.end, DownloadAll::End::failed);
    EXPECT_EQ(s.all.ok, 6);
    EXPECT_EQ(s.all.failed_number, 7);
    EXPECT_EQ(s.order.size(), 7u); // nothing after the failure was tried
    EXPECT_EQ(s.queued.keys.size(), 6u);
    EXPECT_EQ(s.finished_calls, 1);
    EXPECT_EQ(s.all.report_title(), "6 of 22 parts queued");
    EXPECT_NE(s.all.report_message().find("Stopped at part 7"), std::string::npos);
    EXPECT_NE(s.all.report_message().find("6 parts were queued before it"), std::string::npos);
}

TEST(DownloadAll, CancelMidWayStopsAfterThePartInFlight)
{
    Series s;
    s.on_start = [&](const LinkRef &ref)
    {
        if (ref.link_index == 2)
            s.all.cancel(); // Circle while part 3 is being queued
    };
    s.run(s.parts(10));
    EXPECT_EQ(s.all.end, DownloadAll::End::cancelled);
    EXPECT_EQ(s.all.ok, 3); // the part in flight still counts
    EXPECT_EQ(s.order.size(), 3u);
    EXPECT_EQ(s.queued.keys.size(), 3u);
    EXPECT_EQ(s.finished_calls, 1);
    EXPECT_EQ(s.all.report_title(), "Cancelled: 3 of 10 parts queued");
    // Cancelling an idle series does nothing.
    s.all.cancel();
    EXPECT_FALSE(s.all.active);
}

TEST(DownloadAll, IncompleteAndRestartedSetsCarryTheirWarningInTheConfirmText)
{
    host::FakeBackend backend;
    GameView v;
    load_game_view(&v, starfall_details(backend), {}, LinkPrefs{});
    const StartedLinks none;
    int warned = 0;
    int captured = 0;
    for (std::size_t m = 0; m < v.page.mirrors.size(); ++m)
    {
        const Mirror &mirror = v.page.mirrors[m];
        if (!mirror.multipart())
            continue;
        const PartsPlan plan = plan_parts(v, static_cast<int>(m), none);
        if (plan.refs.empty())
            continue;
        if (!mirror.warning().empty())
        {
            ++warned;
            EXPECT_NE(plan.message.find("Careful: " + mirror.warning()), std::string::npos) << mirror.host;
        }
        if (plan.one_at_a_time)
        {
            ++captured;
            EXPECT_NE(plan.message.find("each part needs its own capture"), std::string::npos) << mirror.host;
            EXPECT_NE(plan.message.find(mirror.host), std::string::npos);
            EXPECT_FALSE(plan.title.empty());
        }
        else
        {
            EXPECT_NE(plan.message.find(std::to_string(plan.refs.size()) + " parts from " + plan.host),
                      std::string::npos);
        }
    }
    EXPECT_GE(warned, 1);
    EXPECT_GE(captured, 1);
}

TEST(QueuedRecord, SurvivesARestartAndSkipsDamagedLines)
{
    TempDir dir;
    const std::vector<std::string> paths = queued_record_paths({dir.path + "/wings_prefs.sav"});
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], dir.path + "/queued_parts.sav");
    StartedLinks loaded;
    EXPECT_FALSE(load_queued_record(paths, &loaded)); // no file yet

    StartedLinks saved;
    saved.add(LinkRef{"pkg25", 0});
    saved.add(LinkRef{"pkg25", 21});
    saved.add(LinkRef{"pkg26", 3});
    EXPECT_EQ(save_queued_record(paths, saved), 1);
    ASSERT_TRUE(load_queued_record(paths, &loaded));
    EXPECT_EQ(loaded.keys, saved.keys);
    EXPECT_TRUE(loaded.has(LinkRef{"pkg26", 3}));

    // Bad lines inside a good container are dropped, the rest is kept.
    StartedLinks tolerant;
    ASSERT_TRUE(decode_queued_record("v=1\nq=pkg1\t2\nq=\t5\nq=pkg2\tx\nq=nonsense\nzzz\nq=pkg3\t-1\nq=pkg4\t7\n",
                                     &tolerant));
    EXPECT_EQ(tolerant.keys.size(), 2u);
    EXPECT_TRUE(tolerant.has(LinkRef{"pkg1", 2}));
    EXPECT_TRUE(tolerant.has(LinkRef{"pkg4", 7}));
    // A newer layout or no version line is left alone.
    StartedLinks untouched;
    untouched.add(LinkRef{"keep", 1});
    EXPECT_FALSE(decode_queued_record("v=2\nq=pkg1\t2\n", &untouched));
    EXPECT_FALSE(decode_queued_record("q=pkg1\t2\n", &untouched));
    EXPECT_FALSE(decode_queued_record("", &untouched));
    EXPECT_EQ(untouched.keys.size(), 1u);
}

TEST(QueuedRecord, DamagedFileIsIgnoredAndTheNextPathIsTried)
{
    TempDir dir;
    const std::string bad = dir.path + "/bad.sav";
    const std::string good = dir.path + "/good.sav";
    StartedLinks saved;
    saved.add(LinkRef{"pkg25", 4});
    ASSERT_EQ(save_queued_record({good}, saved), 1);
    // Flipped byte in the checksummed container.
    std::string data = hui::save::encode(hui::save::Kind::prefs, 1, encode_queued_record(saved));
    data[data.size() / 2] = static_cast<char>(data[data.size() / 2] ^ 0x55);
    ASSERT_TRUE(hui::save::write_atomic(bad, data).empty());
    StartedLinks loaded;
    EXPECT_FALSE(load_queued_record({bad}, &loaded));
    EXPECT_TRUE(loaded.keys.empty());
    ASSERT_TRUE(load_queued_record({bad, good}, &loaded));
    EXPECT_TRUE(loaded.has(LinkRef{"pkg25", 4}));
    // Truncated and empty files.
    ASSERT_TRUE(hui::save::write_atomic(bad, data.substr(0, 7)).empty());
    EXPECT_FALSE(load_queued_record({bad}, &loaded));
    ASSERT_TRUE(hui::save::write_atomic(bad, "").empty());
    EXPECT_FALSE(load_queued_record({bad}, &loaded));
}

} // namespace
} // namespace orbit
