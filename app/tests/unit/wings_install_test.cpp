// Wings TV app - fetching and placing a newer Wings package: the release asset parser, the checksum text, and
// the install state machine against a fake checksum downloader and a fake Wings update helper
// (fake_wings_helper.hpp). No test touches the network or the real /data.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_wings_helper.hpp"
#include "orbit/i18n.hpp"
#include "orbit/update_screen.hpp"
#include "orbit/json.hpp"
#include "orbit/wings_helper_client.hpp"
#include "orbit/wings_install.hpp"
#include "orbit/wings_update.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

namespace fs = std::filesystem;

constexpr const char *kTag = "v0.1.2";

void spit(const std::string &p, const std::string &data)
{
    std::ofstream f(p, std::ios::binary);
    f << data;
}

// ---- the release JSON ----
std::string release_json(const std::string &tag, std::uint64_t size, const std::string &sha,
                         bool with_digest_asset = true, const std::string &asset_url = std::string(),
                         bool prerelease = false, const std::string &digest_field = std::string("\x01"))
{
    const std::string base = "https://github.com/LaRazaDelSol/wings-app/releases/download/" + tag + "/";
    const std::string url = asset_url.empty() ? base + "PPSA99178.ffpkg" : asset_url;
    const std::string digest = digest_field == "\x01" ? "sha256:" + sha : digest_field;
    std::string j = "{\"tag_name\":\"" + tag + "\",\"draft\":false,\"prerelease\":" +
                    (prerelease ? "true" : "false") + ",\"assets\":[{\"name\":\"PPSA99178.ffpkg\",\"size\":" +
                    std::to_string(size) + ",\"digest\":\"" + digest + "\",\"browser_download_url\":\"" + url +
                    "\"}";
    if (with_digest_asset)
        j += ",{\"name\":\"PPSA99178.ffpkg.sha256\",\"size\":90,\"digest\":\"sha256:" + std::string(64, 'a') +
             "\",\"browser_download_url\":\"" + base + "PPSA99178.ffpkg.sha256\"}";
    j += "]}";
    return j;
}

const std::string kSha(64, 'b');

// ---- parser ----
TEST(WingsAssetParse, ReadsSizeDigestAndOfficialAddresses)
{
    WingsRelease r;
    std::string why;
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 54460416, kSha), &r, &why));
    EXPECT_EQ(r.tag, kTag);
    EXPECT_TRUE(r.asset.downloadable);
    EXPECT_EQ(r.asset.size, 54460416u);
    EXPECT_EQ(r.asset.sha256, kSha);
    EXPECT_EQ(r.asset.url, "https://github.com/LaRazaDelSol/wings-app/releases/download/v0.1.2/PPSA99178.ffpkg");
    EXPECT_EQ(r.asset.digest_url,
              "https://github.com/LaRazaDelSol/wings-app/releases/download/v0.1.2/PPSA99178.ffpkg.sha256");
    std::string tag;
    ASSERT_TRUE(parse_wings_latest(release_json(kTag, 54460416, kSha), &tag, &why));
    EXPECT_EQ(tag, kTag);
}

TEST(WingsAssetParse, UpperCaseDigestIsAccepted)
{
    std::string upper = kSha;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    WingsRelease r;
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 100000, "", true, "", false, "SHA256:" + upper), &r, nullptr));
    EXPECT_TRUE(r.asset.downloadable);
    EXPECT_EQ(r.asset.sha256, kSha);
}

TEST(WingsAssetParse, MissingPackageIsNotDownloadable)
{
    WingsRelease r;
    ASSERT_TRUE(parse_wings_release(R"({"tag_name":"v0.1.2","assets":[{"name":"other.zip","size":10}]})", &r,
                                    nullptr));
    EXPECT_FALSE(r.asset.downloadable);
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::missing);
    EXPECT_FALSE(wings_asset_issue_text(r.asset.issue).empty());
    ASSERT_TRUE(parse_wings_release(R"({"tag_name":"v0.1.2"})", &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::missing);
}

TEST(WingsAssetParse, MissingChecksumAssetIsNotDownloadable)
{
    WingsRelease r;
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 54460416, kSha, false), &r, nullptr));
    EXPECT_FALSE(r.asset.downloadable);
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::no_checksum);
    EXPECT_EQ(r.asset.size, 0u); // nothing half-trusted is kept
    EXPECT_TRUE(r.asset.sha256.empty());
}

TEST(WingsAssetParse, BadDigestSizeAndAddressAreRefused)
{
    WingsRelease r;
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 54460416, "", true, "", false, "md5:abcd"), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::no_digest);
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 54460416, "", true, "", false, ""), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::no_digest);
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 54460416, std::string(63, 'a')), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::no_digest);
    // Oversize (more than the 256 MiB limit), empty and too small to hold an image.
    ASSERT_TRUE(parse_wings_release(release_json(kTag, kStreamMaxImage + 1, kSha), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::bad_size);
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 0, kSha), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::bad_size);
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 4096, kSha), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::bad_size);
    ASSERT_TRUE(parse_wings_release(release_json(kTag, kStreamMaxImage, kSha), &r, nullptr));
    EXPECT_TRUE(r.asset.downloadable);
    // An address that is not this tag's official one is never followed.
    ASSERT_TRUE(parse_wings_release(
        release_json(kTag, 54460416, kSha, true, "https://example.invalid/PPSA99178.ffpkg"), &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::bad_url);
    ASSERT_TRUE(parse_wings_release(
        release_json(kTag, 54460416, kSha, true,
                     "https://github.com/LaRazaDelSol/wings-app/releases/download/v9.9.9/PPSA99178.ffpkg"),
        &r, nullptr));
    EXPECT_EQ(r.asset.issue, WingsAssetIssue::bad_url);
}

TEST(WingsAssetParse, PreReleaseAndDraftAreIgnored)
{
    WingsRelease r;
    std::string why;
    EXPECT_FALSE(parse_wings_release(release_json(kTag, 54460416, kSha, true, "", true), &r, &why));
    EXPECT_FALSE(parse_wings_release(
        R"({"tag_name":"v0.1.2","draft":true,"assets":[]})", &r, &why));
    EXPECT_FALSE(parse_wings_release("[]", &r, &why));
    EXPECT_FALSE(parse_wings_release(R"({"tag_name":"latest"})", &r, &why));
}

TEST(WingsAssetParse, UrlRules)
{
    EXPECT_EQ(wings_asset_url("v0.1.2"),
              "https://github.com/LaRazaDelSol/wings-app/releases/download/v0.1.2/PPSA99178.ffpkg");
    EXPECT_EQ(wings_asset_url("v0.1.2/../x"), "");
    EXPECT_EQ(wings_asset_url(""), "");
    EXPECT_TRUE(wings_url_allowed(wings_asset_url("v0.1.2"), false, "v0.1.2"));
    EXPECT_FALSE(wings_url_allowed(wings_asset_url("v0.1.2"), false, "v0.1.3"));
    EXPECT_FALSE(wings_url_allowed("https://evil.example/PPSA99178.ffpkg", false, "v0.1.2"));
    EXPECT_TRUE(wings_url_allowed("https://objects.githubusercontent.com/x", true, ""));
    EXPECT_TRUE(wings_url_allowed("https://release-assets.githubusercontent.com/x", true, ""));
    EXPECT_FALSE(wings_url_allowed("https://evil.example/x", true, ""));
    EXPECT_FALSE(wings_url_allowed("http://github.com/x", true, ""));
}

TEST(WingsAssetParse, ChecksumText)
{
    std::string hex;
    const std::string h(64, 'c');
    EXPECT_TRUE(parse_wings_checksum_text(h + "  PPSA99178.ffpkg\n", &hex));
    EXPECT_EQ(hex, h);
    EXPECT_TRUE(parse_wings_checksum_text(h + " *PPSA99178.ffpkg\r\n", &hex));
    EXPECT_TRUE(parse_wings_checksum_text(h, &hex));
    EXPECT_TRUE(parse_wings_checksum_text("\n" + std::string(64, 'D') + "\n", &hex));
    EXPECT_EQ(hex, std::string(64, 'd'));
    EXPECT_FALSE(parse_wings_checksum_text(h + "  other.bin\n", &hex));
    EXPECT_FALSE(parse_wings_checksum_text(h.substr(1) + "\n", &hex));
    EXPECT_FALSE(parse_wings_checksum_text(h + "zz  PPSA99178.ffpkg", &hex));
    EXPECT_FALSE(parse_wings_checksum_text("", &hex));
    EXPECT_FALSE(parse_wings_checksum_text("<html>not found</html>", &hex));
}

TEST(WingsAssetParse, SavedCopyKeepsTheAssetAndRebuildsAddresses)
{
    WingsRelease r;
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 54460416, kSha), &r, nullptr));
    const std::string payload = encode_wings_cache(1234567890, r.tag, r.asset);
    std::int64_t at = 0;
    std::string tag;
    WingsAsset a;
    ASSERT_TRUE(decode_wings_cache(payload, &at, &tag, &a));
    EXPECT_EQ(tag, kTag);
    EXPECT_TRUE(a.downloadable);
    EXPECT_EQ(a.size, 54460416u);
    EXPECT_EQ(a.sha256, kSha);
    EXPECT_EQ(a.url, r.asset.url);
    // An old copy (tag only) or a damaged digest has no package.
    ASSERT_TRUE(decode_wings_cache(encode_wings_cache(5, kTag), &at, &tag, &a));
    EXPECT_FALSE(a.downloadable);
    ASSERT_TRUE(decode_wings_cache("checked=5\ntag=v0.1.2\nsize=100000\nsha256=zz\n", &at, &tag, &a));
    EXPECT_FALSE(a.downloadable);
    EXPECT_TRUE(decode_wings_cache(payload, &at, &tag)); // the older signature still works
}

// What the checker says about release_ while the old version runs (the screen feeds this to the snapshot).
WingsUpdateStatus newer_status(const WingsRelease &release)
{
    WingsUpdateStatus up;
    up.attached = true;
    up.have_latest = true;
    up.latest_tag = release.tag;
    up.asset = release.asset;
    up.latest_text = "0.1.2";
    up.running_text = "0.1.1";
    up.advice = WingsAdvice::newer;
    return up;
}

// ---- fakes ----
// The published checksum text (the only thing the app itself downloads now).
class FakeNet final : public StreamingDownloader
{
  public:
    bool ready(std::string *why) override
    {
        if (!up)
            *why = "The network is off.";
        return up;
    }
    std::string get_stream(const std::string &url, std::uint64_t, const StreamHooks &hooks,
                           const std::function<bool(const std::uint8_t *, std::size_t)> &sink,
                           int *status) override
    {
        urls.push_back(url);
        *status = digest_status;
        if (*status < 200 || *status > 299)
            return {};
        if (hooks.cancel && hooks.cancel())
            return "cancelled";
        if (!sink(reinterpret_cast<const std::uint8_t *>(digest_text.data()), digest_text.size()))
            return "sink";
        return {};
    }
    bool up = true;
    std::string digest_text;
    int digest_status = 200;
    std::vector<std::string> urls;
};

class WingsInstallRig : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = (fs::temp_directory_path() / ("wings-inst-" + std::to_string(::getpid()) + "-" +
                                              ::testing::UnitTest::GetInstance()->current_test_info()->name()))
                    .string();
        fs::remove_all(root_);
        fs::create_directories(root_); // like /download0 on the console: the parent always exists
        note_dir_ = root_ + "/wings";
        sha_ = std::string(64, 'c');
        net_.digest_text = sha_ + "  PPSA99178.ffpkg\n";
        ASSERT_TRUE(parse_wings_release(release_json(kTag, kSize, sha_), &release_, nullptr));
        client_.sleep_ms = [](int) {};
    }
    void TearDown() override
    {
        installer_.wait_idle();
        fs::remove_all(root_);
    }
    void attach(const std::string &running = "00.001.001", bool with_helper = true)
    {
        WingsInstallConfig cfg;
        cfg.downloader = &net_;
        cfg.helper = with_helper ? &client_ : nullptr;
        cfg.note_dir = note_dir_;
        cfg.running = running;
        cfg.sleep_ms = [this](int) { if (on_poll) on_poll(); };
        cfg.log = [this](const std::string &line) { log_.push_back(line); };
        cfg.clock = [this] { return clock_ += 1.0; };
        installer_.attach(std::move(cfg));
    }
    WingsInstallStatus download()
    {
        installer_.run_download(release_);
        return installer_.status();
    }
    WingsInstallStatus install(bool confirm)
    {
        installer_.run_install(confirm);
        return installer_.status();
    }
    bool note_exists() const
    {
        return fs::exists(note_dir_ + "/placed.v1");
    }
    static constexpr std::uint64_t kSize = 70000;
    std::string root_, note_dir_, sha_;
    FakeNet net_;
    FakeWingsHelper fake_;
    WingsHelperClient client_{fake_, fake_.sender(), "/app0/assets/helper/wings-helper.elf", kWingsHelperBuild};
    WingsInstaller installer_;
    WingsRelease release_;
    std::function<void()> on_poll;
    double clock_ = 100.0;
    std::vector<std::string> log_;
    bool logged(const std::string &part) const
    {
        return std::any_of(log_.begin(), log_.end(),
                           [&](const std::string &l) { return l.find(part) != std::string::npos; });
    }
};

TEST(WingsHelperSituation, EachHelperStateHasItsOwnMeaning)
{
    WingsHelperStatus s;
    s.phase = WingsHelperPhase::idle;
    s.target = WingsHelperTarget::none;
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::ready);
    s.phase = WingsHelperPhase::error; // a failed job is over: a new install may be asked for
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::ready);
    s.phase = WingsHelperPhase::downloading;
    s.version = "0.1.2"; // the helper drops the tag's "v"
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::following);
    s.phase = WingsHelperPhase::placing;
    s.version = "0.1.1";
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::other_job);
    s.version.clear(); // busy without a version is still another job, never "following"
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::other_job);
    s.phase = WingsHelperPhase::placed;
    s.version = "0.1.2";
    s.target = WingsHelperTarget::wings;
    s.installed_version = "0.1.2";
    s.old_kept = true;
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::already_placed);
    s.target = WingsHelperTarget::manual; // the placed file was replaced by hand since
    s.installed_version.clear();
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::old_waiting);
    s.old_kept = false;
    EXPECT_EQ(wings_helper_situation(s, "v0.1.2"), WingsHelperSituation::ready);
    EXPECT_TRUE(wings_same_release("0.1.2", "v0.1.2"));
    EXPECT_TRUE(wings_same_release("v0.1.2", "v0.1.2"));
    EXPECT_FALSE(wings_same_release("0.1.2", "v0.1.20"));
    EXPECT_FALSE(wings_same_release("", "v0.1.2"));
}

TEST_F(WingsInstallRig, DownloadChecksAndWaitsForTheConfirm)
{
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::verified);
    EXPECT_EQ(s.target, WingsTarget::none);
    EXPECT_FALSE(s.needs_replace_confirm);
    ASSERT_EQ(net_.urls.size(), 1u);
    EXPECT_EQ(net_.urls[0], release_.asset.digest_url);
    EXPECT_EQ(fake_.sent.size(), 1u);      // the helper was started...
    EXPECT_EQ(fake_.downloads.size(), 1u); // ...downloaded and checked the package...
    EXPECT_EQ(fake_.phase, "downloaded");
    EXPECT_EQ(fake_.downloaded_version, "0.1.2");
    EXPECT_EQ(s.got_bytes, kSize);
    EXPECT_TRUE(fake_.installs.empty()); // ...but nothing is installed before the confirm
    const WingsFlowSnapshot snap = wings_flow_snapshot(newer_status(release_), s);
    EXPECT_EQ(snap.state, WingsFlowState::ready_to_install);
    EXPECT_EQ(snap.step, WingsFlowStep::install);
}

// The whole flow on the helper's routes: download -> downloaded -> install -> placed, each route once.
TEST_F(WingsInstallRig, DownloadThenInstallUseTheirOwnRoutes)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    const auto count = [this](const std::string &route)
    {
        return std::count_if(fake_.requests.begin(), fake_.requests.end(), [&](const std::string &r)
                             { return r.size() > route.size() && r.compare(r.size() - route.size(), route.size(), route) == 0; });
    };
    EXPECT_EQ(count("/download"), 1);
    EXPECT_EQ(count("/install"), 0);
    const std::size_t before_install = fake_.requests.size();
    const WingsInstallStatus s = install(false);
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
    EXPECT_EQ(count("/download"), 1); // Install never calls the download route
    EXPECT_EQ(count("/install"), 1);
    for (std::size_t i = before_install; i < fake_.requests.size(); ++i)
        EXPECT_EQ(fake_.requests[i].find("/download"), std::string::npos) << fake_.requests[i];
    EXPECT_EQ(fake_.phase, "placed");
    EXPECT_TRUE(fake_.downloaded_version.empty());
}

// The helper already holds a checked download of this release (kept across a helper restart): the Download step
// reuses it and Install places it, with no second download.
TEST_F(WingsInstallRig, AHeldDownloadOfThisReleaseResumesAtInstall)
{
    fake_.downloaded_version = "0.1.2";
    fake_.size = kSize;
    attach(); // the helper is started and reports "downloaded" for 0.1.2
    WingsInstallStatus s = download();
    ASSERT_EQ(s.phase, WingsInstallPhase::verified) << s.message;
    EXPECT_TRUE(fake_.downloads.empty());
    EXPECT_EQ(wings_flow_snapshot(newer_status(release_), s).step, WingsFlowStep::install);
    s = install(false);
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
    EXPECT_TRUE(fake_.downloads.empty());
    EXPECT_EQ(fake_.installs.size(), 1u);
}

// A held download of another release is not reused: this one is downloaded.
TEST_F(WingsInstallRig, AHeldDownloadOfAnotherReleaseIsNotReused)
{
    fake_.downloaded_version = "0.1.1";
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    EXPECT_EQ(fake_.downloads.size(), 1u);
    EXPECT_EQ(fake_.downloaded_version, "0.1.2");
}

// The helper lost its download (or never had one): Install says so and sends the player back to Download.
TEST_F(WingsInstallRig, InstallWithoutADownloadIsSentBackToDownload)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    fake_.downloaded_version.clear();
    fake_.phase = "idle";
    const WingsInstallStatus s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("Download the update first"), std::string::npos);
    EXPECT_EQ(fake_.downloads.size(), 1u); // Install did not download
    EXPECT_FALSE(s.install_tried);
    EXPECT_EQ(wings_flow_snapshot(newer_status(release_), s).step, WingsFlowStep::download);
}

TEST_F(WingsInstallRig, InstallFollowsTheHelperUntilPlaced)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    const WingsInstallStatus s = install(false);
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
    EXPECT_EQ(s.placed_by, WingsPlacedBy::helper);
    EXPECT_TRUE(s.old_kept);
    ASSERT_EQ(fake_.installs.size(), 1u);
    json::Value v;
    ASSERT_TRUE(json::parse(fake_.installs[0], &v));
    EXPECT_EQ(v.text("version"), kTag);
    EXPECT_EQ(v.text("url"), release_.asset.url);
    EXPECT_EQ(v.text("sha256"), sha_);
    EXPECT_EQ(v.number("size"), static_cast<double>(kSize));
    EXPECT_FALSE(v["replace"].as_bool(true));
    EXPECT_EQ(v.text("running"), "00.001.001");
    EXPECT_TRUE(note_exists());
}

TEST_F(WingsInstallRig, PlayerPlacedCopyIsOnlyReplacedAfterTheQuestionIsAnswered)
{
    fake_.target = "manual";
    attach();
    WingsInstallStatus s = download();
    ASSERT_EQ(s.phase, WingsInstallPhase::verified);
    EXPECT_EQ(s.target, WingsTarget::own_copy);
    EXPECT_TRUE(s.needs_replace_confirm);
    s = install(false);
    EXPECT_EQ(s.phase, WingsInstallPhase::verified);
    EXPECT_TRUE(fake_.installs.empty());
    s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
    ASSERT_EQ(fake_.installs.size(), 1u);
    EXPECT_NE(fake_.installs[0].find("\"replace\":true"), std::string::npos);
}

TEST_F(WingsInstallRig, TheTwoPublishedChecksumsMustAgree)
{
    net_.digest_text = std::string(64, 'd') + "  PPSA99178.ffpkg\n";
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_TRUE(fake_.requests.empty()); // the helper is never involved
}

TEST_F(WingsInstallRig, AnUnreadableChecksumStopsBeforeTheHelper)
{
    net_.digest_status = 404;
    attach();
    EXPECT_EQ(download().phase, WingsInstallPhase::failed);
    EXPECT_TRUE(fake_.requests.empty());
}

TEST_F(WingsInstallRig, FolderCopyBlocksBeforeAnythingIsFetched)
{
    fake_.target = "folder";
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::blocked);
    EXPECT_EQ(s.target, WingsTarget::folder);
    EXPECT_FALSE(installer_.install(true));
    EXPECT_TRUE(fake_.installs.empty());
    const std::vector<WingsInstallStep> steps = wings_install_manual_steps(s);
    ASSERT_FALSE(steps.empty());
    EXPECT_NE(steps[0].detail.find("/data/homebrew/PPSA99178"), std::string::npos);
}

TEST_F(WingsInstallRig, SomethingElseInTheWayBlocks)
{
    fake_.target = "blocked";
    attach();
    EXPECT_EQ(download().phase, WingsInstallPhase::blocked);
    EXPECT_TRUE(fake_.installs.empty());
}

TEST_F(WingsInstallRig, NoHelperShowsTheManualSteps)
{
    fake_.loader_up = false;
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::manual);
    EXPECT_NE(s.message.find("Connection refused"), std::string::npos);
    const WingsFlowSnapshot snap = wings_flow_snapshot(newer_status(release_), s);
    EXPECT_EQ(snap.state, WingsFlowState::manual);
    bool names_url = false;
    for (const WingsInstallStep &step : snap.manual_steps)
        names_url = names_url || step.detail == release_.asset.url;
    EXPECT_TRUE(names_url);

    attach("00.001.001", false); // a build without the helper: the same steps
    EXPECT_EQ(download().phase, WingsInstallPhase::manual);
}

TEST_F(WingsInstallRig, AFailedDownloadCheckIsAFailureOnTheDownloadStep)
{
    fake_.fail_with = "the download failed its SHA-256 check";
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("SHA-256"), std::string::npos);
    EXPECT_TRUE(fake_.installs.empty());
    const WingsFlowSnapshot snap = wings_flow_snapshot(newer_status(release_), s);
    EXPECT_EQ(snap.state, WingsFlowState::failed);
    EXPECT_EQ(snap.step, WingsFlowStep::download);
    EXPECT_TRUE(snap.can_retry);
}

TEST_F(WingsInstallRig, AHelperErrorIsAFailureOnTheInstallStep)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    fake_.fail_with = "the download failed its SHA-256 check"; // the second check, before placing
    const WingsInstallStatus s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("SHA-256"), std::string::npos);
    EXPECT_FALSE(note_exists());
    const WingsFlowSnapshot snap = wings_flow_snapshot(newer_status(release_), s);
    EXPECT_EQ(snap.state, WingsFlowState::failed);
    EXPECT_EQ(snap.step, WingsFlowStep::install);
    EXPECT_TRUE(snap.can_retry);
}

TEST_F(WingsInstallRig, ARefusedInstallSaysWhy)
{
    fake_.answer_install = 400;
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    const WingsInstallStatus s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("official"), std::string::npos);
}

TEST_F(WingsInstallRig, CancelAsksTheHelperToStop)
{
    attach();
    on_poll = [this]
    {
        if (fake_.received > 0 && fake_.phase == "downloading")
            installer_.cancel();
    };
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::idle);
    EXPECT_NE(s.message.find("cancelled"), std::string::npos);
    EXPECT_EQ(fake_.cancels, 1);
    EXPECT_TRUE(fake_.installs.empty());
    EXPECT_FALSE(note_exists());
}

TEST_F(WingsInstallRig, AHelperThatStopsAnsweringIsAFailure)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    fake_.silent_after = fake_.status_reads + 1; // placing takes few reads: go quiet right after the start
    const WingsInstallStatus s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("stopped answering"), std::string::npos);
}

TEST_F(WingsInstallRig, AnInstallUnderWayIsFollowedAfterReopening)
{
    fake_.running = true;
    fake_.phase = "downloading";
    fake_.version = kTag;
    fake_.size = kSize;
    attach();
    EXPECT_EQ(download().phase, WingsInstallPhase::verified); // the download goes on to "downloaded"
    EXPECT_TRUE(fake_.downloads.empty());                     // no second download was asked for
    EXPECT_TRUE(fake_.installs.empty());
}

TEST_F(WingsInstallRig, APlacingUnderWayIsFollowedAfterReopening)
{
    fake_.running = true;
    fake_.phase = "placing";
    fake_.installing = true;
    fake_.version = "0.1.2";
    fake_.size = kSize;
    attach();
    EXPECT_EQ(download().phase, WingsInstallPhase::placed);
    EXPECT_TRUE(fake_.downloads.empty());
    EXPECT_TRUE(fake_.installs.empty());
}

// The console history of 2026-10-14: an install Wings stopped following finished in the background.
TEST_F(WingsInstallRig, AnInstallThatFinishedInTheBackgroundReadsAsInstalled)
{
    fake_.running = true;
    fake_.phase = "placed";
    fake_.version = "0.1.2";
    fake_.target = "wings";
    fake_.installed_version = "0.1.2";
    fake_.old_kept = true;
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
    EXPECT_NE(s.message.find("already installed"), std::string::npos);
    EXPECT_TRUE(note_exists());
    EXPECT_TRUE(fake_.installs.empty());
    EXPECT_TRUE(logged("[WINGS-UPDATE] helper status: phase=placed version=0.1.2"));
}

TEST_F(WingsInstallRig, AnOldFileFromAnEarlierUpdateIsTidiedFirst)
{
    fake_.running = true;
    fake_.target = "manual"; // the placed file was replaced by hand since
    fake_.old_kept = true;
    attach();
    WingsInstallStatus s = download();
    ASSERT_EQ(s.phase, WingsInstallPhase::verified);
    EXPECT_EQ(fake_.tidies.size(), 1u);
    s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
}

TEST_F(WingsInstallRig, AnOldFileThatCannotBeTidiedSaysRestartWings)
{
    fake_.running = true;
    fake_.target = "wings";
    fake_.installed_version = "0.1.1";
    fake_.old_kept = true;
    fake_.tidy_ok = false;
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("restart"), std::string::npos);
    EXPECT_EQ(s.message.find("busy"), std::string::npos);
    EXPECT_TRUE(fake_.installs.empty());
}

TEST_F(WingsInstallRig, AnOldWaitingAnswerOnInstallIsNotCalledBusy)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    fake_.answer_install = 409; // old_waiting every time
    fake_.tidy_ok = false;
    const WingsInstallStatus s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("restart"), std::string::npos);
    EXPECT_TRUE(logged("the helper answered old_waiting"));
}

TEST_F(WingsInstallRig, AnotherJobIsWaitedForWithItsProgressThenTheInstallGoesOn)
{
    fake_.running = true;
    fake_.phase = "downloading";
    fake_.version = "0.1.1"; // another release
    fake_.size = kSize;
    std::vector<std::string> seen;
    on_poll = [&] { seen.push_back(installer_.status().message); };
    attach();
    WingsInstallStatus s = download();
    ASSERT_EQ(s.phase, WingsInstallPhase::verified) << s.message;
    EXPECT_TRUE(std::any_of(seen.begin(), seen.end(),
                            [](const std::string &m) { return m.find("Waiting") != std::string::npos; }));
    EXPECT_TRUE(fake_.installs.empty());
    s = install(true);
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
}

TEST_F(WingsInstallRig, AHelperFromAnotherPackageIsReplacedAndLogged)
{
    fake_.running = true;
    fake_.build = "1.0.0"; // the helper the previous test package shipped
    fake_.phase = "downloading";
    fake_.version = "0.1.2";
    fake_.size = kSize;
    fake_.frozen = true;
    attach();
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::verified) << s.message;
    EXPECT_EQ(fake_.cancels, 1);
    EXPECT_EQ(fake_.sent.size(), 1u);
    EXPECT_TRUE(logged("replaced a helper from another Wings package: phase=downloading version=0.1.2 helper=1.0.0"));
}

TEST_F(WingsInstallRig, ADownloadIsRefusedWhenTheReleaseIsNoLongerNewer)
{
    attach("00.001.009"); // running is ahead of v0.1.2
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("not newer"), std::string::npos);
    EXPECT_TRUE(net_.urls.empty());
    EXPECT_TRUE(fake_.requests.empty());
}

TEST_F(WingsInstallRig, NetworkOffSaysSoAndChangesNothing)
{
    attach();
    net_.up = false;
    const WingsInstallStatus s = download();
    EXPECT_EQ(s.phase, WingsInstallPhase::failed);
    EXPECT_NE(s.message.find("network"), std::string::npos);
    EXPECT_TRUE(net_.urls.empty());
    EXPECT_TRUE(fake_.requests.empty());
}

TEST_F(WingsInstallRig, TheThreadsDoTheSameJobs)
{
    attach();
    ASSERT_TRUE(installer_.start_download(release_));
    installer_.wait_idle();
    ASSERT_EQ(installer_.status().phase, WingsInstallPhase::verified);
    ASSERT_TRUE(installer_.install(true));
    installer_.wait_idle();
    EXPECT_EQ(installer_.status().phase, WingsInstallPhase::placed);
    EXPECT_FALSE(installer_.install(true));            // nothing left to install
    EXPECT_FALSE(installer_.start_download(release_)); // placed: quit and restart first
}

TEST_F(WingsInstallRig, NothingStartsWithoutADownloadableRelease)
{
    attach();
    WingsRelease none = release_;
    none.asset.downloadable = false;
    EXPECT_FALSE(installer_.start_download(none));
    EXPECT_FALSE(installer_.install(true)); // not verified yet
    EXPECT_EQ(installer_.status().phase, WingsInstallPhase::idle);
}

// The reported bug: after Install the screen offered "Download update" again. The installer's state lived only
// in memory, so closing and opening Wings before the new version ran (the old version still runs,
// the checker still says "newer") started the flow over.
TEST_F(WingsInstallRig, InstalledSurvivesReopeningWingsUntilTheNewVersionRuns)
{
    attach();
    ASSERT_EQ(download().phase, WingsInstallPhase::verified);
    ASSERT_EQ(install(true).phase, WingsInstallPhase::placed);
    const WingsUpdateStatus up = newer_status(release_);
    EXPECT_EQ(wings_flow_snapshot(up, installer_.status()).state, WingsFlowState::installed);

    attach(); // Wings closed and opened again; the old package is still mounted
    WingsInstallStatus s = installer_.status();
    EXPECT_EQ(s.phase, WingsInstallPhase::placed);
    EXPECT_EQ(s.tag, kTag);
    EXPECT_EQ(s.placed_by, WingsPlacedBy::helper);
    // The Settings rows are rebuilt (note_release every time), the checker runs again, the switch is touched.
    installer_.note_release(release_);
    installer_.note_release(release_);
    EXPECT_EQ(installer_.status().phase, WingsInstallPhase::placed);
    EXPECT_FALSE(installer_.start_download(release_));
    WingsFlowSnapshot snap = wings_flow_snapshot(up, installer_.status());
    EXPECT_EQ(snap.state, WingsFlowState::installed);
    EXPECT_EQ(snap.step, WingsFlowStep::restart);
    EXPECT_TRUE(snap.buttons.quit);
    EXPECT_FALSE(snap.buttons.download);
    WingsUpdateStatus rechecked = up;
    rechecked.advice = WingsAdvice::not_checked; // mid-check, or the switch off
    rechecked.enabled = false;
    EXPECT_EQ(wings_flow_snapshot(rechecked, installer_.status()).state, WingsFlowState::installed);
    EXPECT_TRUE(fake_.tidies.empty()); // the .old is the way back until then

    attach("00.001.002"); // the new version runs: the flow is over, and the helper tidies the .old
    installer_.wait_idle();
    EXPECT_EQ(installer_.status().phase, WingsInstallPhase::idle);
    EXPECT_FALSE(note_exists());
    ASSERT_EQ(fake_.tidies.size(), 1u);
    EXPECT_NE(fake_.tidies[0].find("00.001.002"), std::string::npos);
}

TEST_F(WingsInstallRig, ANoteFromAnEarlierWingsStillReadsAsInstalled)
{
    fs::create_directories(note_dir_);
    spit(note_dir_ + "/placed.v1", "tag=v0.1.2\nby=pegasus\n");
    attach();
    EXPECT_EQ(installer_.status().phase, WingsInstallPhase::placed);
    EXPECT_EQ(installer_.status().placed_by, WingsPlacedBy::pegasus);
}

TEST_F(WingsInstallRig, TheDownloadReportsSpeedFromTheClock)
{
    attach();
    double seen_speed = 0.0;
    on_poll = [this, &seen_speed] { seen_speed = std::max(seen_speed, installer_.status().speed_bps); };
    ASSERT_EQ(download().phase, WingsInstallPhase::verified); // the helper downloads in the Download step
    // Each status read is one second later on the fake clock and 30000 bytes further.
    EXPECT_NEAR(seen_speed, 30000.0, 1.0);
    EXPECT_EQ(installer_.status().eta_s, 0);
}

TEST(WingsPlacedNote, RoundTrip)
{
    WingsPlacedNote n;
    n.tag = "v0.1.2";
    n.by = WingsPlacedBy::pegasus;
    WingsPlacedNote back;
    ASSERT_TRUE(decode_wings_placed(encode_wings_placed(n), &back));
    EXPECT_EQ(back.tag, n.tag);
    EXPECT_EQ(back.by, n.by);
    n.by = WingsPlacedBy::helper;
    ASSERT_TRUE(decode_wings_placed(encode_wings_placed(n), &back));
    EXPECT_EQ(back.by, WingsPlacedBy::helper);
    EXPECT_FALSE(decode_wings_placed("tag=v0.1.2\nby=magic\n", &back));
    EXPECT_FALSE(decode_wings_placed("tag=x\nby=direct\n", &back));
}

TEST(WingsRate, SpeedAndTimeLeftFromSamples)
{
    WingsRate r;
    EXPECT_EQ(r.bytes_per_s(), 0.0);
    EXPECT_EQ(r.eta_s(1000), -1);
    r.sample(10.0, 0);
    r.sample(10.2, 200);
    EXPECT_EQ(r.bytes_per_s(), 0.0); // under half a second: unknown
    r.sample(11.0, 1000000);
    EXPECT_DOUBLE_EQ(r.bytes_per_s(), 1000000.0);
    EXPECT_EQ(r.eta_s(4000000), 3);
    EXPECT_EQ(r.eta_s(4000001), 4); // rounded up
    // Only the last few seconds count: a slow start is forgotten.
    for (int i = 1; i <= 10; ++i)
        r.sample(11.0 + i, 1000000 + static_cast<std::uint64_t>(i) * 2000000);
    EXPECT_NEAR(r.bytes_per_s(), 2000000.0, 1.0);
    EXPECT_EQ(r.eta_s(21000000), 0); // done
    // A restart of the count starts again.
    r.sample(30.0, 10);
    EXPECT_EQ(r.bytes_per_s(), 0.0);
    r.reset();
    EXPECT_EQ(r.eta_s(10), -1);
    EXPECT_EQ(wings_speed_text(0.0), "");
    EXPECT_EQ(wings_speed_text(2.5 * 1024 * 1024), "2.5 MB/s");
    EXPECT_EQ(wings_speed_text(512 * 1024), "512 KB/s");
    EXPECT_EQ(wings_eta_text(-1), "");
    EXPECT_EQ(wings_eta_text(12), "About 12 s left");
    EXPECT_EQ(wings_eta_text(61), "About 2 min left");
}

TEST(WingsFlow, EachPhaseIsOneStepWithItsButtons)
{
    WingsRelease release;
    ASSERT_TRUE(parse_wings_release(release_json(kTag, 100000, kSha), &release, nullptr));
    const WingsUpdateStatus up = newer_status(release);
    WingsInstallStatus inst;

    WingsFlowSnapshot s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::available);
    EXPECT_EQ(s.step_index, 1); // Download is next
    EXPECT_TRUE(s.buttons.download);
    EXPECT_TRUE(s.buttons.later);
    EXPECT_EQ(s.version_text, "0.1.2");

    inst.tag = kTag;
    inst.phase = WingsInstallPhase::checking;
    inst.total_bytes = 100000;
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::downloading);
    EXPECT_TRUE(s.buttons.cancel);

    inst.phase = WingsInstallPhase::downloading;
    inst.got_bytes = 25000;
    inst.speed_bps = 2.5 * 1024 * 1024;
    inst.eta_s = 12;
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::downloading);
    EXPECT_FLOAT_EQ(s.progress, 0.25f);
    EXPECT_EQ(s.speed_text, "2.5 MB/s");
    EXPECT_EQ(s.eta_text, "About 12 s left");

    inst.got_bytes = 100000; // all in: the hash and image check
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::checking);
    EXPECT_EQ(s.step, WingsFlowStep::download); // the helper checks its download
    EXPECT_LT(s.progress, 0.0f);
    EXPECT_FALSE(s.buttons.cancel);

    inst.phase = WingsInstallPhase::verified;
    inst.needs_replace_confirm = true;
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::ready_to_install);
    EXPECT_EQ(s.step_index, 2); // downloaded and checked: Install is next
    EXPECT_TRUE(s.buttons.install);
    EXPECT_TRUE(s.needs_replace_confirm);

    inst.phase = WingsInstallPhase::placing;
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::installing);
    EXPECT_FALSE(s.buttons.install);

    inst.phase = WingsInstallPhase::placed;
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::installed);
    EXPECT_EQ(s.step_index, 3);
    EXPECT_TRUE(s.step_done);
    EXPECT_TRUE(s.buttons.quit);

    inst.phase = WingsInstallPhase::manual;
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::manual);
    EXPECT_FALSE(s.manual_steps.empty());
    EXPECT_TRUE(s.buttons.steps);

    inst.phase = WingsInstallPhase::failed;
    inst.message = "GitHub answered HTTP 500. Nothing was changed.";
    s = wings_flow_snapshot(up, inst);
    EXPECT_EQ(s.state, WingsFlowState::failed);
    EXPECT_EQ(s.reason, inst.message);
    EXPECT_TRUE(s.can_retry);
    EXPECT_TRUE(s.buttons.retry);

    // A failure about an older release does not hide a newer one.
    inst.tag = "v0.1.1";
    EXPECT_EQ(wings_flow_snapshot(up, inst).state, WingsFlowState::available);

    // Nothing newer: idle, no buttons. A newer release Wings cannot fetch: the steps by hand.
    WingsUpdateStatus same = up;
    same.advice = WingsAdvice::up_to_date;
    s = wings_flow_snapshot(same, WingsInstallStatus());
    EXPECT_EQ(s.state, WingsFlowState::idle);
    EXPECT_FALSE(s.buttons.download || s.buttons.later || s.buttons.retry);
    WingsUpdateStatus no_pkg = up;
    no_pkg.asset.downloadable = false;
    no_pkg.asset.issue = WingsAssetIssue::missing;
    s = wings_flow_snapshot(no_pkg, WingsInstallStatus());
    EXPECT_EQ(s.state, WingsFlowState::manual);
    EXPECT_FALSE(s.manual_steps.empty());
    EXPECT_FALSE(s.buttons.download);
}

TEST(WingsInstallWords, EveryPhaseHasAValueAndTheConfirmNamesTheTarget)
{
    WingsInstallStatus s;
    for (const WingsInstallPhase p :
         {WingsInstallPhase::idle, WingsInstallPhase::checking, WingsInstallPhase::downloading,
          WingsInstallPhase::verified, WingsInstallPhase::placing, WingsInstallPhase::placed,
          WingsInstallPhase::manual, WingsInstallPhase::blocked, WingsInstallPhase::failed})
    {
        s.phase = p;
        s.message = "x";
        EXPECT_FALSE(wings_install_text(s).value.empty());
    }
    s.target = WingsTarget::own_copy;
    EXPECT_NE(wings_install_confirm_text(s, "0.1.2").find("placed yourself"), std::string::npos);
    s.target = WingsTarget::wings_copy;
    EXPECT_EQ(wings_install_confirm_text(s, "0.1.2").find("placed yourself"), std::string::npos);
    EXPECT_NE(wings_install_confirm_text(s, "0.1.2").find("close Wings and open it again"), std::string::npos);
}

bool mentions_jailbreak(const std::string &text)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("jailbreak") != std::string::npos;
}

// Every text the update flow shows: the snapshot (title, text, reason, manual steps), the Settings row words, the
// install confirm, the manual steps per target, the page's buttons and row, and every status message and log line
// of the installer on the helper's routes. None of them talks about a jailbreak: Wings is closed and opened again.
TEST_F(WingsInstallRig, NoUpdateTextMentionsAJailbreak)
{
    std::vector<std::string> texts;
    const WingsUpdateStatus up = newer_status(release_);
    const auto add_status = [&](const WingsInstallStatus &inst)
    {
        texts.push_back(inst.message);
        const WingsInstallText words = wings_install_text(inst);
        texts.push_back(words.value);
        texts.push_back(words.detail);
        for (const WingsInstallStep &step : wings_install_manual_steps(inst))
        {
            texts.push_back(step.label);
            texts.push_back(step.detail);
        }
        texts.push_back(wings_install_confirm_text(inst, "0.1.2"));
        WingsUpdateStatus same = up;
        same.advice = WingsAdvice::up_to_date;
        WingsUpdateStatus no_pkg = up;
        no_pkg.asset.downloadable = false;
        no_pkg.asset.issue = WingsAssetIssue::missing;
        for (const WingsUpdateStatus &u : {up, same, no_pkg})
        {
            const WingsFlowSnapshot snap = wings_flow_snapshot(u, inst);
            for (const std::string &t : {snap.title, snap.text, snap.reason})
                texts.push_back(t);
            for (const WingsInstallStep &step : snap.manual_steps)
            {
                texts.push_back(step.label);
                texts.push_back(step.detail);
            }
            UpdateView view;
            if (update_view_from_flow(snap, &view))
            {
                texts.push_back(update_row_value(&view));
                texts.push_back(update_progress_text(view));
                for (const UpdateButton &b : update_buttons(view))
                    texts.push_back(b.label);
            }
        }
    };
    texts.push_back(update_row_value(nullptr));

    // Every phase, target and step by hand.
    for (const WingsInstallPhase p :
         {WingsInstallPhase::idle, WingsInstallPhase::checking, WingsInstallPhase::downloading,
          WingsInstallPhase::verified, WingsInstallPhase::placing, WingsInstallPhase::placed,
          WingsInstallPhase::manual, WingsInstallPhase::blocked, WingsInstallPhase::failed})
        for (const WingsTarget t : {WingsTarget::unknown, WingsTarget::none, WingsTarget::wings_copy,
                                    WingsTarget::own_copy, WingsTarget::folder, WingsTarget::blocked})
            for (const bool tried : {false, true})
            {
                WingsInstallStatus inst;
                inst.tag = kTag;
                inst.phase = p;
                inst.target = t;
                inst.install_tried = tried;
                inst.needs_replace_confirm = t == WingsTarget::own_copy;
                inst.total_bytes = kSize;
                inst.got_bytes = tried ? kSize : kSize / 2;
                add_status(inst);
            }

    // The installer's own messages along each way through the flow.
    on_poll = [&] { texts.push_back(installer_.status().message); };
    const auto run = [&](const std::function<void()> &setup, bool and_install)
    {
        installer_.wait_idle();
        fs::remove_all(note_dir_);
        fake_ = FakeWingsHelper{};
        setup();
        attach();
        add_status(download());
        if (and_install)
            add_status(install(true));
    };
    run([] {}, true);                                                   // download -> downloaded -> install
    run([this] { fake_.downloaded_version = "0.1.2"; fake_.size = kSize; }, true); // held download
    run([this] { fake_.fail_with = "the download failed its SHA-256 check"; }, false);
    run([this] { fake_.loader_up = false; }, false);                    // the manual steps
    run([this] { fake_.target = "folder"; }, false);
    run([this] { fake_.target = "blocked"; }, false);
    run([this] { fake_.answer_download = 400; }, false);
    run([this] { fake_.answer_install = 409; fake_.tidy_ok = false; }, true); // old_waiting
    run([this] { fake_.running = true; fake_.target = "wings"; fake_.installed_version = "0.1.1";
                 fake_.old_kept = true; fake_.tidy_ok = false; }, false);
    // Install without a download (the helper lost it): "Download the update first".
    run([] {}, false);
    fake_.downloaded_version.clear();
    fake_.phase = "idle";
    add_status(install(true));
    run([this] { fake_.running = true; fake_.phase = "placed"; fake_.version = "0.1.2"; fake_.target = "wings";
                 fake_.installed_version = "0.1.2"; fake_.old_kept = true; }, false); // already installed
    texts.insert(texts.end(), log_.begin(), log_.end());

    EXPECT_GT(texts.size(), 100u);
    for (const std::string &t : texts)
        EXPECT_FALSE(mentions_jailbreak(t)) << t;
}

} // namespace
} // namespace orbit
