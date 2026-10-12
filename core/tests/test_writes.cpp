// Wings core tests - exact request paths and bodies for every write route.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Expected strings are what the Pegasus web UI's JSON.stringify / fetch calls
// produce (app.js / file-manager.js lines in the comments). No network: the
// ReplayTransport records what the adapter sends.

#include "test_harness.hpp"
#include "test_support.hpp"
#include "wings/logic.hpp"

using namespace wings;

namespace
{
void expect_req(Rig &r, const char *method, const std::string &path, const std::string &body, int line,
                const std::string &content_type = "")
{
    if (r.t->log().empty())
    {
        testing::report(false, "request recorded", __FILE__, line, std::string("nothing sent for ") + method + " " + path);
        return;
    }
    const auto &q = r.t->last();
    testing::report(q.method == method, "method", __FILE__, line, "got " + q.method + " expected " + method);
    testing::report(q.path == path, "path", __FILE__, line, "got " + q.path + " expected " + path);
    testing::report(q.body == body, "body", __FILE__, line, "got " + q.body + " expected " + body);
    if (!content_type.empty())
        testing::report(q.content_type == content_type, "content-type", __FILE__, line, "got " + q.content_type);
}
#define EXPECT_REQ(rig, method, path, body) expect_req(rig, method, path, body, __LINE__)
} // namespace

TEST(write_settings)
{
    Rig r;
    r.t->add("POST", "/api/settings", 200, "{\"downloadRoot\":\"/mnt/ext0/dl\",\"downloadConnections\":16}");
    auto s = r.api.setDownloadRoot("/mnt/ext0/dl"); // app.js:5103
    CHECK(s.ok() && s.value.downloadRoot == "/mnt/ext0/dl");
    EXPECT_REQ(r, "POST", "/api/settings", R"({"downloadRoot":"/mnt/ext0/dl"})");
    CHECK_EQ_INT(r.t->log().size(), 1); // reply carried settings: no follow-up GET

    Rig r2;
    r2.t->add("POST", "/api/settings", 200, R"({"ok":true})"); // reply without settings => reload (app.js:5117)
    r2.t->add("GET", "/api/settings", 200, R"({"downloadRoot":"/x","downloadConnections":32})");
    s = r2.api.setDownloadConnections(32); // app.js:5116
    CHECK(s.ok() && s.value.downloadConnections == 32);
    CHECK_EQ_INT(r2.t->log().size(), 2);
    CHECK_EQ_STR(r2.t->log()[0].body, R"({"downloadConnections":32})");
    CHECK_EQ_STR(r2.t->log()[1].method + " " + r2.t->log()[1].path, "GET /api/settings");

    r.t->clear_log();
    CHECK(!r.api.setDownloadConnections(0).ok());
    CHECK(!r.api.setDownloadConnections(65).ok());
    auto bad = r.api.setDownloadConnections(100);
    CHECK(bad.error.kind == ApiError::Kind::client);
    CHECK_EQ_STR(bad.error.message, "Download connections must be between 1 and 64.");
    CHECK(r.t->log().empty()); // validated locally, nothing sent
    CHECK(r.api.setDownloadConnections(1).error.kind != ApiError::Kind::client);
    CHECK(r.api.setDownloadConnections(64).error.kind != ApiError::Kind::client);

    r.t->clear_log();
    r.api.setDebridToken(DebridService::realDebrid, "tok123"); // app.js:5129
    EXPECT_REQ(r, "POST", "/api/settings", R"({"realDebridToken":"tok123"})");
    r.api.setDebridToken(DebridService::torBox, "tb"); // app.js:5142
    EXPECT_REQ(r, "POST", "/api/settings", R"({"torBoxToken":"tb"})");
    r.api.clearDebridToken(DebridService::torBox); // app.js:5147
    EXPECT_REQ(r, "POST", "/api/settings", R"({"torBoxToken":""})");
    r.api.clearDebridToken(DebridService::realDebrid);
    EXPECT_REQ(r, "POST", "/api/settings", R"({"realDebridToken":""})");
    // JSON.stringify escaping: quote, backslash, newline, control char, unicode untouched.
    r.api.setDebridToken(DebridService::torBox, std::string("a\"b\\c\n\x01\xC3\xA9"));
    EXPECT_REQ(r, "POST", "/api/settings", std::string("{\"torBoxToken\":\"a\\\"b\\\\c\\n\\u0001\xC3\xA9\"}"));

    SettingsUpdate none;
    CHECK(r.api.updateSettings(none).error.kind == ApiError::Kind::client);
    SettingsUpdate combo;
    combo.hasDownloadRoot = combo.hasDownloadConnections = combo.hasRealDebridToken = combo.hasTorBoxToken = true;
    combo.downloadRoot = "/r";
    combo.downloadConnections = 8;
    combo.realDebridToken = "a";
    combo.torBoxToken = "b";
    r.api.updateSettings(combo);
    EXPECT_REQ(r, "POST", "/api/settings", R"({"downloadRoot":"/r","downloadConnections":8,"realDebridToken":"a","torBoxToken":"b"})");
}

TEST(write_debrid_refresh)
{
    Rig r;
    r.t->add("POST", "/api/debrid/refresh", 200,
             R"({"configured":true,"valid":true,"premium":false,"accountType":"free","hosterCount":3,"domainCount":9,"lastCheckedAt":5,"error":"","domains":["a.com","b.org"]})");
    auto d = r.api.refreshDebrid(DebridService::torBox); // app.js:5165
    EXPECT_REQ(r, "POST", "/api/debrid/refresh", R"({"service":"torbox"})");
    CHECK(d.ok() && d.value.configured && d.value.valid && !d.value.premium);
    CHECK_EQ_STR(d.value.accountType, "free");
    CHECK_EQ_INT(d.value.domains.size(), 2);
    r.api.refreshDebrid(DebridService::realDebrid);
    EXPECT_REQ(r, "POST", "/api/debrid/refresh", R"({"service":"real-debrid"})");
}

TEST(write_sources)
{
    Rig r;
    r.api.addSourceUrl("https://example.com/cat.json"); // app.js:4831
    EXPECT_REQ(r, "POST", "/api/sources/url", R"({"url":"https://example.com/cat.json"})");
    const std::string catalog = "{\n  \"name\": \"Mine\",\n  \"packages\": []\n}\n";
    r.api.uploadSourceCatalog(catalog); // app.js:4804 raw file text, untouched
    EXPECT_REQ(r, "POST", "/api/sources/upload", catalog);
    CHECK(r.t->last().content_type.empty()); // empty => the wire format defaults to application/json (see wire_format test)
    r.api.deleteSource(7); // app.js:4942
    EXPECT_REQ(r, "POST", "/api/sources/delete", R"({"sourceId":7})");
    r.api.refreshSource(3); // app.js:4955
    EXPECT_REQ(r, "POST", "/api/sources/refresh", R"({"sourceId":3})");
    r.api.setSourceEnabled(7, false); // app.js:4979
    EXPECT_REQ(r, "POST", "/api/sources/enabled", R"({"sourceId":7,"enabled":false})");
    r.api.setSourceEnabled(7, true);
    EXPECT_REQ(r, "POST", "/api/sources/enabled", R"({"sourceId":7,"enabled":true})");
}

TEST(write_default_sources)
{
    Rig r;
    r.t->add("POST", "/api/default-sources/apply", 200, R"({"failures":["DLPS Catalog: timeout",{"id":"pfs","error":"bad json"}],"sources":[{"id":9,"name":"n"}]})");
    auto a = r.api.applyDefaultSources(2, {"dlps", "pfs"}); // app.js:2819
    EXPECT_REQ(r, "POST", "/api/default-sources/apply", R"({"revision":2,"selectedIds":["dlps","pfs"]})");
    CHECK(a.ok());
    CHECK_EQ_INT(a.value.failures.size(), 2);
    CHECK_EQ_STR(a.value.failures[0], "DLPS Catalog: timeout");
    CHECK_EQ_STR(a.value.failures[1], "pfs: bad json");
    CHECK(a.value.hasSources && a.value.sources.size() == 1);
    r.api.applyDefaultSources(0, {});
    EXPECT_REQ(r, "POST", "/api/default-sources/apply", R"({"revision":0,"selectedIds":[]})");
    r.api.markDefaultSourcesSeen(); // app.js:2860: no body, no JSON content type
    EXPECT_REQ(r, "POST", "/api/default-sources/seen", "");
}

TEST(write_queue)
{
    Rig r;
    r.api.enqueuePackage(6, 0); // app.js:4869
    EXPECT_REQ(r, "POST", "/api/queue", R"({"packageId":6,"linkIndex":0})");
    r.api.queueDirect("https://h/f.bin", "f.bin", "application/octet-stream"); // app.js:3560
    EXPECT_REQ(r, "POST", "/api/queue/direct", R"({"url":"https://h/f.bin","fileName":"f.bin","contentType":"application/octet-stream"})");
    r.api.queueDirect("https://h/f.bin", "", "");
    EXPECT_REQ(r, "POST", "/api/queue/direct", R"({"url":"https://h/f.bin","fileName":"","contentType":""})");
    r.api.pauseJob(5); // app.js:4997 /api/queue/${action}
    EXPECT_REQ(r, "POST", "/api/queue/pause", R"({"jobId":5})");
    r.api.resumeJob(5);
    EXPECT_REQ(r, "POST", "/api/queue/resume", R"({"jobId":5})");
    r.api.cancelJob(5);
    EXPECT_REQ(r, "POST", "/api/queue/cancel", R"({"jobId":5})");
    r.api.retryJob(5);
    EXPECT_REQ(r, "POST", "/api/queue/retry", R"({"jobId":5})");
    r.api.clearFinished(); // app.js:5250
    EXPECT_REQ(r, "POST", "/api/queue/clear-finished", "");
    r.api.submitArchivePassword(3, "pw\"x"); // app.js:5021
    EXPECT_REQ(r, "POST", "/api/queue/archive-password", R"({"jobId":3,"password":"pw\"x"})");
    r.api.submitArchiveChoice(3, 1); // app.js:5043
    EXPECT_REQ(r, "POST", "/api/queue/archive-choice", R"({"jobId":3,"candidateIndex":1})");

    r.t->clear_log();
    CHECK(r.api.submitArchivePassword(3, "").error.kind == ApiError::Kind::client);
    CHECK(r.api.submitArchiveChoice(3, -1).error.kind == ApiError::Kind::client);
    CHECK(r.api.queueDirect("", "x", "").error.kind == ApiError::Kind::client);
    CHECK(r.t->log().empty());
    r.api.submitArchiveChoice(3, 0); // index 0 is valid
    EXPECT_REQ(r, "POST", "/api/queue/archive-choice", R"({"jobId":3,"candidateIndex":0})");
}

TEST(queue_waiting_states_parse)
{
    Rig r;
    r.t->add("GET", "/api/queue", 200,
             R"({"summary":{"total":2,"waitingPassword":1,"waitingChoice":1},"jobs":[
               {"id":10,"status":"waiting_password","archivePasswordRequired":true,"archiveFormat":"rar","archiveMode":"stream","phase":"extract","detail":"Wrong password"},
               {"id":11,"status":"waiting_choice","archiveCandidates":[{"index":0,"name":"GameA"},{"index":1,"name":"GameB"}],"phase":"finish","futureField":1}]})");
    auto q = r.api.getQueue();
    CHECK(q.ok() && q.value.jobs.size() == 2);
    CHECK(q.value.jobs[0].status == JobStatus::waitingPassword && q.value.jobs[0].archivePasswordRequired);
    CHECK(logic::queueJobStreamMode(q.value.jobs[0])); // archiveMode == "stream"
    CHECK(q.value.jobs[1].status == JobStatus::waitingChoice);
    CHECK_EQ_INT(q.value.jobs[1].archiveCandidates.size(), 2);
    CHECK_EQ_STR(q.value.jobs[1].archiveCandidates[1].name, "GameB");
    CHECK_EQ_INT(q.value.summary.waitingPassword, 1);
    CHECK_EQ_INT(q.value.summary.waitingChoice, 1);
}

TEST(write_link_flows)
{
    Rig r;
    r.api.resolveProbe("https://example.com/f"); // app.js:3547
    EXPECT_REQ(r, "POST", "/api/resolve/probe", R"({"url":"https://example.com/f"})");

    r.api.debridQueue(6, 2, DebridService::torBox); // app.js:4907
    EXPECT_REQ(r, "POST", "/api/debrid/queue", R"({"packageId":6,"linkIndex":2,"service":"torbox"})");
    r.api.debridQueue(6, 0, DebridService::realDebrid);
    EXPECT_REQ(r, "POST", "/api/debrid/queue", R"({"packageId":6,"linkIndex":0,"service":"real-debrid"})");
    r.api.debridQueueSelection("sel-1", {0, 2}, DebridService::realDebrid); // app.js:3939
    EXPECT_REQ(r, "POST", "/api/debrid/queue", R"({"selectionId":"sel-1","indexes":[0,2],"service":"real-debrid"})");
    r.api.debridQueueSelection("sel-1", {-1, 3}, DebridService::torBox); // negatives filtered (app.js:3926)
    EXPECT_REQ(r, "POST", "/api/debrid/queue", R"({"selectionId":"sel-1","indexes":[3],"service":"torbox"})");
    r.t->clear_log();
    CHECK(r.api.debridQueueSelection("sel-1", {}, DebridService::torBox).error.kind == ApiError::Kind::client);
    CHECK(r.api.debridQueueSelection("sel-1", {-5}, DebridService::torBox).error.kind == ApiError::Kind::client);
    CHECK(r.t->log().empty());

    // needsSelection reply (app.js:4911-4915)
    r.t->add("POST", "/api/debrid/queue", 200,
             R"({"needsSelection":true,"selectionId":"abc","options":[{"index":0,"filename":"a.rar","filesize":1048576,"host":"1fichier.com"},{"index":1,"filename":"b.rar","filesize":2}]})");
    auto dq = r.api.debridQueue(1, 0, DebridService::torBox);
    CHECK(dq.ok() && dq.value.needsSelection);
    CHECK_EQ_STR(dq.value.selectionId, "abc");
    CHECK_EQ_INT(dq.value.options.size(), 2);
    CHECK_EQ_STR(dq.value.options[0].filename, "a.rar");
    CHECK_EQ_INT(dq.value.options[0].filesize, 1048576);
    CHECK_EQ_STR(dq.value.options[0].host, "1fichier.com");
    CHECK(!dq.value.response.hasQueue && dq.value.response.dispatchResults.empty());
    // dispatched reply with a native result
    r.t->set("POST", "/api/debrid/queue", 200,
             R"({"summary":{"total":1,"queued":1},"jobs":[{"id":4,"status":"queued"}],"dispatchResults":[{"ok":true,"kind":"native-pkg","accepted":true,"title":"T","contentId":"C1"},{"ok":true,"kind":"queued","jobId":4}]})");
    dq = r.api.debridQueue(1, 0, DebridService::torBox);
    CHECK(dq.ok() && !dq.value.needsSelection && dq.value.response.hasQueue);
    CHECK_EQ_INT(logic::nativeDispatchResults(dq.value.response).size(), 1);
    CHECK_EQ_INT(logic::queuedDispatchResults(dq.value.response).size(), 1);

    r.t->add("POST", "/api/provider/resolve", 200, R"({"sessionId":12,"extra":true})");
    auto pr = r.api.providerResolve(6, 1); // app.js:4078
    EXPECT_REQ(r, "POST", "/api/provider/resolve", R"({"packageId":6,"linkIndex":1})");
    CHECK(pr.ok() && pr.value.sessionId == 12);

    r.api.browserOpen("https://rootz.so/d/1", true, 6, 2); // app.js:4081
    EXPECT_REQ(r, "POST", "/api/browser/open", R"({"url":"https://rootz.so/d/1","guarded":true,"packageId":6,"linkIndex":2})");
    r.api.browserOpen("https://filekeeper.net/x", false, 6, 0);
    EXPECT_REQ(r, "POST", "/api/browser/open", R"({"url":"https://filekeeper.net/x","guarded":false,"packageId":6,"linkIndex":0})");
    r.api.browserOpen("https://x.example/p", false, std::nullopt, std::nullopt); // app.js:4929
    EXPECT_REQ(r, "POST", "/api/browser/open", R"({"url":"https://x.example/p","guarded":false})");

    r.t->add("GET", "/api/browser/capture/status?sessionId=12", 200,
             R"({"sessionId":12,"status":"completed","message":"done","dispatchResult":{"ok":true,"kind":"queued","jobId":9},"summary":{"total":1},"jobs":[{"id":9}]})");
    auto cs = r.api.captureStatus(12); // app.js:4032
    EXPECT_REQ(r, "GET", "/api/browser/capture/status?sessionId=12", "");
    CHECK(cs.ok() && cs.value.state == CaptureState::completed);
    CHECK(cs.value.dispatch.hasQueue && cs.value.dispatch.dispatchResults.size() == 1);
    r.api.captureCancel(12); // app.js:3996
    EXPECT_REQ(r, "POST", "/api/browser/capture/cancel", R"({"sessionId":12})");
    r.api.captureActive();
    EXPECT_REQ(r, "GET", "/api/browser/capture/active", "");
}

TEST(write_storage_logs)
{
    Rig r;
    r.api.mkdir("/data/new folder"); // app.js:5093
    EXPECT_REQ(r, "POST", "/api/mkdir", R"({"path":"/data/new folder"})");
    r.api.browse("/mnt/ext0/My Games");
    EXPECT_REQ(r, "GET", "/api/browse?path=%2Fmnt%2Fext0%2FMy%20Games", "");
    r.api.getLogs(70);
    EXPECT_REQ(r, "GET", "/api/logs?since=70", "");
    r.api.clearLogs(); // app.js:5324
    EXPECT_REQ(r, "POST", "/api/logs/clear", "");
    r.api.clientLog("error", "API request failed", "/api/x boom"); // app.js:585
    EXPECT_REQ(r, "POST", "/api/client-log",
               R"({"level":"error","source":"frontend","message":"API request failed","detail":"/api/x boom"})");
    r.api.clientLog("info", "m", "", "wings");
    EXPECT_REQ(r, "POST", "/api/client-log", R"({"level":"info","source":"wings","message":"m","detail":""})");
}

TEST(write_apr_and_installed_games)
{
    Rig r;
    r.t->add("POST", "/api/apr-emu/update/start", 200, R"({"jobId":5,"titleId":"PPSA1","status":"running","progress":42.6,"detail":"Downloading"})");
    auto j = r.api.aprUpdateStart("PPSA1", ""); // app.js:4545
    EXPECT_REQ(r, "POST", "/api/apr-emu/update/start", R"({"titleId":"PPSA1"})");
    CHECK(j.ok() && j.value.jobId == "5" && j.value.status == "running" && j.value.progress == 43);
    r.api.aprUpdateStart("PPSA1", "rel-2");
    EXPECT_REQ(r, "POST", "/api/apr-emu/update/start", R"({"titleId":"PPSA1","versionId":"rel-2"})");

    r.t->add("GET", "/api/apr-emu/update/status?jobId=5", 200,
             R"({"jobId":"5","status":"completed","progress":150,"aprEmu":{"present":true,"version":"1.4.2","build":"release","remountRequired":true,"remountReason":"Remount the game"}})");
    auto st = r.api.aprUpdateStatus({"5", "PPSA1"}); // app.js:4458
    EXPECT_REQ(r, "GET", "/api/apr-emu/update/status?jobId=5", "");
    CHECK(st.ok() && st.value.status == "completed" && st.value.progress == 100 && st.value.hasAprEmu);
    CHECK(st.value.aprEmu.present && st.value.aprEmu.remountRequired);
    r.api.aprUpdateStatus({"", "PPSA 1"});
    EXPECT_REQ(r, "GET", "/api/apr-emu/update/status?titleId=PPSA%201", "");
    r.api.aprRestoreStatus({"", "PPSA1"}); // app.js:4513
    EXPECT_REQ(r, "GET", "/api/apr-emu/restore/status?titleId=PPSA1", "");
    r.api.aprRestoreStatus({"9", "PPSA1"});
    EXPECT_REQ(r, "GET", "/api/apr-emu/restore/status?jobId=9", "");
    r.t->add("GET", "/api/apr-emu/restore/status?titleId=Q", 200, R"({"progress":-5})");
    auto rs = r.api.aprRestoreStatus({"", "Q"});
    CHECK(rs.value.status == "starting" && rs.value.progress == 0); // app.js:4373-4376 defaults

    // Delete flow is request-shape tested only (replayed); never sent to a real console by tests.
    r.t->add("POST", "/api/installed-games/delete", 200, R"({"jobId":7,"titleId":"PPSA1","status":"running","filesDeleted":12,"detail":"Deleting"})");
    auto del = r.api.installedGameDelete("PPSA1"); // app.js:4592
    EXPECT_REQ(r, "POST", "/api/installed-games/delete", R"({"titleId":"PPSA1"})");
    CHECK(del.ok() && del.value.jobId == "7" && del.value.filesDeleted == 12 && del.value.status == "running");
    r.api.installedGameDeleteStatus({"7", "PPSA1"}); // app.js:4650
    EXPECT_REQ(r, "GET", "/api/installed-games/delete/status?jobId=7", "");
    r.api.installedGameDeleteStatus({"", "PPSA1"});
    EXPECT_REQ(r, "GET", "/api/installed-games/delete/status?titleId=PPSA1", "");
    r.t->clear_log();
    CHECK(r.api.installedGameDelete("").error.kind == ApiError::Kind::client);
    CHECK(r.api.aprUpdateStart("", "").error.kind == ApiError::Kind::client);
    CHECK(r.t->log().empty());

    r.t->add("GET", "/appdb/icon?id=PPSA1", 200, "\x89PNG", "image/png");
    auto img = r.api.fetchAppdbArt("/appdb/icon?id=PPSA1");
    CHECK(img.ok() && img.value.size() == 4);
    CHECK(r.api.fetchAppdbArt("/etc/passwd").error.kind == ApiError::Kind::client);
}

TEST(write_file_manager)
{
    Rig r;
    r.api.fileMkdir("/data", " new "); // fm.js:1243 (name trimmed by validateName)
    EXPECT_REQ(r, "POST", "/api/files/mkdir", R"({"parent":"/data","name":"new"})");
    r.api.fileRename("/data/new", "renamed"); // fm.js:1261
    EXPECT_REQ(r, "POST", "/api/files/rename", R"({"path":"/data/new","name":"renamed"})");
    r.t->clear_log();
    CHECK(r.api.fileMkdir("/data", "a/b").error.kind == ApiError::Kind::client);
    CHECK(r.api.fileMkdir("/data", "..").error.kind == ApiError::Kind::client);
    CHECK(r.api.fileRename("/data/x", ".").error.kind == ApiError::Kind::client);
    CHECK(r.t->log().empty());

    r.api.fileDelete({"/data/a", "/data//a/", "/data/b"}); // fm.js:1274 uniquePaths
    EXPECT_REQ(r, "POST", "/api/files/delete", R"({"paths":["/data/a","/data/b"]})");
    r.api.fileCopy({"/data/a", "/data/b"}, "/mnt/ext0/", Collision::keepBoth); // fm.js:1291
    EXPECT_REQ(r, "POST", "/api/files/copy", R"({"paths":["/data/a","/data/b"],"destination":"/mnt/ext0","collision":"keep-both"})");
    r.api.fileMove({"/data/a"}, "/data/dst", Collision::replace);
    EXPECT_REQ(r, "POST", "/api/files/move", R"({"paths":["/data/a"],"destination":"/data/dst","collision":"replace"})");
    r.api.fileExtract({"/data/x.rar"}, "/data/out", Collision::skip); // fm.js:1311
    EXPECT_REQ(r, "POST", "/api/files/extract", R"({"paths":["/data/x.rar"],"destination":"/data/out","collision":"skip"})");
    r.api.fileExtractPassword(4, "s3cret"); // fm.js:1333
    EXPECT_REQ(r, "POST", "/api/files/extract/password", R"({"operationId":4,"password":"s3cret"})");
    r.api.filePkgInstall("/data/g.pkg"); // fm.js:1348
    EXPECT_REQ(r, "POST", "/api/files/pkg/install", R"({"path":"/data/g.pkg"})");
    r.api.fileOperationCancel(4); // fm.js:1444
    EXPECT_REQ(r, "POST", "/api/files/operation/cancel", R"({"operationId":4})");

    r.t->clear_log();
    CHECK(r.api.fileDelete({}).error.kind == ApiError::Kind::client);
    CHECK(r.api.fileDelete({"relative/path"}).error.kind == ApiError::Kind::client);
    CHECK(r.api.fileCopy({"/a"}, "relative", Collision::keepBoth).error.kind == ApiError::Kind::client);
    CHECK(r.api.fileCopy({}, "/b", Collision::keepBoth).error.kind == ApiError::Kind::client);
    CHECK(r.api.fileExtractPassword(1, "").error.kind == ApiError::Kind::client);
    CHECK(r.t->log().empty());

    // Upload: PUT, raw bytes, URLSearchParams encoding (space => '+'), fm.js:1377-1383.
    const std::string bytes("\x00\x01raw", 5);
    r.api.fileUpload("/data/my dir", "my file.bin", "", bytes);
    expect_req(r, "PUT", "/api/files/upload?directory=%2Fdata%2Fmy+dir&name=my+file.bin", bytes, __LINE__, "application/octet-stream");
    r.api.fileUpload("/data", "f.txt", "folder/sub/f.txt", "x");
    EXPECT_REQ(r, "PUT", "/api/files/upload?directory=%2Fdata&name=f.txt&relativePath=folder%2Fsub%2Ff.txt", "x");
    r.api.fileUpload("/data", "", "", "");
    EXPECT_REQ(r, "PUT", "/api/files/upload?directory=%2Fdata&name=upload.bin", "");

    r.api.fileList("//data//x/"); // normalised like fm.js:918
    EXPECT_REQ(r, "GET", "/api/files/list?path=%2Fdata%2Fx", "");
    r.api.fileList("");
    EXPECT_REQ(r, "GET", "/api/files/list?path=%2F", "");
    r.api.fileLocations();
    EXPECT_REQ(r, "GET", "/api/files/locations", "");
    r.api.fileOperations();
    EXPECT_REQ(r, "GET", "/api/files/operations", "");
}

TEST(file_operation_states_parse)
{
    Rig r;
    r.t->add("GET", "/api/files/operations", 200,
             R"({"operations":[{"operationId":2,"type":"extract","status":"waiting_password","error":"Wrong archive password","source":"/d/x.rar","cancelable":true},
                               {"operationId":3,"type":"pkg-install","status":"accepted"},
                               {"operationId":4,"type":"copy","status":"running","cancelable":false,"cancelRequested":true}]})");
    auto ops = r.api.fileOperations();
    CHECK(ops.ok() && ops.value.size() == 3);
    CHECK(ops.value[0].status == FileOpStatus::waitingPassword && logic::canCancelFileOp(ops.value[0]));
    CHECK_EQ_STR(ops.value[0].error, "Wrong archive password");
    CHECK(logic::isTerminalFileOp(ops.value[1].status));
    CHECK(!logic::canCancelFileOp(ops.value[2]) && ops.value[2].cancelRequested);
}
