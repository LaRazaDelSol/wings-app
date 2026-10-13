// Wings TV app - tests of "Download all parts": sequence, retries, other hosts, skip and continue, cancel, record.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "fake_backend.hpp"
#include "orbit/download_all.hpp"
#include "orbit/game_view.hpp"
#include "orbit/store.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <deque>
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
            all.tick(0.5f);
        }
    }
};

LinkStart queued_outcome()
{
    return LinkStart{};
}

LinkStart failure(const std::string &error, bool transient)
{
    LinkStart outcome;
    outcome.error = error;
    outcome.transient = transient;
    return outcome;
}

std::string key(const LinkRef &ref)
{
    return ref.package_id + "#" + std::to_string(ref.link_index);
}

// `count` parts of "pkg25" on VikingFile; `alternate` adds the same part on
// Rapidgator ("pkg30", same index) after it.
std::vector<PartPlan> plans(int count, bool alternate = false)
{
    std::vector<PartPlan> out;
    for (int i = 0; i < count; ++i)
    {
        PartPlan part;
        part.number = i + 1;
        part.refs.push_back(LinkRef{"pkg25", i});
        part.hosts.push_back("VikingFile");
        if (alternate)
        {
            part.refs.push_back(LinkRef{"pkg30", i});
            part.hosts.push_back("Rapidgator");
        }
        out.push_back(std::move(part));
    }
    return out;
}

// The series on a fake start: answers at once with `answer`, or (held) keeps
// every Done for the test to call.
struct Runner
{
    DownloadAll all;
    bool held = false;
    std::function<LinkStart(const LinkRef &)> answer = [](const LinkRef &) { return queued_outcome(); };
    std::vector<LinkRef> asked;
    std::deque<DownloadAll::Done> pending; // a deque: calling one may add the next
    std::vector<LinkRef> queued;
    int finished_calls = 0;

    void run(std::vector<PartPlan> parts)
    {
        all.begin(
            std::move(parts), "VikingFile",
            [this](const LinkRef &ref, DownloadAll::Done done)
            {
                asked.push_back(ref);
                if (held)
                    pending.push_back(std::move(done));
                else
                    done(answer(ref));
            },
            [this](const LinkRef &ref) { queued.push_back(ref); }, [this] { ++finished_calls; });
    }
    // Frames of `dt` seconds until the series ends (or the frames run out).
    void frames(int count, float dt = 0.5f)
    {
        for (int i = 0; i < count && all.active; ++i)
            all.tick(dt);
    }
};

const std::string kContinue = "Choose \"Download all\" again to continue: parts already queued are left out.";

bool ends_with(const std::string &text, const std::string &tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

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
    Runner r;
    r.held = true;
    r.run(plans(3));
    EXPECT_TRUE(r.all.active);
    ASSERT_EQ(r.asked.size(), 1u); // a start that has not answered holds the series at part 1
    EXPECT_EQ(r.all.current, 1);
    EXPECT_EQ(r.all.attempt, 1);
    EXPECT_EQ(r.all.trying_host, "VikingFile");
    r.frames(100); // frames alone move nothing while a part is out
    EXPECT_EQ(r.asked.size(), 1u);
    r.pending[0](queued_outcome());
    ASSERT_EQ(r.asked.size(), 2u);
    EXPECT_EQ(r.all.done, 1);
    EXPECT_EQ(r.all.current, 2);
    r.pending[0](queued_outcome()); // a second Done of the same part is ignored
    EXPECT_EQ(r.asked.size(), 2u);
    EXPECT_EQ(r.all.ok, 1);
    r.pending[1](queued_outcome());
    r.pending[2](queued_outcome());
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished);
    EXPECT_EQ(r.all.ok, 3);
    EXPECT_EQ(r.queued.size(), 3u);
    EXPECT_EQ(r.finished_calls, 1);
}

TEST(DownloadAll, AllQueuedOnTheFakeStart)
{
    Runner r;
    r.run(plans(5));
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished);
    EXPECT_EQ(r.all.total, 5);
    EXPECT_EQ(r.all.done, 5);
    EXPECT_EQ(r.all.ok, 5);
    EXPECT_TRUE(r.all.failed.empty());
    EXPECT_EQ(r.all.via_alternate, 0);
    EXPECT_EQ(r.asked.size(), 5u);
    EXPECT_EQ(r.queued.size(), 5u);
    EXPECT_EQ(r.finished_calls, 1);
    EXPECT_EQ(r.all.toast(), "5 parts queued from VikingFile");
}

TEST(DownloadAll, ATimeoutIsAskedAgainAfterAWaitAndThenQueues)
{
    Runner r;
    int calls = 0;
    r.answer = [&calls](const LinkRef &ref)
    {
        if (ref.link_index == 1 && ++calls <= 2)
            return failure("timed out", true);
        return queued_outcome();
    };
    r.run(plans(3));
    ASSERT_TRUE(r.all.active);
    EXPECT_EQ(r.asked.size(), 2u);
    EXPECT_EQ(r.all.current, 2);
    EXPECT_EQ(r.all.attempt, 1);
    EXPECT_FLOAT_EQ(r.all.waiting, kFirstBackoff);
    r.all.tick(1.0f);
    EXPECT_EQ(r.asked.size(), 2u); // still waiting
    EXPECT_FLOAT_EQ(r.all.waiting, 1.0f);
    r.all.tick(1.0f); // the wait is over: second attempt, timed out again
    EXPECT_EQ(r.asked.size(), 3u);
    EXPECT_EQ(r.all.attempt, 2);
    EXPECT_FLOAT_EQ(r.all.waiting, kSecondBackoff);
    r.all.tick(kSecondBackoff - 0.1f);
    EXPECT_EQ(r.asked.size(), 3u);
    r.all.tick(0.2f); // third attempt queues, part 3 follows at once
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished);
    EXPECT_EQ(r.all.ok, 3);
    EXPECT_TRUE(r.all.failed.empty());
    ASSERT_EQ(r.asked.size(), 5u);
    EXPECT_EQ(key(r.asked[1]), "pkg25#1");
    EXPECT_EQ(key(r.asked[2]), "pkg25#1");
    EXPECT_EQ(key(r.asked[3]), "pkg25#1");
    EXPECT_EQ(key(r.asked[4]), "pkg25#2");
}

TEST(DownloadAll, BackoffWaitsTwoThenSixSecondsThenGivesUpTheLink)
{
    EXPECT_EQ(kPartAttempts, 3);
    EXPECT_FLOAT_EQ(kFirstBackoff, 2.0f);
    EXPECT_FLOAT_EQ(kSecondBackoff, 6.0f);
    Runner r;
    r.answer = [](const LinkRef &) { return failure("timed out", true); };
    r.run(plans(1));
    EXPECT_EQ(r.asked.size(), 1u);
    r.all.tick(kFirstBackoff - 0.1f);
    EXPECT_EQ(r.asked.size(), 1u);
    r.all.tick(0.2f);
    EXPECT_EQ(r.asked.size(), 2u);
    r.all.tick(kSecondBackoff - 0.5f);
    EXPECT_EQ(r.asked.size(), 2u);
    EXPECT_NEAR(r.all.waiting, 0.5f, 1e-4f);
    r.all.tick(0.5f);
    EXPECT_EQ(r.asked.size(), 3u); // three attempts, then the part is given up
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished_with_failures);
    ASSERT_EQ(r.all.failed.size(), 1u);
    EXPECT_EQ(r.all.failed[0].number, 1);
    EXPECT_EQ(r.all.failed[0].why, "timed out");
    EXPECT_EQ(r.all.ok, 0);
    EXPECT_TRUE(r.queued.empty());
    r.frames(100);
    EXPECT_EQ(r.asked.size(), 3u); // nothing after the end
    // A zero or negative frame time never ends a wait.
    Runner z;
    z.answer = [](const LinkRef &) { return failure("timed out", true); };
    z.run(plans(1));
    z.all.tick(0.0f);
    z.all.tick(-5.0f);
    EXPECT_EQ(z.asked.size(), 1u);
}

TEST(DownloadAll, AnAnswerThatWillNotChangeGoesStraightToTheOtherHost)
{
    Runner r;
    r.answer = [](const LinkRef &ref)
    {
        if (ref.package_id == "pkg25" && ref.link_index == 0)
            return failure("not supported", false);
        return queued_outcome();
    };
    r.run(plans(2, true));
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished);
    ASSERT_EQ(r.asked.size(), 3u); // no retry, no wait
    EXPECT_EQ(key(r.asked[0]), "pkg25#0");
    EXPECT_EQ(key(r.asked[1]), "pkg30#0");
    EXPECT_EQ(key(r.asked[2]), "pkg25#1"); // the next part starts on the chosen host again
    ASSERT_EQ(r.queued.size(), 2u);
    EXPECT_EQ(key(r.queued[0]), "pkg30#0"); // the record keeps the link that worked
    EXPECT_EQ(r.all.via_alternate, 1);
    ASSERT_EQ(r.all.alternates.size(), 1u);
    EXPECT_EQ(r.all.alternates[0].number, 1);
    EXPECT_EQ(r.all.alternates[0].host, "Rapidgator");
    EXPECT_EQ(r.all.toast(), "2 parts queued from VikingFile (1 from another host)");
}

TEST(DownloadAll, APartThatFailsEverywhereIsSkippedAndTheSeriesGoesOn)
{
    Runner r;
    r.answer = [](const LinkRef &ref)
    {
        if (ref.link_index == 1)
            return failure("not found", false); // part 2, both hosts
        if (ref.link_index == 2 && ref.package_id == "pkg25")
            return failure("not supported", false); // part 3 comes from Rapidgator
        return queued_outcome();
    };
    r.run(plans(5, true));
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished_with_failures);
    EXPECT_EQ(r.all.total, 5);
    EXPECT_EQ(r.all.done, 5);
    EXPECT_EQ(r.all.ok, 4);
    EXPECT_EQ(r.asked.size(), 7u); // 1, 2, 2', 3, 3', 4, 5
    ASSERT_EQ(r.all.failed.size(), 1u);
    EXPECT_EQ(r.all.failed[0].number, 2);
    EXPECT_EQ(r.all.failed[0].why, "Rapidgator: not found");
    EXPECT_EQ(r.all.failed_number, 2);
    EXPECT_EQ(r.queued.size(), 4u);
    EXPECT_EQ(r.finished_calls, 1);
    EXPECT_EQ(r.all.report_title(), "4 of 5 parts queued");
    const std::string message = r.all.report_message();
    EXPECT_NE(message.find("Part 2 did not queue: Rapidgator: not found."), std::string::npos) << message;
    EXPECT_NE(message.find("4 parts were queued."), std::string::npos) << message;
    EXPECT_NE(message.find("1 part came from another host: part 3 from Rapidgator."), std::string::npos) << message;
    EXPECT_TRUE(ends_with(message, kContinue)) << message;
}

TEST(DownloadAll, FourFailedPartsInARowStopTheSeries)
{
    EXPECT_EQ(kStopAfterFailedParts, 4);
    Runner r;
    r.answer = [](const LinkRef &ref)
    {
        if (ref.link_index >= 2)
            return failure("Pegasus is not answering", false);
        return queued_outcome();
    };
    r.run(plans(10));
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::stopped);
    EXPECT_EQ(r.all.ok, 2);
    EXPECT_EQ(r.all.done, 6);
    EXPECT_EQ(r.all.failed.size(), 4u);
    EXPECT_EQ(r.asked.size(), 6u); // parts 7 to 10 were never tried
    EXPECT_EQ(r.finished_calls, 1);
    EXPECT_EQ(r.all.report_title(), "Stopped: 2 of 10 parts queued");
    const std::string message = r.all.report_message();
    EXPECT_NE(message.find("Pegasus seems stuck"), std::string::npos) << message;
    EXPECT_NE(message.find("Parts 3, 4, 5 and 1 more did not queue."), std::string::npos) << message;
    EXPECT_NE(message.find("2 parts were queued."), std::string::npos) << message;
    EXPECT_TRUE(ends_with(message, kContinue)) << message;
}

TEST(DownloadAll, AQueuedPartBreaksTheRowOfFailures)
{
    Runner r;
    r.answer = [](const LinkRef &ref)
    {
        const int number = ref.link_index + 1;
        if (number == 4 || number == 8)
            return queued_outcome();
        return failure("not found", false);
    };
    r.run(plans(9));
    EXPECT_EQ(r.all.end, DownloadAll::End::finished_with_failures); // 3 in a row at most
    EXPECT_EQ(r.all.ok, 2);
    EXPECT_EQ(r.all.failed.size(), 7u);
    EXPECT_EQ(r.asked.size(), 9u);
    const std::string message = r.all.report_message();
    EXPECT_NE(message.find("Parts 1, 2, 3 and 4 more did not queue. Last reason: not found."), std::string::npos)
        << message;
    // The last part failing as the fourth in a row ends the series normally.
    Runner tail;
    tail.answer = [](const LinkRef &ref)
    { return ref.link_index == 0 ? queued_outcome() : failure("not found", false); };
    tail.run(plans(5));
    EXPECT_EQ(tail.all.end, DownloadAll::End::finished_with_failures);
    EXPECT_EQ(tail.all.done, 5);
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
    EXPECT_EQ(s.finished_calls, 1);
}

TEST(DownloadAll, CancelWhileAPartIsOutWaitsForItsOutcome)
{
    Runner r;
    r.held = true;
    r.run(plans(5));
    r.pending[0](queued_outcome());
    r.all.cancel();
    EXPECT_TRUE(r.all.active); // part 2 is still out
    EXPECT_EQ(r.finished_calls, 0);
    r.pending[1](failure("timed out", true)); // it failed: no wait, no retry, the series ends
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::cancelled);
    EXPECT_EQ(r.all.ok, 1);
    EXPECT_EQ(r.asked.size(), 2u);
    EXPECT_TRUE(r.all.failed.empty());
    EXPECT_EQ(r.finished_calls, 1);
    const std::string message = r.all.report_message();
    EXPECT_NE(message.find("Stopped on your request"), std::string::npos) << message;
    EXPECT_TRUE(ends_with(message, kContinue)) << message;
}

TEST(DownloadAll, CancelDuringAWaitEndsAtOnce)
{
    Runner r;
    r.answer = [](const LinkRef &) { return failure("timed out", true); };
    r.run(plans(4));
    ASSERT_TRUE(r.all.active);
    EXPECT_GT(r.all.waiting, 0.0f);
    r.all.cancel();
    EXPECT_FALSE(r.all.active);
    EXPECT_EQ(r.all.end, DownloadAll::End::cancelled);
    EXPECT_FLOAT_EQ(r.all.waiting, 0.0f);
    EXPECT_EQ(r.finished_calls, 1);
    r.frames(100, 1.0f);
    EXPECT_EQ(r.asked.size(), 1u);
    EXPECT_EQ(r.all.report_title(), "Cancelled: 0 of 4 parts queued");
}

TEST(DownloadAll, ACaptureThatStartedIsNotAQueuedPart)
{
    LinkStart capture;
    capture.kind = LinkResult::Kind::capture;
    EXPECT_EQ(part_failure_reason(capture), "still resolving");
    Runner r;
    int calls = 0;
    r.answer = [&calls, capture](const LinkRef &) { return ++calls == 1 ? capture : queued_outcome(); };
    r.run(plans(1));
    EXPECT_TRUE(r.all.active); // asked again after a wait, even though it was not marked transient
    EXPECT_TRUE(r.queued.empty());
    EXPECT_EQ(r.all.ok, 0);
    EXPECT_FLOAT_EQ(r.all.waiting, kFirstBackoff);
    r.frames(10);
    EXPECT_EQ(r.all.end, DownloadAll::End::finished);
    EXPECT_EQ(r.all.ok, 1);
    EXPECT_EQ(r.queued.size(), 1u);
    // Never resolving: the part fails with that reason.
    Runner never;
    never.answer = [capture](const LinkRef &) { return capture; };
    never.run(plans(1));
    never.frames(100);
    EXPECT_EQ(never.asked.size(), 3u);
    EXPECT_TRUE(never.queued.empty());
    ASSERT_EQ(never.all.failed.size(), 1u);
    EXPECT_EQ(never.all.failed[0].why, "still resolving");
}

TEST(DownloadAll, FailureReasonsOfTheOtherOutcomes)
{
    EXPECT_EQ(part_failure_reason(queued_outcome()), "");
    EXPECT_EQ(part_failure_reason(failure("timed out", true)), "timed out");
    LinkStart blocked;
    blocked.kind = LinkResult::Kind::blocked;
    EXPECT_EQ(part_failure_reason(blocked), "not supported");
    blocked.message = "host is down";
    EXPECT_EQ(part_failure_reason(blocked), "host is down");
    LinkStart select;
    select.kind = LinkResult::Kind::select_files;
    EXPECT_NE(part_failure_reason(select).find("which files"), std::string::npos);
    LinkStart browser;
    browser.kind = LinkResult::Kind::browser_opened;
    EXPECT_NE(part_failure_reason(browser).find("PS5 browser"), std::string::npos);
}

TEST(DownloadAll, ANewRunIgnoresTheOutcomeOfTheOldOne)
{
    Runner r;
    r.held = true;
    r.run(plans(3));
    ASSERT_EQ(r.pending.size(), 1u);
    DownloadAll::Done old = r.pending[0];
    r.run(plans(2));
    old(queued_outcome());
    EXPECT_EQ(r.all.ok, 0);
    EXPECT_TRUE(r.queued.empty());
    EXPECT_EQ(r.all.total, 2);
    r.pending[1](queued_outcome());
    EXPECT_EQ(r.all.ok, 1);
    // A part with no link at all is listed, not a crash.
    Runner empty;
    std::vector<PartPlan> parts = plans(2);
    parts[0].refs.clear();
    parts[0].hosts.clear();
    empty.run(std::move(parts));
    EXPECT_EQ(empty.all.end, DownloadAll::End::finished_with_failures);
    EXPECT_EQ(empty.all.ok, 1);
    ASSERT_EQ(empty.all.failed.size(), 1u);
    EXPECT_EQ(empty.all.failed[0].number, 1);
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
