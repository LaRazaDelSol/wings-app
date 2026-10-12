// Wings TV app - The first-run "load Pegasus DL" flow: grace period, one automatic send per launch,
// loader refusal, health cancels, mode persistence, offline, release choice. No network, no sockets.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/i18n.hpp"
#include "orbit/pegasus_load_flow.hpp"
#include "orbit/prefs.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace orbit
{
namespace
{

LoadFacts facts(double now, bool health, bool net = true, bool cached = true)
{
    LoadFacts f;
    f.now = now;
    f.health_ok = health;
    f.net_ok = net;
    f.cached = cached;
    return f;
}

PegasusRelease release(const std::string &tag, bool pre = false, bool downloadable = true)
{
    PegasusRelease r;
    r.tag = tag;
    r.prerelease = pre;
    r.downloadable = downloadable;
    r.size = 100;
    r.sha256 = std::string(64, 'a');
    return r;
}

TEST(PegasusLoadFlow, NothingIsOfferedAtFirstPaintOnlyAfterTheGracePeriod)
{
    PegasusLoadFlow flow;
    EXPECT_EQ(flow.tick(facts(0.0, false)), LoadCommand::none);
    EXPECT_FALSE(flow.offering());
    EXPECT_EQ(flow.state(), LoadState::grace);
    flow.tick(facts(kPegasusOfferGraceS - 0.1, false));
    EXPECT_FALSE(flow.offering());
    flow.tick(facts(kPegasusOfferGraceS, false));
    EXPECT_TRUE(flow.offering());
    EXPECT_EQ(flow.state(), LoadState::offering);
}

TEST(PegasusLoadFlow, ChoicesCanBePressedDuringTheGraceAndStartTheLoad)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    EXPECT_FALSE(flow.offering());
    EXPECT_TRUE(flow.choosable());
    EXPECT_EQ(flow.choose(LoadChoice::once, facts(2.0, false, true, false)), LoadCommand::begin_load);
    EXPECT_EQ(flow.state(), LoadState::downloading);
    EXPECT_FALSE(flow.choosable());
    // Health answering during that early load stops it quietly.
    EXPECT_EQ(flow.tick(facts(3.0, true)), LoadCommand::cancel);
    EXPECT_EQ(flow.state(), LoadState::idle);
    EXPECT_FALSE(flow.sent_this_launch());
}

TEST(PegasusLoadFlow, GracePressOfEnableTurnsAutoOnAndAutoDoesNotSendAgain)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    ASSERT_EQ(flow.choose(LoadChoice::enable_auto, facts(1.0, false)), LoadCommand::begin_load);
    EXPECT_TRUE(flow.take_mode_change());
    flow.on_sending();
    flow.on_sent(5.0);
    EXPECT_EQ(flow.tick(facts(20.0, false)), LoadCommand::none); // the grace ending sends nothing
    EXPECT_EQ(flow.state(), LoadState::waiting_for_pegasus);
}

TEST(PegasusLoadFlow, ManualDuringTheGraceStopsTheOffer)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    EXPECT_EQ(flow.choose(LoadChoice::manual, facts(1.0, false)), LoadCommand::none);
    EXPECT_FALSE(flow.choosable());
    EXPECT_EQ(flow.tick(facts(20.0, false)), LoadCommand::none);
    EXPECT_FALSE(flow.offering());
}

TEST(PegasusLoadFlow, AutoModeStillWaitsTheGraceBeforeSending)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    EXPECT_EQ(flow.tick(facts(0.0, false)), LoadCommand::none);
    EXPECT_TRUE(flow.choosable());
    EXPECT_EQ(flow.tick(facts(kPegasusOfferGraceS - 0.1, false)), LoadCommand::none);
    EXPECT_EQ(flow.tick(facts(kPegasusOfferGraceS, false)), LoadCommand::begin_load);
}

TEST(PegasusLoadFlow, ABreakInTheOutageRestartsTheGrace)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(8.0, false));
    flow.tick(facts(9.0, true)); // answered for a moment
    EXPECT_EQ(flow.state(), LoadState::idle);
    flow.tick(facts(9.5, false));
    flow.tick(facts(12.0, false)); // 2.5 s into the new outage
    EXPECT_FALSE(flow.offering());
    flow.tick(facts(14.6, false)); // it had answered: reported as stopped, not offered as first-run
    EXPECT_EQ(flow.state(), LoadState::dropped);
}

TEST(PegasusLoadFlow, HealthAnsweringHidesTheOfferAndRefusesChoices)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    ASSERT_TRUE(flow.offering());
    EXPECT_EQ(flow.tick(facts(12.0, true)), LoadCommand::none);
    EXPECT_FALSE(flow.offering());
    EXPECT_EQ(flow.choose(LoadChoice::once, facts(12.0, true)), LoadCommand::none);
}

TEST(PegasusLoadFlow, LoadOnceBeginsAndKeepsTheMode)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    EXPECT_EQ(flow.choose(LoadChoice::once, facts(11.0, false, true, false)), LoadCommand::begin_load);
    EXPECT_EQ(flow.state(), LoadState::downloading);
    EXPECT_EQ(flow.mode(), PegasusStartMode::ask);
    EXPECT_FALSE(flow.take_mode_change());
    flow.on_verifying();
    EXPECT_EQ(flow.state(), LoadState::verifying);
    flow.on_sending();
    EXPECT_EQ(flow.state(), LoadState::sending);
    flow.on_sent(20.0);
    EXPECT_EQ(flow.state(), LoadState::waiting_for_pegasus);
    EXPECT_TRUE(flow.sent_this_launch());
    EXPECT_EQ(flow.tick(facts(25.0, true)), LoadCommand::none);
    EXPECT_EQ(flow.state(), LoadState::done);
}

TEST(PegasusLoadFlow, EnableSetsAutoOnceAndAsksForOneNotice)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    EXPECT_EQ(flow.choose(LoadChoice::enable_auto, facts(11.0, false)), LoadCommand::begin_load);
    EXPECT_EQ(flow.mode(), PegasusStartMode::autoload);
    EXPECT_TRUE(flow.take_mode_change());
    EXPECT_FALSE(flow.take_mode_change());
    EXPECT_TRUE(flow.take_auto_notice());
    EXPECT_FALSE(flow.take_auto_notice());
    EXPECT_EQ(flow.state(), LoadState::verifying); // cached: only checked and sent
}

TEST(PegasusLoadFlow, ManualStopsOfferingAndPersists)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    EXPECT_EQ(flow.choose(LoadChoice::manual, facts(11.0, false)), LoadCommand::none);
    EXPECT_EQ(flow.mode(), PegasusStartMode::manual);
    EXPECT_TRUE(flow.take_mode_change());
    EXPECT_FALSE(flow.offering());
    flow.tick(facts(40.0, false));
    EXPECT_FALSE(flow.offering());
    EXPECT_EQ(flow.state(), LoadState::idle);
}

TEST(PegasusLoadFlow, AutoModeSendsOncePerLaunchAfterTheGrace)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    EXPECT_EQ(flow.tick(facts(0.0, false)), LoadCommand::none);
    EXPECT_EQ(flow.tick(facts(9.0, false)), LoadCommand::none);
    EXPECT_EQ(flow.tick(facts(10.0, false)), LoadCommand::begin_load);
    EXPECT_EQ(flow.tick(facts(10.1, false)), LoadCommand::none); // not twice
    flow.on_sending();
    flow.on_sent(11.0);
    // Pegasus dies later: only state is reported, nothing is sent or offered.
    flow.tick(facts(20.0, true));
    EXPECT_EQ(flow.tick(facts(30.0, false)), LoadCommand::none);
    EXPECT_EQ(flow.tick(facts(60.0, false)), LoadCommand::none);
    // Dropped: reported, never resent by itself.
    EXPECT_EQ(flow.state(), LoadState::dropped);
}

TEST(PegasusLoadFlow, AutoModeNeverSendsWhileHealthAnswers)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    for (double t = 0.0; t < 60.0; t += 1.0)
        EXPECT_EQ(flow.tick(facts(t, true)), LoadCommand::none);
    EXPECT_EQ(flow.state(), LoadState::idle);
    EXPECT_FALSE(flow.sent_this_launch());
}

TEST(PegasusLoadFlow, LoaderRefusalRetriesEveryThreeSecondsAndGivesUpAfterThirty)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    flow.tick(facts(0.0, false));
    ASSERT_EQ(flow.tick(facts(10.0, false)), LoadCommand::begin_load);
    flow.on_sending();
    flow.on_loader_refused(10.0);
    EXPECT_EQ(flow.state(), LoadState::loader_wait);
    EXPECT_FALSE(flow.choosable());
    EXPECT_EQ(flow.tick(facts(12.9, false)), LoadCommand::none);
    EXPECT_EQ(flow.tick(facts(13.0, false)), LoadCommand::begin_load);
    EXPECT_TRUE(flow.retry_run());
    EXPECT_TRUE(flow.busy());
    flow.on_sending();
    int retries = 1;
    double t = 13.0;
    while (true)
    {
        flow.on_loader_refused(t);
        if (flow.state() == LoadState::failed)
            break;
        ASSERT_EQ(flow.state(), LoadState::loader_wait);
        t += 3.0;
        ASSERT_EQ(flow.tick(facts(t, false)), LoadCommand::begin_load);
        flow.on_sending();
        ++retries;
        ASSERT_LT(retries, 20);
    }
    EXPECT_GE(t - 10.0, kPegasusLoaderWindowS);
    EXPECT_LE(t - 10.0, kPegasusLoaderWindowS + kPegasusLoaderRetryS);
    EXPECT_EQ(flow.fail(), LoadFail::no_loader);
    EXPECT_TRUE(flow.retryable());
    // Auto mode does not start again by itself.
    EXPECT_EQ(flow.tick(facts(t + 20.0, false)), LoadCommand::none);
    EXPECT_EQ(flow.state(), LoadState::failed);
}

TEST(PegasusLoadFlow, ExplicitRetryResetsTheWindowAndIsAllowedAfterTheOnePerLaunchSend)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    flow.tick(facts(0.0, false));
    ASSERT_EQ(flow.tick(facts(10.0, false)), LoadCommand::begin_load); // the launch's one automatic send
    flow.on_sending();
    flow.on_loader_refused(10.0);
    flow.tick(facts(13.0, false));
    flow.on_sending();
    flow.on_loader_refused(41.0); // window spent
    ASSERT_EQ(flow.state(), LoadState::failed);
    EXPECT_EQ(flow.choose(LoadChoice::retry, facts(42.0, false)), LoadCommand::begin_load);
    EXPECT_TRUE(flow.busy());
    flow.on_sending();
    flow.on_loader_refused(42.5); // a new window starts here, not at the old first refusal
    EXPECT_EQ(flow.state(), LoadState::loader_wait);
    flow.tick(facts(45.5, false));
    flow.on_sending();
    flow.on_sent(46.0);
    EXPECT_TRUE(flow.sent_this_launch());
    EXPECT_EQ(flow.state(), LoadState::waiting_for_pegasus);
}

TEST(PegasusLoadFlow, TheGrayChoiceOfAFailureIsSessionOnly)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    ASSERT_EQ(flow.choose(LoadChoice::once, facts(11.0, false)), LoadCommand::begin_load);
    flow.on_failed(LoadFail::send);
    ASSERT_TRUE(flow.retryable());
    EXPECT_EQ(flow.choose(LoadChoice::manual, facts(12.0, false)), LoadCommand::none);
    EXPECT_FALSE(flow.take_mode_change());
    EXPECT_EQ(flow.mode(), PegasusStartMode::ask);
    // Nothing is offered again this session while Pegasus stays away.
    flow.tick(facts(13.0, false));
    flow.tick(facts(60.0, false));
    EXPECT_EQ(flow.state(), LoadState::idle);
    EXPECT_FALSE(flow.choosable());
    // It is offered afresh after Pegasus answered and went away again.
    flow.tick(facts(61.0, true));
    flow.tick(facts(62.0, false));
    flow.tick(facts(70.0, false));
    EXPECT_EQ(flow.state(), LoadState::dropped);
}

TEST(PegasusLoadFlow, TheGrayChoiceOnTheFirstScreenStillPersistsManual)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    flow.choose(LoadChoice::manual, facts(11.0, false));
    EXPECT_TRUE(flow.take_mode_change());
    EXPECT_EQ(flow.mode(), PegasusStartMode::manual);
}

TEST(PegasusLoadFlow, DroppedAfterBeingConnectedIsDebouncedAndNeverResendsByItself)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    flow.tick(facts(0.0, true));
    EXPECT_EQ(flow.tick(facts(100.0, false)), LoadCommand::none);
    EXPECT_NE(flow.state(), LoadState::dropped);
    EXPECT_FALSE(flow.choosable()); // no first-run choices for a drop
    EXPECT_EQ(flow.tick(facts(100.0 + kPegasusDroppedS - 0.1, false)), LoadCommand::none);
    EXPECT_NE(flow.state(), LoadState::dropped);
    EXPECT_EQ(flow.tick(facts(100.0 + kPegasusDroppedS, false)), LoadCommand::none);
    EXPECT_EQ(flow.state(), LoadState::dropped);
    EXPECT_TRUE(flow.retryable());
    // Even long after the grace period, auto mode does not send.
    for (double t = 110.0; t < 200.0; t += 5.0)
        EXPECT_EQ(flow.tick(facts(t, false)), LoadCommand::none);
    EXPECT_EQ(flow.state(), LoadState::dropped);
    // Only Retry sends, then Pegasus answering returns the screen to normal.
    EXPECT_EQ(flow.choose(LoadChoice::retry, facts(201.0, false)), LoadCommand::begin_load);
    flow.on_sending();
    flow.on_sent(202.0);
    flow.tick(facts(210.0, true));
    EXPECT_EQ(flow.state(), LoadState::done);
}

TEST(PegasusLoadFlow, AnswerDuringDroppedReturnsToNormal)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, true));
    flow.tick(facts(10.0, false));
    flow.tick(facts(16.0, false));
    ASSERT_EQ(flow.state(), LoadState::dropped);
    flow.tick(facts(17.0, true));
    EXPECT_EQ(flow.state(), LoadState::idle);
    EXPECT_FALSE(flow.choosable());
}

TEST(PegasusLoadFlow, HealthAnsweringWhileDownloadingCancelsTheJob)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    ASSERT_EQ(flow.choose(LoadChoice::once, facts(11.0, false, true, false)), LoadCommand::begin_load);
    EXPECT_EQ(flow.tick(facts(13.0, true)), LoadCommand::cancel);
    EXPECT_EQ(flow.state(), LoadState::idle);
    flow.on_failed(LoadFail::download); // a late failure from the cancelled job is ignored
    EXPECT_EQ(flow.state(), LoadState::idle);
    EXPECT_FALSE(flow.sent_this_launch());
}

TEST(PegasusLoadFlow, OfflineWithNothingCachedOffersOnlyTheManualChoice)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false, false, false));
    flow.tick(facts(11.0, false, false, false));
    ASSERT_TRUE(flow.offering());
    EXPECT_TRUE(flow.needs_internet());
    EXPECT_TRUE(flow.manual_only());
    EXPECT_EQ(flow.choose(LoadChoice::once, facts(11.0, false, false, false)), LoadCommand::none);
    EXPECT_EQ(flow.choose(LoadChoice::enable_auto, facts(11.0, false, false, false)), LoadCommand::none);
    EXPECT_EQ(flow.mode(), PegasusStartMode::ask);
    // The network comes back: the choices return.
    flow.tick(facts(12.0, false, true, false));
    EXPECT_FALSE(flow.manual_only());
}

TEST(PegasusLoadFlow, AutoModeOfflineWithNothingCachedDoesNotTryToSend)
{
    PegasusLoadFlow flow(PegasusStartMode::autoload);
    flow.tick(facts(0.0, false, false, false));
    EXPECT_EQ(flow.tick(facts(12.0, false, false, false)), LoadCommand::none);
    EXPECT_TRUE(flow.needs_internet());
    EXPECT_TRUE(flow.offering());
}

TEST(PegasusLoadFlow, AWaitingSendTimesOutIntoAMessageWithoutResending)
{
    PegasusLoadFlow flow;
    flow.tick(facts(0.0, false));
    flow.tick(facts(11.0, false));
    ASSERT_EQ(flow.choose(LoadChoice::once, facts(11.0, false)), LoadCommand::begin_load);
    flow.on_sent(12.0);
    EXPECT_EQ(flow.tick(facts(12.0 + kPegasusAnswerWaitS, false)), LoadCommand::none);
    EXPECT_EQ(flow.state(), LoadState::failed);
    EXPECT_EQ(flow.fail(), LoadFail::no_answer);
}

TEST(PegasusLoadFlow, ModeIsPersistedInThePrefsFile)
{
    WingsLocalPrefs prefs;
    EXPECT_EQ(prefs.pegasus_start, PegasusStartMode::ask);
    prefs.pegasus_start = PegasusStartMode::autoload;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_EQ(back.pegasus_start, PegasusStartMode::autoload);
    prefs.pegasus_start = PegasusStartMode::manual;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_EQ(back.pegasus_start, PegasusStartMode::manual);
    // A file from before this choice, or with an unknown word, keeps the default.
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("region=USA\npegasus_start=banana\n", &old));
    EXPECT_EQ(old.pegasus_start, PegasusStartMode::ask);
}

TEST(PegasusRelease, PicksTheNewestInsideTheTestedRangeElseNewestWithTheNotice)
{
    bool untested = true;
    std::vector<PegasusRelease> list = {release("v1.12.0"), release("v1.11.0"), release("v1.10.0")};
    const PegasusRelease *r = pick_pegasus_release(list, &untested);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->tag, "v1.11.0"); // the tested range is 1.11.0
    EXPECT_FALSE(untested);
    list = {release("v1.12.0"), release("v1.13.0", true), release("v1.10.0")};
    r = pick_pegasus_release(list, &untested);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->tag, "v1.12.0"); // the pre-release is skipped
    EXPECT_TRUE(untested);
    list = {release("v1.11.0", false, false)};
    EXPECT_EQ(pick_pegasus_release(list, &untested), nullptr);
}

TEST(PegasusRelease, AutoModeKeepsTheNewestCachedReleaseAndNeverUpgrades)
{
    const std::vector<PegasusRelease> list = {release("v1.12.0"), release("v1.11.0"), release("v1.10.0")};
    const PegasusRelease *r =
        pick_cached_pegasus_release(list, [](const std::string &tag) { return tag == "v1.10.0"; });
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->tag, "v1.10.0");
    EXPECT_EQ(pick_cached_pegasus_release(list, [](const std::string &) { return false; }), nullptr);
}

TEST(PegasusSha256File, HashesFromDiskInChunks)
{
    const std::string path = std::string(::testing::TempDir()) + "wings_p3_sha.bin";
    std::string body(200000, 'x');
    body[0] = 0x7f;
    body[1] = 'E';
    body[2] = 'L';
    body[3] = 'F';
    FILE *f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    std::string hex;
    std::uint64_t size = 0;
    bool elf = false;
    ASSERT_TRUE(sha256_file(path, &hex, &size, &elf));
    EXPECT_EQ(size, body.size());
    EXPECT_TRUE(elf);
    EXPECT_EQ(hex, sha256_hex(body));
    std::remove(path.c_str());
    EXPECT_FALSE(sha256_file(path, &hex, &size, &elf));
}

} // namespace
} // namespace orbit
