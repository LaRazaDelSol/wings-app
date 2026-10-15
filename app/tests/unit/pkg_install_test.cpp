// Wings TV app - .pkg links that download fast to disk and then install (pkg_install.hpp, B-17).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_backend.hpp"
#include "orbit/pkg_install.hpp"
#include "orbit/prefs.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

namespace orbit
{
namespace
{

using Kind = PkgStep::Kind;

StorageMount mount(const std::string &path, double free_bytes)
{
    StorageMount m;
    m.path = path;
    m.free_bytes = free_bytes;
    return m;
}

// ---- route choice ----

TEST(PkgInstall, RouteGoesDirectOnlyWhenTheProbeIsFineAndTheSpaceFits)
{
    const std::vector<StorageMount> roomy{mount("/data", 100e9)};
    EXPECT_EQ(pkg_route_before_queue(false, "", 10e9, "/data/dl", roomy).route, PkgRoute::handoff);
    EXPECT_EQ(pkg_route_before_queue(true, "Link returned HTTP 404.", 10e9, "/data/dl", roomy).route,
              PkgRoute::handoff);
    EXPECT_EQ(pkg_route_before_queue(true, "", 10e9, "/data/dl", roomy).route, PkgRoute::direct);
    // Size or free space unknown: let Pegasus try.
    EXPECT_EQ(pkg_route_before_queue(true, "", -1.0, "/data/dl", roomy).route, PkgRoute::direct);
    EXPECT_EQ(pkg_route_before_queue(true, "", 10e9, "/data/dl", {}).route, PkgRoute::direct);
    // On /data: the file plus the installed app.
    EXPECT_EQ(pkg_route_before_queue(true, "", 60e9, "/data/dl", roomy).route, PkgRoute::handoff);
    // Root on a USB drive: the file there, the staging copy and the app on /data.
    const std::vector<StorageMount> usb{mount("/data", 15e9), mount("/mnt/usb0", 500e9)};
    const PkgRouteChoice short_data = pkg_route_before_queue(true, "", 10e9, "/mnt/usb0/dl", usb);
    EXPECT_EQ(short_data.route, PkgRoute::handoff);
    EXPECT_NE(short_data.reason.find("/data"), std::string::npos);
    EXPECT_EQ(pkg_route_before_queue(true, "", 5e9, "/mnt/usb0/dl", usb).route, PkgRoute::direct);
}

TEST(PkgInstall, ADirectAnswerIsTrackedHandedOffOrFallsBack)
{
    EXPECT_EQ(pkg_classify_direct(false, false, false, 0), PkgDirectAnswer::use_handoff);
    EXPECT_EQ(pkg_classify_direct(true, true, false, 0), PkgDirectAnswer::handed_off);
    EXPECT_EQ(pkg_classify_direct(true, false, true, 41), PkgDirectAnswer::track);
    EXPECT_EQ(pkg_classify_direct(true, false, true, 0), PkgDirectAnswer::use_handoff);
    EXPECT_EQ(pkg_classify_direct(true, false, false, 0), PkgDirectAnswer::use_handoff);
}

TEST(PkgInstall, FileStemsAreSafeAndNeverEmpty)
{
    EXPECT_EQ(pkg_file_stem("God of War: Ragnarok (USA).pkg"), "God of War Ragnarok (USA)");
    EXPECT_EQ(pkg_file_stem("/data/dl/Game.FPKG"), "Game");
    EXPECT_EQ(pkg_file_stem("a/b\\c?.pkg"), "c");
    EXPECT_EQ(pkg_file_stem(""), "package");
    EXPECT_EQ(pkg_file_stem("..."), "package");
    EXPECT_EQ(pkg_folder_of("/data/dl/T.pkg"), "/data/dl");
}

// ---- stages ----

PkgOpSeen op(const std::string &status, const std::string &error = "")
{
    return {"7", "pkg install", status, error};
}

// Tracked job 41, downloaded and renamed, install running as operation 7.
void run_to_installing(PkgInstallModel &m, const std::string &title_id = "PPSA01234")
{
    m.track("41", "Tea", title_id, "Tea.pkg");
    m.observe_queue({{"41", "downloading", "/data/dl/Tea.fpkg", ""}});
    EXPECT_EQ(m.next(0.0).kind, Kind::none);
    EXPECT_EQ(m.row_text("41"), "");
    m.observe_queue({{"41", "completed", "/data/dl/Tea.fpkg", ""}});
    const PkgStep rename = m.next(0.0);
    ASSERT_EQ(rename.kind, Kind::rename);
    EXPECT_EQ(rename.path, "/data/dl/Tea.fpkg");
    EXPECT_EQ(rename.name, "Tea.pkg");
    EXPECT_EQ(m.row_text("41"), "Installing...");
    m.renamed("41", true, "/data/dl/Tea.pkg", "");
    const PkgStep install = m.next(0.0);
    ASSERT_EQ(install.kind, Kind::install);
    EXPECT_EQ(install.path, "/data/dl/Tea.pkg");
    m.install_started("41", true, {op("installing")}, "");
    EXPECT_EQ(m.next(0.0).kind, Kind::poll);
    m.observe_ops({op("staged")});
    EXPECT_EQ(m.find("41")->stage, PkgStage::installing);
}

TEST(PkgInstall, AcceptedThenSeenOnTheConsoleDeletesThePkg)
{
    PkgInstallModel m;
    run_to_installing(m);
    m.observe_ops({op("accepted")});
    EXPECT_EQ(m.row_text("41"), "Installed - find it on your home screen");
    PkgStep confirm = m.next(0.0);
    ASSERT_EQ(confirm.kind, Kind::confirm);
    EXPECT_EQ(confirm.title_id, "PPSA01234");
    m.confirmed("41", false, 0.0);
    EXPECT_EQ(m.next(1.0).kind, Kind::none); // not again right away
    ASSERT_EQ(m.next(25.0).kind, Kind::confirm);
    m.confirmed("41", true, 25.0);
    const PkgStep remove = m.next(26.0);
    ASSERT_EQ(remove.kind, Kind::remove);
    EXPECT_EQ(remove.path, "/data/dl/Tea.pkg");
    m.removed("41", true, "");
    EXPECT_EQ(m.next(30.0).kind, Kind::none);
    EXPECT_EQ(m.row_text("41"), "Installed - find it on your home screen");
}

TEST(PkgInstall, TheKeepSettingKeepsTheFileAfterASuccess)
{
    PkgInstallModel m;
    m.set_keep_files(true);
    run_to_installing(m);
    m.observe_ops({op("accepted")});
    EXPECT_EQ(m.find("41")->stage, PkgStage::installed);
    EXPECT_EQ(m.next(100.0).kind, Kind::none);
}

TEST(PkgInstall, AFailedInstallKeepsTheFileAndSaysWhere)
{
    PkgInstallModel m;
    run_to_installing(m);
    m.observe_ops({op("failed", "Pegasus DL package install failed: 0x80990085")});
    EXPECT_EQ(m.find("41")->stage, PkgStage::failed);
    EXPECT_EQ(m.row_text("41"),
              "Install failed: Pegasus DL package install failed: 0x80990085 (the file is kept in /data/dl)");
    for (double t : {0.0, 100.0, 10000.0})
        EXPECT_EQ(m.next(t).kind, Kind::none); // never a delete
}

TEST(PkgInstall, ARenameOrInstallRefusalFailsAndKeepsTheFile)
{
    PkgInstallModel renaming;
    renaming.track("41", "Tea", "", "Tea.pkg");
    renaming.observe_queue({{"41", "completed", "/data/dl/Tea.fpkg", ""}});
    renaming.renamed("41", false, "", "busy");
    EXPECT_EQ(renaming.find("41")->stage, PkgStage::failed);
    EXPECT_NE(renaming.row_text("41").find("/data/dl"), std::string::npos);
    EXPECT_EQ(renaming.next(0.0).kind, Kind::none);

    PkgInstallModel installing;
    installing.track("41", "Tea", "", "Tea.pkg");
    installing.observe_queue({{"41", "completed", "/data/dl/Tea.pkg", ""}}); // already .pkg: no rename
    ASSERT_EQ(installing.next(0.0).kind, Kind::install);
    installing.install_started("41", false, {}, "A package install is already running");
    EXPECT_EQ(installing.next(0.0).kind, Kind::install); // waits its turn
    installing.install_started("41", false, {}, "Not a package");
    EXPECT_EQ(installing.find("41")->stage, PkgStage::failed);
    EXPECT_EQ(installing.next(0.0).kind, Kind::none);
}

TEST(PkgInstall, OneInstallAtATimeAndAJobThatLeavesIsDropped)
{
    PkgInstallModel m;
    m.track("1", "A", "", "A.pkg");
    m.track("2", "B", "", "B.pkg");
    m.track("3", "C", "", "C.pkg");
    m.observe_queue({{"1", "completed", "/d/A.pkg", ""}, {"2", "completed", "/d/B.pkg", ""}});
    EXPECT_EQ(m.find("3"), nullptr); // cancelled and gone before it finished
    ASSERT_EQ(m.next(0.0).job_id, "1");
    m.install_started("1", true, {{"9", "pkg install", "installing", ""}}, "");
    EXPECT_EQ(m.next(0.0).kind, Kind::poll); // B waits
    m.observe_ops({{"9", "pkg install", "accepted", ""}});
    const PkgStep b = m.next(0.0);
    EXPECT_EQ(b.kind, Kind::install);
    EXPECT_EQ(b.job_id, "2");
}

TEST(PkgInstall, WithoutATitleIdANewTitleConfirmsTheInstall)
{
    EXPECT_TRUE(pkg_install_seen("PPSA1", {}, {"PPSA0", "PPSA1"}));
    EXPECT_FALSE(pkg_install_seen("PPSA1", {}, {"PPSA0"}));
    EXPECT_FALSE(pkg_install_seen("", {"PPSA0"}, {"PPSA0"}));
    EXPECT_TRUE(pkg_install_seen("", {"PPSA0"}, {"PPSA0", "PPSA2"}));

    PkgInstallModel m;
    m.track("41", "Tea", "", "Tea.pkg");
    m.observe_queue({{"41", "completed", "/d/Tea.pkg", ""}});
    m.titles_before("41", {"PPSA0"});
    m.install_started("41", true, {op("installing")}, "");
    m.observe_ops({op("accepted")});
    const PkgStep confirm = m.next(0.0);
    ASSERT_EQ(confirm.kind, Kind::confirm);
    EXPECT_EQ(confirm.before, std::vector<std::string>{"PPSA0"});

    // No title id and no list from before: the file is kept rather than guessed at.
    PkgInstallModel blind;
    blind.track("41", "Tea", "", "Tea.pkg");
    blind.observe_queue({{"41", "completed", "/d/Tea.pkg", ""}});
    blind.install_started("41", true, {op("installing")}, "");
    blind.observe_ops({op("accepted")});
    EXPECT_EQ(blind.next(0.0).kind, Kind::none);
}

TEST(PkgInstall, TheKeepSettingIsSavedAndOffByDefault)
{
    WingsLocalPrefs prefs;
    EXPECT_FALSE(prefs.keep_pkg_files);
    prefs.keep_pkg_files = true;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_TRUE(back.keep_pkg_files);
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("auto_reconnect=1\n", &old));
    EXPECT_FALSE(old.keep_pkg_files);
}

// ---- PegasusBackend over recorded replies ----

std::string samples_dir()
{
    if (const char *env = std::getenv("WINGS_SAMPLES"))
        return env;
    return std::string(HUI_SOURCE_DIR) + "/../../recon/samples";
}

bool read_sample(const std::string &name, std::string *out)
{
    std::ifstream file(samples_dir() + "/" + name, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    *out = text.str();
    return true;
}

constexpr const char *kHealth =
    R"({"status":"running","port":6970,"downloadOnly":false,"nativePkgHandoff":true,"version":"1.11.0"})";
constexpr const char *kProbe =
    R"({"requestedUrl":"https://h/x.pkg","finalUrl":"https://h/x.pkg","status":200,"contentType":"application/octet-stream","fileName":"x.pkg","hasContentLength":true,"contentLength":1000,"isHtml":false,"isDownload":true})";
constexpr const char *kQueuedDirect =
    R"({"jobs":[],"dispatchResult":{"kind":"queued","ok":true,"jobId":41,"fileName":"T.fpkg","message":"Download added to the Pegasus DL queue."}})";
constexpr const char *kNativeAnyway =
    R"({"dispatchResult":{"ok":true,"kind":"native-pkg","accepted":true,"title":"T","message":"started","contentId":"UP0000-TEST_00-0000000000000000"}})";

struct PkgRig
{
    std::shared_ptr<wings::http::ReplayTransport> replay = std::make_shared<wings::http::ReplayTransport>();
    PegasusBackend backend{replay, "127.0.0.1:6970"};
    std::vector<std::string> logged;
    std::string package_id;
    std::string title_id;
    int link = -1;

    // False when the recorded library is not reachable.
    bool load()
    {
        pkg_installs().reset();
        pkg_installs().set_keep_files(false);
        pkg_installs().set_log([this](const std::string &line) { logged.push_back(line); });
        replay->add("GET", "/api/health", 200, kHealth);
        std::string library;
        if (!read_sample("library.json", &library))
            return false;
        replay->add("GET", "/api/library", 200, library);
        const Result<std::vector<Package>> all = backend.library();
        for (const Package &p : all.value)
        {
            if (!p.has_pkg)
                continue;
            const Result<PackageDetail> detail = backend.package(p.id);
            for (const PackageLink &l : detail.value.links)
            {
                if (l.route == LinkRoute::native_pkg)
                {
                    package_id = p.id;
                    title_id = p.title_id;
                    link = l.index;
                    return true;
                }
            }
        }
        return false;
    }
    ~PkgRig() { pkg_installs().set_log(nullptr); }
};

#define LOAD_RIG(rig)                                                                              \
    PkgRig rig;                                                                                    \
    if (!rig.load())                                                                               \
        GTEST_SKIP() << "recon/samples/library.json is not reachable from here";

TEST(PkgInstallBackend, APkgLinkDownloadsToDiskThenInstallsAndTheFileIsDeleted)
{
    LOAD_RIG(rig)
    rig.replay->add("POST", "/api/resolve/probe", 200, kProbe);
    rig.replay->add("POST", "/api/queue/direct", 200, kQueuedDirect);
    const Result<LinkResult> started = rig.backend.start_link(rig.package_id, rig.link, "");
    ASSERT_TRUE(started.ok());
    EXPECT_EQ(started.value.kind, LinkResult::Kind::dispatched);
    EXPECT_FALSE(started.value.dispatch.native);
    EXPECT_EQ(rig.replay->count("POST", "/api/queue"), 0u); // never the PS5's Downloads
    ASSERT_EQ(rig.replay->count("POST", "/api/queue/direct"), 1u);
    EXPECT_NE(rig.replay->last().body.find(".fpkg\""), std::string::npos);

    const std::string tid = rig.title_id.empty() ? std::string("PPSA99999") : rig.title_id;
    rig.replay->add("GET", "/api/queue", 200,
                    R"({"jobs":[{"id":41,"status":"completed","fileName":"T.fpkg","destinationPath":"/data/pegasus-dl/downloads/T.fpkg"}]})");
    rig.replay->add("POST", "/api/files/rename", 200,
                    R"({"path":"/data/pegasus-dl/downloads/T.pkg","name":"T.pkg","type":"file"})");
    rig.replay->add("POST", "/api/files/pkg/install", 200,
                    R"({"operations":[{"operationId":7,"type":"pkg install","status":"installing"}]})");
    rig.replay->add("GET", "/api/files/operations", 200,
                    R"({"operations":[{"operationId":7,"type":"pkg install","status":"accepted"}]})");
    if (rig.title_id.empty())
        rig.replay->add("GET", "/appdb", 200, R"({"titles":[]})");
    rig.replay->add("GET", "/appdb", 200, R"({"titles":[{"titleId":")" + tid + R"("}]})");
    rig.replay->add("POST", "/api/files/delete", 200, R"({"operations":[]})");

    const Result<std::vector<Job>> first = rig.backend.queue();
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value.size(), 1u);
    EXPECT_EQ(first.value[0].install_state, "installed");
    EXPECT_EQ(first.value[0].install_text, "Installed - find it on your home screen");
    EXPECT_EQ(rig.replay->count("POST", "/api/files/delete"), 0u); // not before the console has it
    ASSERT_TRUE(rig.backend.queue().ok());                         // confirm
    ASSERT_TRUE(rig.backend.queue().ok());                         // delete
    ASSERT_EQ(rig.replay->count("POST", "/api/files/delete"), 1u);
    EXPECT_NE(rig.replay->last().body.find("/data/pegasus-dl/downloads/T.pkg"), std::string::npos);
    bool tagged = false;
    for (const std::string &line : rig.logged)
        tagged = tagged || line.rfind("[WINGS-PKG] ", 0) == 0;
    EXPECT_TRUE(tagged);
}

TEST(PkgInstallBackend, AFailedInstallKeepsTheFile)
{
    LOAD_RIG(rig)
    rig.replay->add("POST", "/api/resolve/probe", 200, kProbe);
    rig.replay->add("POST", "/api/queue/direct", 200, kQueuedDirect);
    ASSERT_TRUE(rig.backend.start_link(rig.package_id, rig.link, "").ok());
    rig.replay->add("GET", "/api/queue", 200,
                    R"({"jobs":[{"id":41,"status":"completed","destinationPath":"/data/dl/T.pkg"}]})");
    rig.replay->add("GET", "/appdb", 200, R"({"titles":[]})");
    rig.replay->add("POST", "/api/files/pkg/install", 200,
                    R"({"operations":[{"operationId":7,"type":"pkg install","status":"installing"}]})");
    rig.replay->add("GET", "/api/files/operations", 200,
                    R"({"operations":[{"operationId":7,"type":"pkg install","status":"failed","error":"0x80990085"}]})");
    const Result<std::vector<Job>> jobs = rig.backend.queue();
    ASSERT_TRUE(jobs.ok());
    ASSERT_EQ(jobs.value.size(), 1u);
    EXPECT_EQ(jobs.value[0].install_state, "install_failed");
    EXPECT_EQ(jobs.value[0].install_text, "Install failed: 0x80990085 (the file is kept in /data/dl)");
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(rig.backend.queue().ok());
    EXPECT_EQ(rig.replay->count("POST", "/api/files/delete"), 0u);
}

TEST(PkgInstallBackend, TheOldHandOffIsUsedWhenTheDirectPathIsNotThere)
{
    {
        LOAD_RIG(rig) // no probe answer
        rig.replay->add("POST", "/api/queue", 200, kNativeAnyway);
        const Result<LinkResult> started = rig.backend.start_link(rig.package_id, rig.link, "");
        ASSERT_TRUE(started.ok());
        EXPECT_TRUE(started.value.dispatch.native);
        EXPECT_EQ(rig.replay->count("POST", "/api/queue/direct"), 0u);
        EXPECT_EQ(rig.replay->count("POST", "/api/queue"), 1u);
    }
    {
        LOAD_RIG(rig) // the direct queue fails
        rig.replay->add("POST", "/api/resolve/probe", 200, kProbe);
        rig.replay->add("POST", "/api/queue/direct", 500, R"({"error":"nope"})");
        rig.replay->add("POST", "/api/queue", 200, kNativeAnyway);
        const Result<LinkResult> started = rig.backend.start_link(rig.package_id, rig.link, "");
        ASSERT_TRUE(started.ok());
        EXPECT_TRUE(started.value.dispatch.native);
        EXPECT_EQ(rig.replay->count("POST", "/api/queue"), 1u);
    }
    {
        LOAD_RIG(rig) // Pegasus hands the direct request to the PS5 anyway: nothing more is queued
        rig.replay->add("POST", "/api/resolve/probe", 200, kProbe);
        rig.replay->add("POST", "/api/queue/direct", 200, kNativeAnyway);
        const Result<LinkResult> started = rig.backend.start_link(rig.package_id, rig.link, "");
        ASSERT_TRUE(started.ok());
        EXPECT_TRUE(started.value.dispatch.native);
        EXPECT_EQ(rig.replay->count("POST", "/api/queue"), 0u);
    }
    {
        LOAD_RIG(rig) // not enough free space
        const std::string big =
            R"({"requestedUrl":"https://h/x.pkg","finalUrl":"https://h/x.pkg","status":200,"fileName":"x.pkg","hasContentLength":true,"contentLength":900000000000,"isDownload":true})";
        rig.replay->add("POST", "/api/resolve/probe", 200, big);
        rig.replay->add("GET", "/api/mounts", 200,
                        R"({"mounts":[{"path":"/data","freeBytes":1000000000,"totalBytes":2000000000}]})");
        rig.replay->add("POST", "/api/queue", 200, kNativeAnyway);
        ASSERT_TRUE(rig.backend.start_link(rig.package_id, rig.link, "").ok());
        EXPECT_EQ(rig.replay->count("POST", "/api/queue/direct"), 0u);
        EXPECT_EQ(rig.replay->count("POST", "/api/queue"), 1u);
    }
}

} // namespace
} // namespace orbit
