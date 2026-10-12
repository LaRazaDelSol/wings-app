// Wings TV app - After a download finishes: ShadowMount's local API client (config.ini, replies),
// the debounced scan, the banner states and the Downloads tab's two choices. No sockets: a fake
// transport answers, and the clock is passed in.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/prefs.hpp"
#include "orbit/screens.hpp"
#include "orbit/shadowmount_client.hpp"
#include "orbit/shadowmount_scan.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace orbit
{
namespace
{

http::Response reply(int status, const std::string &body)
{
    return test::json_reply(status, body);
}

// Forwards to the scripted transport so the test can look at what was sent.
class Forward final : public http::Transport
{
  public:
    explicit Forward(test::ScriptedTransport &target) : target_(target)
    {
    }
    http::Response send(const http::Request &request) override
    {
        return target_.send(request);
    }

  private:
    test::ScriptedTransport &target_;
};

struct FakeShadowMount
{
    test::ScriptedTransport transport;
    std::vector<int> ports;
    std::string ini;
    bool down = false;       // connection refused
    bool deferred = false;   // scan_deferred
    std::string reason;      // scan_deferred_reason
    bool queued = true;
    bool rescan_capability = true;
    std::set<std::string> games;

    FakeShadowMount()
    {
        transport.reply = [this](const http::Request &request)
        {
            if (down)
            {
                http::Response none;
                none.error = "connection refused";
                return none;
            }
            EXPECT_EQ(request.method, "POST");
            if (request.path == "/api/v1/version")
                return reply(200, std::string("{\"status\":0,\"api_version\":1,\"shadowmount_version\":\"1.7\","
                                              "\"capabilities\":[") +
                                      (rescan_capability ? "\"list_games\",\"rescan\"" : "\"list_games\"") + "]}");
            if (request.path == "/api/v1/scan")
                return reply(200, std::string("{\"status\":0,\"scan_queued\":") + (queued ? "true" : "false") +
                                      ",\"scan_deferred\":" + (deferred ? "true" : "false") +
                                      ",\"scan_deferred_reason\":\"" + reason + "\"}");
            if (request.path == "/api/v1/games")
            {
                std::string list;
                for (const std::string &id : games)
                    list += (list.empty() ? "" : ",") + std::string("{\"title_id\":\"") + id + "\"}";
                return reply(200, "{\"status\":0,\"count\":" + std::to_string(games.size()) + ",\"games\":[" +
                                      list + "]}");
            }
            return reply(404, "{\"status\":2,\"error\":\"no such route\"}");
        };
    }
    ShadowMountEnv env()
    {
        ShadowMountEnv out;
        out.transport = [this](std::uint16_t port, int)
        {
            ports.push_back(port);
            return std::unique_ptr<http::Transport>(new Forward(transport));
        };
        out.read_config = [this] { return ini; };
        return out;
    }
};

Job make_job(const std::string &id, const std::string &status, const std::string &title_id = "PPSA01234",
             const std::string &name = "Game")
{
    Job job;
    job.id = id;
    job.status = status;
    job.title_id = title_id;
    job.name = name;
    return job;
}

} // namespace

// ---- config.ini ----

TEST(ShadowMountConfig, DefaultsToTheDocumentedPort)
{
    const ShadowMountConfig config = parse_shadowmount_config("");
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.port, 10101);
}

TEST(ShadowMountConfig, ReadsThePortAndIgnoresCommentsAndSpacing)
{
    const ShadowMountConfig config =
        parse_shadowmount_config("; ShadowMount\r\ndebug=1\r\n  api_port = 12000  \r\n# api_port=1\r\n");
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.port, 12000);
}

TEST(ShadowMountConfig, ApiCanBeTurnedOff)
{
    EXPECT_FALSE(parse_shadowmount_config("api_enabled=0\napi_port=10101\n").enabled);
    EXPECT_FALSE(parse_shadowmount_config("api_port=10101\napi_enabled=0\n").enabled);
    EXPECT_TRUE(parse_shadowmount_config("api_enabled=1\n").enabled);
}

TEST(ShadowMountConfig, GarbageKeepsTheDefaultsAndABadPortMeansNoApi)
{
    const ShadowMountConfig junk = parse_shadowmount_config("\x01\x02 no equals\n=\n==\nfoo=bar\n");
    EXPECT_TRUE(junk.enabled);
    EXPECT_EQ(junk.port, 10101);
    EXPECT_FALSE(parse_shadowmount_config("api_port=abc\n").enabled);
    EXPECT_FALSE(parse_shadowmount_config("api_port=0\n").enabled);
    EXPECT_FALSE(parse_shadowmount_config("api_port=70000\n").enabled);
}

// ---- replies ----

TEST(ShadowMountReplies, ScanReasonsAreCarriedThrough)
{
    for (const char *reason : {"game_active", "runtime_prepared", "rest_mode"})
    {
        const ScanReply scan = parse_scan_reply(
            reply(200, std::string("{\"status\":0,\"scan_queued\":true,\"scan_deferred\":true,"
                                   "\"scan_deferred_reason\":\"") +
                           reason + "\"}"));
        EXPECT_TRUE(scan.ok);
        EXPECT_TRUE(scan.queued);
        EXPECT_TRUE(scan.deferred);
        EXPECT_EQ(scan.reason, reason);
    }
    const ScanReply now = parse_scan_reply(
        reply(200, "{\"status\":0,\"scan_queued\":true,\"scan_deferred\":false,\"scan_deferred_reason\":\"\"}"));
    EXPECT_TRUE(now.ok);
    EXPECT_FALSE(now.deferred);
    EXPECT_TRUE(now.reason.empty());
}

TEST(ShadowMountReplies, ErrorsAreNotSuccess)
{
    EXPECT_FALSE(parse_scan_reply(reply(409, "{\"status\":16,\"error\":\"busy\"}")).ok);
    EXPECT_EQ(parse_scan_reply(reply(409, "{\"status\":16,\"error\":\"busy\"}")).error, "busy");
    EXPECT_FALSE(parse_scan_reply(reply(200, "{\"status\":5}")).ok);
    EXPECT_FALSE(parse_scan_reply(reply(200, "not json")).ok);
    EXPECT_FALSE(parse_scan_reply(reply(200, "[1,2]")).ok);
    http::Response refused;
    refused.error = "connection refused";
    const ScanReply none = parse_scan_reply(refused);
    EXPECT_FALSE(none.reached);
    EXPECT_FALSE(none.ok);
}

TEST(ShadowMountReplies, VersionNeedsTheRescanCapabilityWhenListed)
{
    EXPECT_TRUE(parse_version_reply(reply(200, "{\"status\":0,\"capabilities\":[\"rescan\"]}")).ok);
    EXPECT_FALSE(parse_version_reply(reply(200, "{\"status\":0,\"capabilities\":[\"list_games\"]}")).ok);
    EXPECT_TRUE(parse_version_reply(reply(200, "{\"status\":0,\"shadowmount_version\":\"1.7\"}")).ok);
}

TEST(ShadowMountReplies, GamesYieldTitleIds)
{
    const GamesReply games = parse_games_reply(reply(
        200, "{\"status\":0,\"count\":2,\"games\":[{\"title_id\":\"PPSA01234\"},{\"title_id\":\"CUSA00001\"},{}]}"));
    ASSERT_TRUE(games.ok);
    EXPECT_EQ(games.title_ids, (std::set<std::string>{"PPSA01234", "CUSA00001"}));
    EXPECT_FALSE(parse_games_reply(reply(200, "{\"status\":0}")).ok);
}

// ---- the client ----

TEST(ShadowMountClient, PostsJsonToLoopbackOnTheConfiguredPort)
{
    FakeShadowMount api;
    api.ini = "api_port=12000\n";
    ShadowMountClient client(api.env());
    EXPECT_TRUE(client.version().ok);
    EXPECT_TRUE(client.scan().queued);
    ASSERT_EQ(api.transport.seen.size(), 2u);
    EXPECT_EQ(api.transport.seen[1].path, "/api/v1/scan");
    EXPECT_EQ(api.transport.seen[1].method, "POST");
    EXPECT_EQ(api.transport.seen[1].body, "{\"reset_attempts\":false}");
    EXPECT_EQ(api.ports, (std::vector<int>{12000, 12000}));
}

TEST(ShadowMountClient, ADisabledApiIsNeverCalled)
{
    FakeShadowMount api;
    api.ini = "api_enabled=0\n";
    ShadowMountClient client(api.env());
    EXPECT_FALSE(client.version().reached);
    EXPECT_FALSE(client.scan().ok);
    EXPECT_TRUE(api.transport.seen.empty());
    EXPECT_TRUE(api.ports.empty());
}

TEST(ShadowMountClient, WithoutATransportNothingIsSent)
{
    ShadowMountClient client{ShadowMountEnv{}};
    EXPECT_FALSE(client.wired());
    EXPECT_FALSE(client.scan().ok);
}

// ---- the debounced scan and the banner ----

TEST(AfterInstall, SeveralCompletionsBecomeOneScanAfterFiveSeconds)
{
    FakeShadowMount api;
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading"), make_job("b", "extracting", "PPSA00002")}, true);
    after.update(1.0, {make_job("a", "completed"), make_job("b", "extracting", "PPSA00002")}, true);
    EXPECT_TRUE(after.busy());
    after.update(3.0, {make_job("a", "completed"), make_job("b", "completed", "PPSA00002")}, true);
    after.update(5.9, {make_job("a", "completed"), make_job("b", "completed", "PPSA00002")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 0);
    after.update(6.0, {make_job("a", "completed"), make_job("b", "completed", "PPSA00002")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/version"), 1);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 1);
    // Nothing more, whatever the queue keeps saying.
    for (int i = 0; i < 20; ++i)
        after.update(7.0 + i, {make_job("a", "completed"), make_job("b", "completed", "PPSA00002")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 1);
    EXPECT_EQ(after.scans_sent(), 1);
}

TEST(AfterInstall, AScanThatIsDeferredAsksToCloseWings)
{
    FakeShadowMount api;
    api.deferred = true;
    api.reason = "game_active";
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed")}, true);
    EXPECT_FALSE(after.banner().visible()); // still debouncing
    after.update(6.0, {make_job("a", "completed")}, true);
    EXPECT_EQ(after.banner().phase, ScanPhase::deferred);
    EXPECT_TRUE(after.banner().visible());
    EXPECT_TRUE(after.banner().offers_close());
    EXPECT_EQ(after.banner().reason, "game_active");
    EXPECT_EQ(scan_banner_text(after.banner()),
              "Asked ShadowMount to find your new game. It waits while Wings is open. Close Wings to finish.");
    // A deferred scan is not polled for games.
    after.update(30.0, {make_job("a", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/games"), 0);
}

TEST(AfterInstall, ANotDeferredScanPollsUntilTheTitleIsRegistered)
{
    FakeShadowMount api;
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading", "PPSA01234", "Game A")}, true);
    after.update(1.0, {make_job("a", "completed", "PPSA01234", "Game A")}, true);
    after.update(6.0, {make_job("a", "completed", "PPSA01234", "Game A")}, true);
    EXPECT_EQ(after.banner().phase, ScanPhase::scanning);
    EXPECT_FALSE(after.banner().offers_close());
    EXPECT_EQ(scan_banner_text(after.banner()), "ShadowMount is scanning now.");
    after.update(10.9, {make_job("a", "completed", "PPSA01234", "Game A")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/games"), 0);
    after.update(11.0, {make_job("a", "completed", "PPSA01234", "Game A")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/games"), 1);
    EXPECT_EQ(after.banner().phase, ScanPhase::scanning);
    api.games.insert("PPSA01234");
    after.update(16.0, {make_job("a", "completed", "PPSA01234", "Game A")}, true);
    EXPECT_EQ(after.banner().phase, ScanPhase::registered);
    EXPECT_EQ(scan_banner_text(after.banner()), "Registered: Game A");
    // Done: no further polling.
    after.update(30.0, {make_job("a", "completed", "PPSA01234", "Game A")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/games"), 2);
}

TEST(AfterInstall, PollingStopsAfterSixtySecondsAndOffersToClose)
{
    FakeShadowMount api;
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed")}, true);
    after.update(6.0, {make_job("a", "completed")}, true);
    for (double t = 7.0; t <= 80.0; t += 1.0)
        after.update(t, {make_job("a", "completed")}, true);
    EXPECT_EQ(after.banner().phase, ScanPhase::timed_out);
    EXPECT_TRUE(after.banner().offers_close());
    EXPECT_LE(api.transport.count("/api/v1/games"), 12);
}

TEST(AfterInstall, AnUnreachableApiGivesTheCalmLine)
{
    FakeShadowMount api;
    api.down = true;
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed")}, true);
    after.update(6.0, {make_job("a", "completed")}, true);
    EXPECT_EQ(after.banner().phase, ScanPhase::unreachable);
    EXPECT_TRUE(after.banner().offers_close());
    EXPECT_EQ(scan_banner_text(after.banner()),
              "ShadowMount's API was not found. Close Wings and ShadowMount will find the game on its next "
              "scan.");
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 0); // the probe failed first
}

TEST(AfterInstall, ADisabledApiAlsoGivesTheCalmLine)
{
    FakeShadowMount api;
    api.ini = "api_enabled=0\n";
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed")}, true);
    after.update(6.0, {make_job("a", "completed")}, true);
    EXPECT_EQ(after.banner().phase, ScanPhase::unreachable);
    EXPECT_TRUE(api.transport.seen.empty());
}

TEST(AfterInstall, TheSettingOffMeansNoRequestUnlessAskedByHand)
{
    FakeShadowMount api;
    AfterInstall after(api.env());
    after.set_mode(AfterDownload::nothing);
    after.update(0.0, {make_job("a", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed")}, true);
    for (double t = 2.0; t < 40.0; t += 2.0)
        after.update(t, {make_job("a", "completed")}, true);
    EXPECT_TRUE(api.transport.seen.empty());
    EXPECT_FALSE(after.banner().visible());
    after.scan_now(41.0);
    after.update(41.0, {make_job("a", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 1);
    EXPECT_EQ(after.banner().phase, ScanPhase::scanning);
}

TEST(AfterInstall, JobsAlreadyFinishedAtStartOrAppearingFinishedDoNotScan)
{
    FakeShadowMount api;
    AfterInstall after(api.env());
    after.update(0.0, {make_job("old", "completed")}, true);
    // A reconnect lists old finished jobs under ids never seen unfinished.
    after.update(10.0, {make_job("old", "completed"), make_job("older", "completed")}, true);
    after.update(30.0, {make_job("old", "completed"), make_job("older", "completed")}, true);
    EXPECT_TRUE(api.transport.seen.empty());
}

TEST(AfterInstall, ADismissedBannerStaysGoneAndTheSameCompletionIsNeverRepeated)
{
    FakeShadowMount api;
    api.deferred = true;
    api.reason = "game_active";
    AfterInstall after(api.env());
    after.update(0.0, {make_job("a", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed")}, true);
    after.update(6.0, {make_job("a", "completed")}, true);
    ASSERT_TRUE(after.banner().visible());
    after.dismiss();
    EXPECT_FALSE(after.banner().visible());
    // The job goes away and comes back (a queue refresh): still the same completion.
    after.update(10.0, {}, true);
    after.update(12.0, {make_job("a", "downloading")}, true);
    after.update(13.0, {make_job("a", "completed")}, true);
    after.update(30.0, {make_job("a", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 1);
    EXPECT_FALSE(after.banner().visible());
    // A different download is a new completion.
    after.update(40.0, {make_job("b", "downloading")}, true);
    after.update(41.0, {make_job("b", "completed")}, true);
    after.update(47.0, {make_job("b", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 2);
}

TEST(AfterInstall, CompletionsWhileTheRequestIsOutGetTheirOwnRequest)
{
    FakeShadowMount api;
    ShadowMountEnv env = api.env();
    std::function<void()> held;
    env.run = [&held](std::function<void()> job) { held = std::move(job); };
    AfterInstall after(std::move(env));
    after.update(0.0, {make_job("a", "downloading"), make_job("b", "downloading")}, true);
    after.update(1.0, {make_job("a", "completed"), make_job("b", "downloading")}, true);
    after.update(6.0, {make_job("a", "completed"), make_job("b", "downloading")}, true);
    ASSERT_TRUE(static_cast<bool>(held)); // request out, not yet answered
    after.update(7.0, {make_job("a", "completed"), make_job("b", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 0);
    held();
    held = nullptr;
    after.update(8.0, {make_job("a", "completed"), make_job("b", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 1);
    after.update(13.0, {make_job("a", "completed"), make_job("b", "completed")}, true);
    ASSERT_TRUE(static_cast<bool>(held));
    held();
    after.update(14.0, {make_job("a", "completed"), make_job("b", "completed")}, true);
    EXPECT_EQ(api.transport.count("/api/v1/scan"), 2);
}

// ---- the setting ----

TEST(AfterDownloadPref, DefaultsToScanAndRoundTrips)
{
    WingsLocalPrefs prefs;
    EXPECT_EQ(prefs.after_download, AfterDownload::scan);
    prefs.after_download = AfterDownload::nothing;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_EQ(back.after_download, AfterDownload::nothing);
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("region=USA\n", &old));
    EXPECT_EQ(old.after_download, AfterDownload::scan);
    ASSERT_TRUE(decode_local_prefs("after_download=banana\n", &old));
    EXPECT_EQ(old.after_download, AfterDownload::scan);
}

// ---- the Downloads tab ----

namespace
{

struct BannerHarness
{
    test::TestFonts fonts;
    host::FakeBackend backend{host::FakeMode::normal};
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    FakeShadowMount api;
    std::unique_ptr<App> app;
    Feedback feedback;
    int closed = 0;

    BannerHarness()
    {
        context.app_version = "00.001.000";
        context.shadowmount = api.env();
        context.close_app = [this] { ++closed; };
        app = std::make_unique<App>(context);
        store.poll_once();
        store.load_catalog_once();
        app->show_tab(tab::downloads);
        frame();
        frame();
    }
    void frame(Direction nav = Direction::none, std::uint32_t press = 0)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = press;
        feedback.clear();
        app->update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app->draw(drawn);
    }
    void press(Action action)
    {
        frame(Direction::none, hui::action_bit(action));
    }
    void show(ScanPhase phase)
    {
        ScanBanner banner;
        banner.phase = phase;
        context.after_install->debug_show(banner);
        frame();
    }
};

} // namespace

TEST(DownloadsBanner, UpFromTheFirstRowReachesTheBannerAndCloseAsksFirst)
{
    BannerHarness h;
    DownloadsScreen &downloads = h.app->downloads_screen();
    EXPECT_FALSE(downloads.banner_focused());
    h.show(ScanPhase::deferred);
    ASSERT_EQ(downloads.index(), 0);
    h.frame(Direction::up);
    EXPECT_TRUE(downloads.banner_focused());
    EXPECT_EQ(downloads.banner_button(), 0); // "Close Wings now"
    h.press(Action::confirm);
    EXPECT_TRUE(downloads.modal()); // the question first
    EXPECT_EQ(h.closed, 0);
    h.press(Action::back); // "Not now" in the dialog
    EXPECT_EQ(h.closed, 0);
    EXPECT_TRUE(h.context.after_install->banner().visible());
    h.press(Action::confirm);
    h.press(Action::confirm); // the dialog's confirm
    EXPECT_EQ(h.closed, 1);
}

TEST(DownloadsBanner, NotNowDismissesAndNothingClosesWings)
{
    BannerHarness h;
    h.show(ScanPhase::unreachable);
    h.frame(Direction::up);
    h.frame(Direction::right);
    EXPECT_EQ(h.app->downloads_screen().banner_button(), 1);
    h.press(Action::confirm);
    EXPECT_FALSE(h.context.after_install->banner().visible());
    EXPECT_FALSE(h.app->downloads_screen().banner_focused());
    EXPECT_EQ(h.closed, 0);
}

TEST(DownloadsBanner, ScanningHasOnlyADismissButtonAndUpGoesToTheTopBar)
{
    BannerHarness h;
    h.show(ScanPhase::scanning);
    h.frame(Direction::up);
    ASSERT_TRUE(h.app->downloads_screen().banner_focused());
    h.frame(Direction::right); // only one button
    EXPECT_EQ(h.app->downloads_screen().banner_button(), 0);
    h.frame(Direction::up);
    EXPECT_TRUE(h.app->top_bar_focused());
    EXPECT_EQ(h.closed, 0);
}

TEST(DownloadsBanner, ActionsMenuOffersAScanOnlyWhereTheApiIsWired)
{
    BannerHarness h;
    h.press(Action::confirm);
    const auto &codes = h.app->downloads_screen().menu_codes();
    ASSERT_FALSE(codes.empty());
    EXPECT_EQ(codes.back(), "scan");
}

} // namespace orbit
