// Wings TV app - the Wings update helper client against a fake helper: the request shapes, the status JSON,
// starting the helper through the payload loader, and replacing a helper left by an older Wings.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_wings_helper.hpp"
#include "orbit/wings_helper_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace orbit
{
namespace
{

// The build the app expects is the helper's own stamp (helper/version.h), so the two cannot drift apart.
TEST(WingsHelperClient, ExpectedBuildIsTheHelpersVersionHeader)
{
    EXPECT_STREQ(kWingsHelperBuild, WINGS_HELPER_BUILD);
    EXPECT_EQ(kWingsHelperPort, WINGS_HELPER_PORT);
    EXPECT_EQ(kWingsHelperApi, WINGS_HELPER_API);
}

TEST(WingsHelperClient, StatusJsonDecodes)
{
    WingsHelperStatus s;
    ASSERT_TRUE(decode_wings_helper_status(
        "{\"helper\":\"1.0.0\",\"api\":1,\"phase\":\"downloading\",\"version\":\"v0.1.2\",\"received\":500,"
        "\"size\":1000,\"error\":\"\",\"target\":\"manual\",\"installed_version\":\"v0.1.1\",\"old_kept\":true}",
        &s));
    EXPECT_EQ(s.build, "1.0.0");
    EXPECT_EQ(s.phase, WingsHelperPhase::downloading);
    EXPECT_EQ(s.received, 500u);
    EXPECT_EQ(s.size, 1000u);
    EXPECT_EQ(s.target, WingsHelperTarget::manual);
    EXPECT_EQ(s.installed_version, "v0.1.1");
    EXPECT_TRUE(s.old_kept);
    EXPECT_TRUE(s.busy());
    EXPECT_FALSE(decode_wings_helper_status("{\"api\":1,\"phase\":\"dancing\"}", &s));
    EXPECT_FALSE(decode_wings_helper_status("{\"phase\":\"idle\"}", &s)); // no api
    EXPECT_FALSE(decode_wings_helper_status("not json", &s));
}

TEST(WingsHelperClient, RequestsCarryTheHeaderAndTheRoutes)
{
    const http::Request st = wings_helper_request("status");
    EXPECT_EQ(st.method, "GET");
    EXPECT_EQ(st.path, "/wings-helper/v1/status");
    EXPECT_EQ(st.timeout_ms, kWingsHelperProbeMs);
    EXPECT_NE(st.headers.find("X-Wings-Helper: 1\r\n"), std::string::npos);
    const http::Request in = wings_helper_request("install", "{}");
    EXPECT_EQ(in.method, "POST");
    EXPECT_EQ(in.path, "/wings-helper/v1/install");
    const std::string wire = http::format_request(in, "127.0.0.1", kWingsHelperPort);
    EXPECT_NE(wire.find("X-Wings-Helper: 1\r\n"), std::string::npos);
    EXPECT_NE(wire.find("Content-Type: application/json"), std::string::npos);

    WingsHelperInstall req;
    req.version = "v0.1.2";
    req.url = "https://github.com/LaRazaDelSol/wings-app/releases/download/v0.1.2/PPSA99178.ffpkg";
    req.sha256 = std::string(64, 'b');
    req.size = 70000;
    req.replace = true;
    req.running = "00.001.001";
    json::Value v;
    ASSERT_TRUE(json::parse(encode_wings_helper_install(req), &v));
    EXPECT_EQ(v.text("version"), "v0.1.2");
    EXPECT_EQ(v.text("url"), req.url);
    EXPECT_EQ(v.text("sha256"), req.sha256);
    EXPECT_EQ(v.number("size"), 70000.0);
    EXPECT_TRUE(v["replace"].as_bool());
    EXPECT_EQ(v.text("running"), "00.001.001");
    ASSERT_TRUE(json::parse(encode_wings_helper_tidy("00.001.002"), &v));
    EXPECT_EQ(v.text("running"), "00.001.002");
}

class HelperRig : public ::testing::Test
{
  protected:
    FakeWingsHelper fake_;
    WingsHelperClient client_{fake_, fake_.sender(), "/app0/assets/helper/wings-helper.elf", kWingsHelperBuild};
    int slept_ms_ = 0;
    void SetUp() override
    {
        client_.sleep_ms = [this](int ms) { slept_ms_ += ms; };
    }
};

TEST_F(HelperRig, ARunningHelperIsUsedAsItIs)
{
    fake_.running = true;
    WingsHelperStatus s;
    std::string why;
    ASSERT_TRUE(client_.ensure_running(&s, &why));
    EXPECT_TRUE(fake_.sent.empty());
    EXPECT_EQ(s.phase, WingsHelperPhase::idle);
}

TEST_F(HelperRig, NothingAnsweringSendsTheShippedElf)
{
    WingsHelperStatus s;
    std::string why;
    ASSERT_TRUE(client_.ensure_running(&s, &why)) << why;
    ASSERT_EQ(fake_.sent.size(), 1u);
    EXPECT_EQ(fake_.sent[0], "/app0/assets/helper/wings-helper.elf");
    EXPECT_EQ(s.build, kWingsHelperBuild);
}

TEST_F(HelperRig, ALoaderThatRefusesIsTriedAFewTimesThenSaysWhy)
{
    fake_.loader_up = false;
    WingsHelperStatus s;
    std::string why;
    EXPECT_FALSE(client_.ensure_running(&s, &why));
    EXPECT_EQ(fake_.sent.size(), static_cast<std::size_t>(kWingsHelperSendTries));
    EXPECT_EQ(why, "Connection refused");
}

TEST_F(HelperRig, AnOlderHelperIsStoppedAndReplaced)
{
    fake_.running = true;
    fake_.build = "0.9";
    WingsHelperStatus s;
    std::string why;
    ASSERT_TRUE(client_.ensure_running(&s, &why)) << why;
    EXPECT_NE(std::find(fake_.requests.begin(), fake_.requests.end(), "POST /wings-helper/v1/stop"),
              fake_.requests.end());
    EXPECT_EQ(fake_.sent.size(), 1u);
    EXPECT_EQ(s.build, kWingsHelperBuild);
    EXPECT_NE(client_.stopped_stale.find("helper=0.9"), std::string::npos);
}

TEST_F(HelperRig, AnOlderHelperPlacingFilesIsNeverStopped)
{
    fake_.running = true;
    fake_.build = "0.9";
    fake_.phase = "placing";
    fake_.frozen = true;
    WingsHelperStatus s;
    std::string why;
    EXPECT_FALSE(client_.ensure_running(&s, &why));
    EXPECT_FALSE(why.empty());
    EXPECT_TRUE(fake_.running);
    EXPECT_TRUE(fake_.sent.empty());
    EXPECT_EQ(std::find(fake_.requests.begin(), fake_.requests.end(), "POST /wings-helper/v1/stop"),
              fake_.requests.end());
}

// The console case of 2026-10-14: a helper left by the previous package (same api, other build) is still
// downloading. It is cancelled, stopped and replaced by the shipped one, which then answers idle.
TEST_F(HelperRig, AStaleHelperStillDownloadingIsCancelledStoppedAndSentAgain)
{
    fake_.running = true;
    fake_.build = "1.0.0";
    fake_.phase = "downloading";
    fake_.version = "0.1.2";
    fake_.size = 1000000;
    fake_.frozen = true;
    WingsHelperStatus s;
    std::string why;
    ASSERT_TRUE(client_.ensure_running(&s, &why)) << why;
    EXPECT_EQ(fake_.cancels, 1);
    EXPECT_EQ(fake_.sent.size(), 1u);
    EXPECT_EQ(s.build, kWingsHelperBuild);
    EXPECT_EQ(s.phase, WingsHelperPhase::idle);
    EXPECT_NE(client_.stopped_stale.find("phase=downloading version=0.1.2 helper=1.0.0"), std::string::npos);
}

TEST(WingsHelperClient, StatusLineNamesEveryField)
{
    WingsHelperStatus s;
    ASSERT_TRUE(decode_wings_helper_status(
        "{\"helper\":\"1.0.1\",\"api\":1,\"phase\":\"placed\",\"version\":\"0.1.2\",\"received\":7,\"size\":9,"
        "\"error\":\"\",\"target\":\"manual\",\"installed_version\":\"\",\"old_kept\":true}",
        &s));
    EXPECT_EQ(describe_wings_helper_status(s), "phase=placed version=0.1.2 helper=1.0.1 api=1 target=manual "
                                               "installed=- old_kept=yes bytes=7/9");
    EXPECT_STREQ(wings_helper_phase_name(WingsHelperPhase::verifying), "verifying");
    EXPECT_STREQ(wings_helper_phase_name(WingsHelperPhase::downloaded), "downloaded");
}

TEST(WingsHelperClient, TheDownloadedPhaseIsDecoded)
{
    WingsHelperStatus s;
    ASSERT_TRUE(decode_wings_helper_status(
        "{\"helper\":\"1.0.5\",\"api\":2,\"phase\":\"downloaded\",\"version\":\"0.1.2\",\"received\":9,\"size\":9,"
        "\"error\":\"\",\"target\":\"wings\",\"installed_version\":\"0.1.1\",\"old_kept\":false}",
        &s));
    EXPECT_EQ(s.phase, WingsHelperPhase::downloaded);
    EXPECT_EQ(s.version, "0.1.2");
    EXPECT_FALSE(s.busy()); // a finished download is no running job
    EXPECT_EQ(kWingsHelperApi, 2);
    EXPECT_STREQ(kWingsHelperBuild, WINGS_HELPER_BUILD);
}

// The fake speaks the shipped build and api, so the client takes it for the current helper.
TEST_F(HelperRig, TheFakeIsTheCurrentHelper)
{
    fake_.running = true;
    EXPECT_NE(fake_.body().find("\"api\":2"), std::string::npos);
    EXPECT_NE(fake_.body().find(std::string("\"helper\":\"") + WINGS_HELPER_BUILD + "\""), std::string::npos);
}

TEST_F(HelperRig, DownloadThenInstallAnswers)
{
    fake_.running = true;
    WingsHelperInstall req;
    req.version = "v0.1.2";
    req.size = 1000;
    req.url = "https://example.invalid/PPSA99178.ffpkg";
    std::string why;
    // Install before any download: the helper refuses with "not_downloaded", decoded as its own answer.
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::not_downloaded);
    EXPECT_EQ(why, "Download the update first.");
    EXPECT_TRUE(fake_.downloads.empty());
    EXPECT_EQ(client_.download(req, &why), WingsHelperClient::Answer::accepted);
    ASSERT_EQ(fake_.downloads.size(), 1u);
    EXPECT_NE(fake_.downloads[0].find("\"url\":\"https://example.invalid/PPSA99178.ffpkg\""), std::string::npos);
    EXPECT_NE(fake_.requests.back().find("/wings-helper/v1/download"), std::string::npos);
    EXPECT_EQ(client_.download(req, &why), WingsHelperClient::Answer::busy);
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::busy);
    fake_.received = fake_.size;
    fake_.phase = "verifying";
    fake_.advance(); // checked: downloaded
    EXPECT_EQ(fake_.phase, "downloaded");
    const std::size_t asked = fake_.downloads.size(); // the refused "busy" request above counts too
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::accepted);
    EXPECT_NE(fake_.requests.back().find("/wings-helper/v1/install"), std::string::npos);
    EXPECT_EQ(fake_.downloads.size(), asked); // install did not download
    fake_.phase = "idle";
    fake_.answer_download = 400;
    EXPECT_EQ(client_.download(req, &why), WingsHelperClient::Answer::refused);
    fake_.answer_download = 0;
}

TEST_F(HelperRig, InstallAnswers)
{
    fake_.running = true;
    fake_.downloaded_version = "0.1.2";
    fake_.phase = "downloaded";
    WingsHelperInstall req;
    req.version = "v0.1.2";
    req.size = 1000;
    std::string why;
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::accepted);
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::busy);
    fake_.phase = "idle";
    fake_.answer_install = 400;
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::refused);
    EXPECT_EQ(why, "url is not the official one");
    fake_.answer_install = 409; // "old_waiting" has its own answer (the app tidies, then asks again)
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::old_waiting);
    EXPECT_EQ(why, "an earlier update is waiting");
    fake_.running = false;
    EXPECT_EQ(client_.install(req, &why), WingsHelperClient::Answer::unreachable);
}

} // namespace
} // namespace orbit
