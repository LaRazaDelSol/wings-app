// Wings TV app - Circle on a waiting box (B-18): a late answer after cancel changes nothing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/store.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

constexpr int kAutomated = 1; // pkg1 link 1: an automated provider in the fake

Probe direct_probe()
{
    Probe probe;
    probe.url = "https://example.com/files/late.bin";
    probe.final_url = probe.url;
    probe.file_name = "late.bin";
    probe.length = 1.0;
    return probe;
}

std::set<std::string> job_ids(host::FakeBackend &backend)
{
    std::set<std::string> ids;
    for (const Job &job : backend.queue().value)
        ids.insert(job.id);
    return ids;
}

void run_all(Store &store)
{
    while (store.run_next_action())
    {
    }
    store.tick();
}

TEST(CancelWaiting, ADirectLinkQueuedAfterCircleIsCancelledInPegasus)
{
    host::FakeBackend backend;
    Store store(backend);
    const std::set<std::string> before = job_ids(backend);
    std::vector<std::string> lines;
    store.log = [&lines](const std::string &line) { lines.push_back(line); };
    // Circle while Pegasus is answering: the answer arrives after the cancel.
    store.on_answer = [&store] { store.cancel(Store::lane_probe); };
    store.queue_direct(direct_probe(), "");
    ASSERT_TRUE(store.probe_action().busy);
    const std::uint64_t serial = store.probe_action().serial;
    run_all(store);

    EXPECT_FALSE(store.probe_action().busy);
    EXPECT_TRUE(store.probe_action().message.empty()); // no "queued" confirmation
    EXPECT_TRUE(store.probe_action().error.empty());
    EXPECT_EQ(store.probe_action().serial, serial); // no screen sees a finished action
    int added = 0;
    for (const Job &job : backend.queue().value)
    {
        if (before.count(job.id) != 0)
            continue;
        ++added;
        EXPECT_EQ(job.status, "cancelled") << job.id; // nothing half-done stays queued
    }
    EXPECT_EQ(added, 1);
    bool logged = false;
    for (const std::string &line : lines)
        logged = logged || line.rfind("[WINGS-CANCEL] late download cancelled", 0) == 0;
    EXPECT_TRUE(logged);
}

TEST(CancelWaiting, AnActionCancelledBeforeItRanIsNeverSent)
{
    host::FakeBackend backend;
    Store store(backend);
    const std::set<std::string> before = job_ids(backend);
    store.queue_direct(direct_probe(), "");
    store.cancel(Store::lane_probe);
    EXPECT_FALSE(store.probe_action().busy); // the box closes at once
    run_all(store);
    EXPECT_FALSE(store.probe_action().busy);
    EXPECT_EQ(job_ids(backend), before);
}

TEST(CancelWaiting, AnAnswerThatCameBeforeTheFrameIsNotShown)
{
    host::FakeBackend backend;
    Store store(backend);
    store.probe_link("https://example.com/files/a.bin");
    while (store.run_next_action())
    {
    }
    store.cancel(Store::lane_probe); // Circle in the same frame the answer came
    const std::uint64_t probes = store.state().probe_serial;
    store.tick();
    EXPECT_FALSE(store.probe_action().busy);
    EXPECT_EQ(store.state().probe_serial, probes);
    EXPECT_TRUE(store.state().probe.url.empty());
}

TEST(CancelWaiting, ALinkStartAnsweredAfterCircleIsDropped)
{
    host::FakeBackend backend;
    Store store(backend);
    const std::set<std::string> before = job_ids(backend);
    store.on_answer = [&store] { store.cancel(Store::lane_link); };
    const std::uint64_t links = store.state().link_serial;
    store.start_link("pkg1", 0, "");
    run_all(store);
    EXPECT_FALSE(store.link_action().busy);
    EXPECT_TRUE(store.link_action().error.empty());
    EXPECT_EQ(store.state().link_serial, links); // the answer is never applied
    for (const Job &job : backend.queue().value)
    {
        if (before.count(job.id) == 0) // whatever it queued was cancelled
            EXPECT_TRUE(job.status == "cancelled" || job.status == "completed") << job.status;
    }
}

TEST(CancelWaiting, CircleDuringAPartWaitStopsTheCaptureAndTheSeriesHearsNothing)
{
    host::FakeBackend backend;
    backend.set_capture_script({"priming", "priming", "priming", "completed"});
    Store store(backend);
    store.set_series_wait(0.0, 1000.0);
    backend.set_capture_hook(
        [&store](int reads)
        {
            if (reads == 1)
                store.cancel(Store::lane_link);
        });
    int calls = 0;
    store.start_link("pkg1", kAutomated, "", [&calls](const LinkStart &) { ++calls; });
    run_all(store);
    store.tick();
    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(store.link_action().busy);
    EXPECT_EQ(backend.cancelled_captures(), 1);
    EXPECT_FALSE(store.state().capture.active);
}

} // namespace
} // namespace orbit
