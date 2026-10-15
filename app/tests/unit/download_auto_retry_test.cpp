// Wings TV app - Auto-reconnect of dropped downloads (download_auto_retry.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/download_auto_retry.hpp"
#include "orbit/prefs.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

RetryJobSnapshot snap(const std::string &status, double received, const std::string &error = "",
                      bool resumable = true)
{
    RetryJobSnapshot s;
    s.id = "7";
    s.status = status;
    s.received = received;
    s.error = error;
    s.resumable = resumable;
    return s;
}

constexpr const char *kReset = "Segmented download failed: Failure when receiving data from the peer";

using Kind = DownloadAutoRetry::View::Kind;

// The job runs, then drops at `at` seconds with `error`.
void run_then_fail(DownloadAutoRetry &m, double at, double bytes, const std::string &error = kReset)
{
    EXPECT_TRUE(m.observe({snap("downloading", bytes)}, at - 1.0).empty());
    EXPECT_TRUE(m.observe({snap("failed", bytes, error)}, at).empty());
}

TEST(DownloadAutoRetry, DroppedConnectionsAndBusyServersAreTransient)
{
    EXPECT_TRUE(transient_download_error(kReset, ""));
    EXPECT_TRUE(transient_download_error("Connection died, retrying a fresh connect", ""));
    EXPECT_TRUE(transient_download_error("transfer closed with 1234 bytes remaining to read", ""));
    EXPECT_TRUE(transient_download_error("Operation timed out after 30000 milliseconds with 0 bytes received", ""));
    EXPECT_TRUE(transient_download_error("Recv failure: Connection reset by peer", ""));
    EXPECT_TRUE(transient_download_error("remote download failed: torbox status=503 Service Unavailable", ""));
    EXPECT_TRUE(transient_download_error("HTTP server returned status 429", ""));
    EXPECT_TRUE(transient_download_error("HTTP server returned status 502", ""));
    EXPECT_TRUE(transient_download_error("", "Transferred a partial file"));
    EXPECT_EQ(download_error_http_code("remote download failed: x status=504 Gateway Timeout"), 504);
    EXPECT_EQ(download_error_http_code("HTTP/1.1 206 Partial Content"), 206);
    EXPECT_EQ(download_error_http_code("1500 bytes"), 0);
}

TEST(DownloadAutoRetry, RealErrorsAreNotTransient)
{
    EXPECT_FALSE(transient_download_error("remote download failed: torbox status=404 Not Found", ""));
    EXPECT_FALSE(transient_download_error("HTTP server returned status 403", ""));
    EXPECT_FALSE(transient_download_error("Disk full or allocation exceeded", ""));
    EXPECT_FALSE(transient_download_error("Not enough free space to finish this download.", ""));
    EXPECT_FALSE(transient_download_error("Remote file size changed during download", ""));
    EXPECT_FALSE(transient_download_error("ETag mismatch", ""));
    EXPECT_FALSE(transient_download_error("Server returned an unexpected byte range", ""));
    EXPECT_FALSE(transient_download_error("Download cancelled", ""));
    EXPECT_FALSE(transient_download_error("Archive has too many installable candidates", ""));
    EXPECT_FALSE(transient_download_error("", ""));
    EXPECT_FALSE(transient_download_error("Something odd", ""));
}

TEST(DownloadAutoRetry, BacksOffTenTwentyFortySeconds)
{
    EXPECT_EQ(DownloadAutoRetry::backoff_seconds(1), 10.0);
    EXPECT_EQ(DownloadAutoRetry::backoff_seconds(2), 20.0);
    EXPECT_EQ(DownloadAutoRetry::backoff_seconds(3), 40.0);
}

TEST(DownloadAutoRetry, WaitsThenRetriesAndShowsTheCountdown)
{
    DownloadAutoRetry m;
    run_then_fail(m, 100.0, 500.0);
    DownloadAutoRetry::View v = m.view("7", 102.0);
    EXPECT_EQ(v.kind, Kind::waiting);
    EXPECT_EQ(v.seconds, 8);
    EXPECT_EQ(v.attempt, 1);
    EXPECT_EQ(auto_retry_text(v), "Reconnecting in 8 s (try 1 of 3)");
    EXPECT_TRUE(m.observe({snap("failed", 500.0, kReset)}, 109.0).empty());
    const std::vector<std::string> due = m.observe({snap("failed", 500.0, kReset)}, 110.0);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0], "7");
    m.sent("7", true, 110.0);
    EXPECT_EQ(m.view("7", 110.0).kind, Kind::reconnecting);
    // Resumed from the partial file and moving again.
    m.observe({snap("downloading", 500.0)}, 111.0);
    EXPECT_EQ(m.view("7", 111.0).kind, Kind::reconnecting);
    m.observe({snap("downloading", 900.0)}, 112.0);
    EXPECT_EQ(auto_retry_text(m.view("7", 113.0)), "Reconnected");
    EXPECT_EQ(m.view("7", 112.0 + DownloadAutoRetry::kReconnectedShown + 0.1).kind, Kind::none);
    bool logged = false;
    for (const std::string &line : m.take_log())
        logged = logged || line.rfind("[WINGS-RETRY] ", 0) == 0;
    EXPECT_TRUE(logged);
}

TEST(DownloadAutoRetry, StopsAfterThreeTriesWithoutProgress)
{
    DownloadAutoRetry m;
    run_then_fail(m, 100.0, 500.0);
    double t = 100.0;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        EXPECT_EQ(m.view("7", t).attempt, attempt);
        t += DownloadAutoRetry::backoff_seconds(attempt);
        ASSERT_EQ(m.observe({snap("failed", 500.0, kReset)}, t).size(), 1u);
        m.sent("7", true, t);
        m.observe({snap("queued", 500.0)}, t + 1.0); // back in the queue, no new bytes
        t += 2.0;
        m.observe({snap("failed", 500.0, kReset)}, t);
    }
    // Fourth drop without progress: it stays failed.
    EXPECT_EQ(m.view("7", t).kind, Kind::none);
    EXPECT_TRUE(m.observe({snap("failed", 500.0, kReset)}, t + 100.0).empty());
}

TEST(DownloadAutoRetry, NewBytesResetTheCountSoLongDownloadsKeepGoing)
{
    DownloadAutoRetry m;
    double t = 100.0;
    double bytes = 0.0;
    for (int drop = 0; drop < 6; ++drop)
    {
        bytes += 1000.0;
        run_then_fail(m, t, bytes);
        EXPECT_EQ(m.view("7", t).attempt, 1) << "drop " << drop;
        t += 10.0;
        ASSERT_EQ(m.observe({snap("failed", bytes, kReset)}, t).size(), 1u) << "drop " << drop;
        m.sent("7", true, t);
        m.observe({snap("downloading", bytes + 10.0)}, t + 1.0);
        t += 50.0;
    }
}

TEST(DownloadAutoRetry, RealErrorsAndUnresumableJobsAreLeftFailed)
{
    DownloadAutoRetry m;
    run_then_fail(m, 100.0, 500.0, "Disk full or allocation exceeded");
    EXPECT_EQ(m.view("7", 100.0).kind, Kind::none);
    EXPECT_TRUE(m.observe({snap("failed", 500.0, "Disk full or allocation exceeded")}, 200.0).empty());

    DownloadAutoRetry n;
    n.observe({snap("downloading", 10.0)}, 1.0);
    n.observe({snap("failed", 10.0, kReset, false)}, 2.0);
    EXPECT_EQ(n.view("7", 2.0).kind, Kind::none);
}

TEST(DownloadAutoRetry, PausedOrCancelledJobsAreNeverRetried)
{
    for (const char *status : {"paused", "cancelled", "cancelling", "pausing"})
    {
        DownloadAutoRetry m;
        run_then_fail(m, 100.0, 500.0);
        ASSERT_EQ(m.view("7", 100.0).kind, Kind::waiting);
        m.observe({snap(status, 500.0, kReset)}, 101.0);
        EXPECT_EQ(m.view("7", 101.0).kind, Kind::none) << status;
        EXPECT_TRUE(m.observe({snap(status, 500.0, kReset)}, 200.0).empty()) << status;
    }
}

TEST(DownloadAutoRetry, OldFailuresTheSwitchAndStopAreRespected)
{
    // Failed before Wings ever saw it running (e.g. from an earlier session).
    DownloadAutoRetry m;
    EXPECT_TRUE(m.observe({snap("failed", 500.0, kReset)}, 1.0).empty());
    EXPECT_EQ(m.view("7", 1.0).kind, Kind::none);

    DownloadAutoRetry off;
    off.set_enabled(false);
    run_then_fail(off, 100.0, 500.0);
    EXPECT_EQ(off.view("7", 100.0).kind, Kind::none);

    DownloadAutoRetry stop;
    run_then_fail(stop, 100.0, 500.0);
    stop.stop("7");
    EXPECT_EQ(stop.view("7", 100.0).kind, Kind::none);
    EXPECT_TRUE(stop.observe({snap("failed", 500.0, kReset)}, 200.0).empty());
}

TEST(DownloadAutoRetry, ThePreferenceIsOnByDefaultAndSaved)
{
    WingsLocalPrefs prefs;
    EXPECT_TRUE(prefs.auto_reconnect);
    prefs.auto_reconnect = false;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_FALSE(back.auto_reconnect);
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("firmware=\n", &old));
    EXPECT_TRUE(old.auto_reconnect);
}

} // namespace
} // namespace orbit
