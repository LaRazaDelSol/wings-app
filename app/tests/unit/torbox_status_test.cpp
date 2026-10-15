// Wings TV app - TorBox "still preparing" status and retry wait (torbox_status.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/torbox_status.hpp"

#include <gtest/gtest.h>

#include <string>

namespace orbit
{
namespace
{

using Kind = TorboxPrepareWait::Tick::Kind;

constexpr const char *kPegasus = "TorBox is still preparing this download; try again shortly";

TEST(TorboxStatus, MatchesPegasusWording)
{
    EXPECT_TRUE(is_torbox_preparing(kPegasus));
    EXPECT_TRUE(is_torbox_preparing("HTTP 502: TorBox is still preparing this download; try again shortly"));
    EXPECT_TRUE(is_torbox_preparing("TORBOX IS STILL PREPARING THIS DOWNLOAD"));
    EXPECT_TRUE(is_torbox_preparing("torbox is still preparing the file, try again later"));
    EXPECT_TRUE(is_torbox_preparing("TorBox: download is still being prepared"));
}

TEST(TorboxStatus, IgnoresOtherErrors)
{
    for (const char *e : {"TorBox rate limit reached", "TorBox returned HTTP 503",
                          "TorBox did not return downloadable files", "Download host is not supported by TorBox",
                          "TorBox token is not configured", "TorBox selection expired",
                          "Real-Debrid is still preparing this download", "still preparing", "Route not found", ""})
        EXPECT_FALSE(is_torbox_preparing(e)) << e;
}

TEST(TorboxStatus, RetriesEvery30Seconds)
{
    TorboxPrepareWait w;
    EXPECT_EQ(w.tick(0.0).kind, Kind::idle);
    w.start(100.0);
    EXPECT_TRUE(w.active());
    TorboxPrepareWait::Tick t = w.tick(110.0);
    EXPECT_EQ(t.kind, Kind::waiting);
    EXPECT_EQ(t.seconds_left, 20);
    EXPECT_EQ(w.tick(129.5).seconds_left, 1);
    EXPECT_EQ(w.tick(130.0).kind, Kind::retry_now);
    EXPECT_EQ(w.attempts(), 1);
    EXPECT_EQ(w.tick(131.0).kind, Kind::in_flight); // only one retry per wait
    w.start(140.0);                                  // still preparing: wait again from the answer
    EXPECT_EQ(w.tick(140.0).seconds_left, 30);
    EXPECT_EQ(w.tick(170.0).kind, Kind::retry_now);
    EXPECT_EQ(w.attempts(), 2);
}

TEST(TorboxStatus, GivesUpAfterTenMinutes)
{
    TorboxPrepareWait w;
    w.start(0.0);
    double now = 0.0;
    int retries = 0;
    for (;;)
    {
        now += 1.0;
        const TorboxPrepareWait::Tick t = w.tick(now);
        if (t.kind == Kind::retry_now)
        {
            ++retries;
            w.start(now + 2.0); // Pegasus answers "still preparing" again
        }
        if (t.kind == Kind::gave_up || now > 2000.0)
            break;
    }
    EXPECT_GE(now, TorboxPrepareWait::kGiveUpAfter);
    EXPECT_LT(now, TorboxPrepareWait::kGiveUpAfter + 40.0);
    EXPECT_GE(retries, 15);
    EXPECT_LE(retries, 20);
    EXPECT_FALSE(w.active());
    EXPECT_EQ(w.tick(now + 1.0).kind, Kind::idle); // reported once
}

TEST(TorboxStatus, CancelStopsAtOnce)
{
    TorboxPrepareWait w;
    w.start(0.0);
    w.cancel();
    EXPECT_FALSE(w.active());
    EXPECT_EQ(w.tick(30.0).kind, Kind::idle);
    w.start(1000.0); // a new link: a fresh 10 minutes
    EXPECT_EQ(w.tick(1010.0).kind, Kind::waiting);
    EXPECT_EQ(w.attempts(), 0);
}

TEST(TorboxStatus, Texts)
{
    EXPECT_EQ(torbox_waiting_text(20),
              "TorBox is getting this file ready on its server\xE2\x80\xA6 trying again in 20 s");
    EXPECT_NE(torbox_waiting_text(0).find("trying again now"), std::string::npos);
    EXPECT_EQ(torbox_gave_up_text(),
              "TorBox is still preparing this file. Try again later, or open TorBox to check it.");
    EXPECT_EQ(torbox_card_label(), "Preparing on TorBox");
    EXPECT_EQ(torbox_preparing_title(), "TorBox is still preparing this file");
    EXPECT_EQ(std::string(kTorboxDashboardUrl), "https://torbox.app/dashboard");
}

} // namespace
} // namespace orbit
