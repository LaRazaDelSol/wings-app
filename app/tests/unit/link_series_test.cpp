// Wings TV app - A series start_link() ("Download all parts") waits for an automated capture.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/link_retry.hpp"
#include "orbit/store.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orbit
{
namespace
{

constexpr int kAutomated = 1; // pkg1 link 1: an automated provider (MediaFire in the fake)

// Starts one series link, runs the worker by hand and delivers the outcome.
LinkStart start_and_finish(Store &store, const std::string &package_id, int link_index, int *calls)
{
    LinkStart seen;
    store.start_link(package_id, link_index, "",
                     [&seen, calls](const LinkStart &outcome)
                     {
                         seen = outcome;
                         ++*calls;
                     });
    while (store.run_next_action())
    {
    }
    store.tick();
    return seen;
}

TEST(LinkRetry, TransientErrorsAreTheOnesThatMayPass)
{
    EXPECT_TRUE(transient_link_error(0, "timed out", true));
    EXPECT_TRUE(transient_link_error(0, "connection refused", true));
    EXPECT_TRUE(transient_link_error(502, "The host did not answer.", false));
    EXPECT_TRUE(transient_link_error(503, "", false));
    EXPECT_TRUE(transient_link_error(408, "Request Timeout", false));
    EXPECT_TRUE(transient_link_error(425, "", false));
    EXPECT_TRUE(transient_link_error(429, "Too many requests", false));
    EXPECT_TRUE(transient_link_error(409, "A capture session is already running", false));
    EXPECT_TRUE(transient_link_error(409, "Pegasus is busy, try again", false));
    EXPECT_TRUE(transient_link_error(200, "Provider timed out", false));

    EXPECT_FALSE(transient_link_error(404, "Download link not found.", false));
    EXPECT_FALSE(transient_link_error(400, "Choose a debrid service first.", false));
    EXPECT_FALSE(transient_link_error(400, "That debrid service does not take this link.", false));
    EXPECT_FALSE(transient_link_error(200, "Akirabox is not supported.", false));
    EXPECT_FALSE(transient_link_error(410, "The file has expired", false));
    EXPECT_FALSE(transient_link_error(502, "The file was removed by the host", false));
    EXPECT_FALSE(transient_link_error(200, "", false));
}

TEST(LinkSeries, AnAutomatedCaptureEndsOnlyOnceItCompleted)
{
    host::FakeBackend backend;
    backend.set_capture_script({"priming", "queueing", "completed"});
    Store store(backend);
    store.set_series_wait(0.0, 1000.0);
    std::vector<std::string> lines;
    store.log = [&lines](const std::string &line) { lines.push_back(line); };
    int calls = 0;
    std::string state_when_done;
    std::size_t jobs_when_done = 0;
    store.start_link("pkg1", kAutomated, "",
                     [&](const LinkStart &outcome)
                     {
                         ++calls;
                         EXPECT_TRUE(outcome.error.empty()) << outcome.error;
                         EXPECT_EQ(outcome.kind, LinkResult::Kind::dispatched);
                         EXPECT_FALSE(outcome.transient);
                         state_when_done = store.state().capture.state;
                         jobs_when_done = store.state().jobs.size();
                     });
    ASSERT_TRUE(store.run_next_action()); // the start and the whole wait
    EXPECT_EQ(calls, 0);                 // nothing reaches the frame thread before tick()
    EXPECT_EQ(backend.capture_reads(), 3);
    while (store.run_next_action())
    {
    }
    store.tick();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(state_when_done, "completed"); // the snapshot shows the completion first
    EXPECT_GT(jobs_when_done, 0u);           // and the queued download
    store.tick();
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("[WINGS-PARTS] pkg=pkg1 link=1 kind=dispatched transient=0 ms=", 0), 0u)
        << lines[0];
}

TEST(LinkSeries, AFailedCaptureIsAnError)
{
    host::FakeBackend backend;
    backend.set_capture_script({"queueing", "failed"}, false, "The file was removed by the host");
    Store store(backend);
    store.set_series_wait(0.0, 1000.0);
    int calls = 0;
    LinkStart gone = start_and_finish(store, "pkg1", kAutomated, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(gone.error, "The file was removed by the host");
    EXPECT_FALSE(gone.transient);
    EXPECT_EQ(store.state().capture.state, "failed");

    backend.set_capture_script({"queueing", "failed"}, false, "Provider timed out");
    LinkStart slow = start_and_finish(store, "pkg1", kAutomated, &calls);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(slow.error, "Provider timed out");
    EXPECT_TRUE(slow.transient);
}

TEST(LinkSeries, AManualCaptureIsNotWaitedFor)
{
    host::FakeBackend backend;
    backend.set_capture_script({"watching"}, true);
    Store store(backend);
    store.set_series_wait(0.0, 1000.0);
    int calls = 0;
    LinkStart manual = start_and_finish(store, "pkg1", kAutomated, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(manual.error, "needs a check in the PS5 browser: open this part on its own");
    EXPECT_EQ(manual.kind, LinkResult::Kind::capture);
    EXPECT_FALSE(manual.transient);
    EXPECT_EQ(backend.capture_reads(), 0);
    EXPECT_FALSE(store.state().capture.session_id.empty()); // still shown for the player
}

TEST(LinkSeries, StopSeriesEndsTheWait)
{
    host::FakeBackend backend;
    backend.set_capture_script({"queueing"});
    Store store(backend);
    store.set_series_wait(0.0, 1000.0);
    backend.set_capture_hook(
        [&store](int reads)
        {
            if (reads == 2)
                store.stop_series();
        });
    int calls = 0;
    LinkStart stopped = start_and_finish(store, "pkg1", kAutomated, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(stopped.error, "stopped");
    EXPECT_FALSE(stopped.transient);
    EXPECT_EQ(backend.capture_reads(), 2);

    // The next series start clears the stop.
    backend.set_capture_hook({});
    backend.set_capture_script({"queueing", "completed"});
    LinkStart next = start_and_finish(store, "pkg1", kAutomated, &calls);
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(next.error.empty()) << next.error;
}

TEST(LinkSeries, AWaitThatRunsOutIsTransientAndTheSessionStaysWatched)
{
    host::FakeBackend backend;
    backend.set_capture_script({"queueing"});
    Store store(backend);
    store.set_series_wait(0.0, 0.0);
    int calls = 0;
    LinkStart late = start_and_finish(store, "pkg1", kAutomated, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(late.error, "timed out waiting for the provider");
    EXPECT_TRUE(late.transient);
    EXPECT_EQ(store.state().capture.state, "queueing");
    const int reads = backend.capture_reads();
    store.poll_once(); // the normal poll keeps watching it
    EXPECT_EQ(backend.capture_reads(), reads + 1);
}

TEST(LinkSeries, StartFailuresSayWhetherToAskAgain)
{
    host::FakeBackend backend;
    Store store(backend);
    int calls = 0;
    backend.set_failing_link("pkg1", 0);
    LinkStart host_error = start_and_finish(store, "pkg1", 0, &calls);
    EXPECT_FALSE(host_error.error.empty());
    EXPECT_TRUE(host_error.transient); // a 502

    LinkStart missing = start_and_finish(store, "pkg1", 99, &calls);
    EXPECT_EQ(missing.error, "Download link not found.");
    EXPECT_FALSE(missing.transient);

    backend.set_mode(host::FakeMode::offline);
    LinkStart offline = start_and_finish(store, "pkg1", 0, &calls);
    EXPECT_FALSE(offline.error.empty());
    EXPECT_TRUE(offline.transient); // Pegasus not answering
    EXPECT_EQ(calls, 3);
}

} // namespace
} // namespace orbit
