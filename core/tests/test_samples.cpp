// Wings core tests - every recorded Pegasus sample parses through PegasusV1.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_harness.hpp"
#include "test_support.hpp"
#include "wings/logic.hpp"

using namespace wings;

namespace
{
std::string M(const std::string &name)
{
    return sample("mutating/" + name);
}
} // namespace

TEST(sample_health)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/health", sample("health.json")));
    auto h = r.api.health();
    CHECK(h.ok());
    CHECK_EQ_STR(h.value.status, "running");
    CHECK_EQ_INT(h.value.port, 6970);
    CHECK(h.value.nativePkgHandoff);
    CHECK(!h.value.downloadOnly);
    CHECK_EQ_STR(h.value.version, "1.11.0");
    CHECK_EQ_STR(r.t->last().method, "GET");
    CHECK_EQ_STR(r.t->last().path, "/api/health");
    CHECK(r.t->last().body.empty());
}

TEST(sample_settings_and_unknown_fields)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/settings", fixture("settings.json")));
    auto s = r.api.getSettings();
    CHECK(s.ok());
    CHECK_EQ_STR(s.value.downloadRoot, "/data/pegasus-dl/downloads");
    CHECK_EQ_INT(s.value.downloadConnections, 16);
    CHECK(s.value.nativePkgHandoff && s.value.rangeResume && !s.value.queuePersists);
    CHECK(s.value.dnsHelper.present && s.value.dnsHelper.available && s.value.dnsHelper.nanodnsRunning);
    CHECK_EQ_STR(s.value.dnsHelper.recommendedPrimaryDns, "127.0.0.1");
    CHECK(s.value.realDebrid.present && !s.value.realDebrid.configured && s.value.realDebrid.domains.empty());
    CHECK(s.value.torBox.configured && s.value.torBox.valid && s.value.torBox.premium);
    CHECK_EQ_STR(s.value.torBox.accountType, "essential");
    CHECK_EQ_INT(s.value.torBox.hosterCount, 148);
    CHECK_EQ_INT(s.value.torBox.domainCount, 309);
    CHECK_EQ_INT(s.value.torBox.lastCheckedAt, 1791688772);
    CHECK_EQ_INT(s.value.torBox.domains.size(), 4);
    CHECK(logic::debridReady(s.value.torBox) && !logic::debridReady(s.value.realDebrid));
}

TEST(tolerant_parsing_missing_and_mistyped_fields)
{
    Rig r;
    // Missing everything, wrong types, unknown members: defaults, no failure.
    r.t->add("GET", "/api/settings", 200, R"({"downloadConnections":"x","concurrency":8,"dnsHelper":5,"torBox":null,"zzz":{"a":[1,2]}})");
    auto s = r.api.getSettings();
    CHECK(s.ok());
    CHECK_EQ_INT(s.value.downloadConnections, 8); // falls to `concurrency` (app.js:4722)
    CHECK(!s.value.dnsHelper.present && !s.value.torBox.present && !s.value.realDebrid.present);
    CHECK_EQ_STR(s.value.downloadRoot, "");
    r.t->add("GET", "/api/queue", 200, R"({})");
    auto q = r.api.getQueue();
    CHECK(q.ok() && q.value.jobs.empty() && q.value.summary.total == 0);
    r.t->add("GET", "/api/sources", 200, R"({"sources":[{"id":7},{"name":"x","enabled":false,"future":1}]})");
    auto src = r.api.listSources();
    CHECK(src.ok() && src.value.size() == 2 && src.value[0].enabled && src.value[0].deletable && !src.value[1].enabled);
    r.t->add("GET", "/api/mounts", 200, R"({"mounts":[{"path":"/data"}]})");
    auto m = r.api.getMounts();
    CHECK(m.ok() && m.value.size() == 1 && m.value[0].writable); // writable !== false => true
    r.t->add("GET", "/api/queue/x", 200, "{}");
    // Empty 2xx body is `{}` (app.js:608).
    r.t->add("GET", "/api/files/operations", 200, "");
    auto ops = r.api.fileOperations();
    CHECK(ops.ok() && ops.value.empty());
}

TEST(errors_surface_status_and_message)
{
    Rig r;
    r.t->add("POST", "/api/queue/pause", 400, R"({"error":"Pausable job not found"})");
    auto p = r.api.pauseJob(999999);
    CHECK(!p.ok());
    CHECK(p.error.kind == ApiError::Kind::http);
    CHECK_EQ_INT(p.error.status, 400);
    CHECK_EQ_STR(p.error.message, "Pausable job not found");
    CHECK(!p.error.route_missing());

    r.t->add("POST", "/api/queue/resume", 500, "<html>oops</html>"); // non-JSON error body
    auto q = r.api.resumeJob(1);
    CHECK(!q.ok() && q.error.status == 500 && q.error.message == "HTTP 500");

    r.t->add("GET", "/api/health", 200, "{not json");
    auto h = r.api.health();
    CHECK(!h.ok() && h.error.kind == ApiError::Kind::parse && h.error.status == 200);

    r.t->add_failure("GET", "/api/mounts", "connection refused");
    auto m = r.api.getMounts();
    CHECK(!m.ok() && m.error.kind == ApiError::Kind::transport && m.error.status == 0 && m.error.message == "connection refused");

    auto missing = r.api.getLogs(0); // unscripted => 404 Route not found
    CHECK(!missing.ok() && missing.error.route_missing() && missing.error.status == 404);
}

TEST(sample_sources_and_default_sources)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/sources", sample("sources.json")));
    auto s = r.api.listSources();
    CHECK(s.ok() && s.value.size() == 5);
    CHECK_EQ_INT(s.value[0].id, 1);
    CHECK_EQ_STR(s.value[0].name, "evoX-CoreOS");
    CHECK_EQ_STR(s.value[0].kind, "url");
    CHECK(s.value[0].managed && !s.value[0].deletable && s.value[0].hasCatalog && s.value[0].enabled);
    CHECK_EQ_STR(s.value[0].managedId, "evox-coreos");
    CHECK_EQ_INT(s.value[0].packageCount, 17);
    CHECK_EQ_INT(s.value[0].skippedCount, 1);
    CHECK_EQ_STR(s.value[0].status, "Loaded 17 packages");
    CHECK_EQ_INT(s.value[1].packageCount, 1452);

    CHECK(r.t->add_file("GET", "/api/default-sources", sample("default-sources.json")));
    auto d = r.api.getDefaultSources(false);
    CHECK(d.ok());
    CHECK_EQ_STR(d.value.status, "remote");
    CHECK(d.value.available && !d.value.onboardingRequired && !d.value.hasNew);
    CHECK_EQ_INT(d.value.revision, 2);
    CHECK_EQ_INT(d.value.schemaVersion, 1);
    CHECK_EQ_INT(d.value.sources.size(), 5);
    CHECK_EQ_STR(d.value.sources[1].id, "dlps");
    CHECK(d.value.sources[1].defaultEnabled && d.value.sources[1].configured && d.value.sources[1].enabled);
    CHECK_EQ_INT(d.value.sources[1].sourceId, 2);
    CHECK_EQ_INT(d.value.sources[4].packageCount, 57);
    CHECK_EQ_STR(r.t->last().path, "/api/default-sources");
    r.t->add("GET", "/api/default-sources?refresh=1", 200, "{}");
    (void)r.api.getDefaultSources(true);
    CHECK_EQ_STR(r.t->last().path, "/api/default-sources?refresh=1");
}

TEST(sample_library_7mb_parses_fast)
{
    Rig r;
    const double load0 = testing::now_ms();
    CHECK(r.t->add_file("GET", "/api/library", sample("library.json")));
    const double load1 = testing::now_ms();
    const double t0 = testing::now_ms();
    auto lib = r.api.getLibrary();
    const double parse_ms = testing::now_ms() - t0;
    std::printf("    library: %zu packages, file load %.0f ms, adapter parse %.0f ms\n", lib.value.size(), load1 - load0, parse_ms);
    CHECK(lib.ok());
    CHECK_EQ_INT(lib.value.size(), 2962);
    size_t links = 0;
    for (const Package &p : lib.value)
    {
        links += p.downloadLinks.size();
        CHECK_MSG(p.downloadLinkCount == static_cast<std::int64_t>(p.downloadLinks.size()), p.title);
        if (testing::failures() > 20)
            break;
    }
    CHECK_EQ_INT(links, 35559);
    const Package &first = lib.value.front();
    CHECK_EQ_INT(first.id, 1);
    CHECK_EQ_INT(first.sourceId, 1);
    CHECK_EQ_STR(first.sourceName, "evoX-CoreOS");
    CHECK_EQ_STR(first.titleId, "LAPY20016");
    CHECK_EQ_STR(first.title, "Avatar Changer PS5");
    CHECK_EQ_STR(first.version, "Source-Fixe");
    CHECK(first.posterUrl.find("Avatar Changer PS5.png") != std::string::npos);
    CHECK_EQ_STR(first.description, "Avatar-Changer.");
    CHECK_EQ_INT(lib.value.back().id, 2962);
    // Find a package with links and check them.
    const Package *with_links = nullptr;
    for (const Package &p : lib.value)
        if (!p.downloadLinks.empty())
        {
            with_links = &p;
            break;
        }
    CHECK(with_links != nullptr);
    if (with_links)
    {
        CHECK(with_links->downloadLinks[0].hasIndex && with_links->downloadLinks[0].index == 0);
        CHECK(!with_links->downloadLinks[0].url.empty() && !with_links->downloadLinks[0].name.empty());
    }
    // The 7 MB budget: generous bound so slow CI does not flake, but catches O(n^2).
    CHECK_MSG(parse_ms < 3000.0, "library parse took " + std::to_string(parse_ms) + " ms");
}

TEST(sample_queue)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/queue", sample("queue.json")));
    auto q = r.api.getQueue();
    CHECK(q.ok() && q.value.jobs.empty());
    CHECK_EQ_INT(q.value.summary.total, 0);

    // A completed job with the full field set.
    CHECK(r.t->add_file("GET", "/api/queue", M("queue_after.json")));
    r.t->add_file("GET", "/api/queue", M("queue_after.json"));
    q = r.api.getQueue();
    q = r.api.getQueue();
    CHECK(q.ok() && q.value.jobs.size() == 1);
    const QueueJob &j = q.value.jobs[0];
    CHECK_EQ_INT(j.id, 1);
    CHECK_EQ_INT(j.packageId, 0);
    CHECK_EQ_STR(j.downloadLinkName, "Direct link");
    CHECK_EQ_STR(j.title, "wings-test-readme.md");
    CHECK_EQ_STR(j.sourceName, "Manual");
    CHECK_EQ_STR(j.fileName, "wings-test-readme.md");
    CHECK(j.status == JobStatus::completed);
    CHECK_EQ_STR(j.statusRaw, "completed");
    CHECK_EQ_STR(j.detail, "Completed");
    CHECK_EQ_STR(j.destinationPath, "/data/pegasus-dl/downloads/wings-test-readme.md");
    CHECK_EQ_STR(j.contentKind, "file");
    CHECK_EQ_STR(j.archiveFormat, "none");
    CHECK(!j.streamMode && j.hasStreamMode);
    CHECK_EQ_INT(j.sizeBytes, 9870);
    CHECK_EQ_INT(j.bytesDone, 9870);
    CHECK(j.progress == 1.0 && j.phaseProgress == 1.0);
    CHECK(j.phase == JobPhase::download);
    CHECK_EQ_INT(j.phaseBytesDone, 9870);
    CHECK_EQ_INT(j.phaseTotalBytes, 9870);
    CHECK_EQ_INT(j.attempts, 1);
    CHECK_EQ_INT(j.maxAttempts, 3);
    CHECK(j.supportsResume && !j.resumed);
    CHECK_EQ_INT(j.createdAt, 1791689074);
    CHECK_EQ_INT(j.finishedAt, 1791689075);
    CHECK_EQ_INT(q.value.summary.completed, 1);
    CHECK_EQ_INT(q.value.summary.total, 1);
}

TEST(sample_queue_direct_dispatch)
{
    Rig r;
    CHECK(r.t->add_file("POST", "/api/queue/direct", M("queue_direct.json")));
    auto d = r.api.queueDirect("https://raw.githubusercontent.com/pegasus-ps5/pegasus-dl/main/README.md", "wings-test-readme.md",
                               "text/plain; charset=utf-8");
    CHECK(d.ok());
    CHECK(d.value.hasQueue);
    CHECK_EQ_INT(d.value.queue.summary.queued, 1);
    CHECK(d.value.queue.jobs.size() == 1 && d.value.queue.jobs[0].status == JobStatus::queued);
    CHECK(d.value.queue.jobs[0].phase == JobPhase::none);
    CHECK_EQ_INT(d.value.dispatchResults.size(), 1);
    const DispatchResult &dr = d.value.dispatchResults[0];
    CHECK(dr.ok && dr.kind == DispatchKind::queued && dr.jobId == 1);
    CHECK_EQ_STR(dr.fileName, "wings-test-readme.md");
    CHECK_EQ_STR(dr.message, "Download added to the Pegasus DL queue.");
    CHECK_EQ_INT(logic::queuedDispatchResults(d.value).size(), 1);
    CHECK_EQ_INT(logic::nativeDispatchResults(d.value).size(), 0);
    auto out = logic::finishDispatchResponse(d.value, "Direct link queued.");
    CHECK(out.queueIncluded && !out.needsQueueReload && out.schedulePoll);
    CHECK_EQ_STR(out.toast, "Direct link queued.");
}

TEST(sample_appdb)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/appdb", sample("appdb.json")));
    auto a = r.api.getInstalledGames();
    CHECK(a.ok());
    CHECK(a.value.databaseAvailable);
    CHECK_EQ_INT(a.value.count, 15);
    CHECK_EQ_INT(a.value.titles.size(), 15);
    const InstalledGame &g = a.value.titles[0];
    CHECK_EQ_STR(g.titleId, "PPSA27366");
    CHECK_EQ_STR(g.titleName, "AI LIMIT");
    CHECK_EQ_STR(g.iconUrl, "/appdb/icon?id=PPSA27366");
    CHECK_EQ_STR(g.pic0Url, "/appdb/pic0?id=PPSA27366");
    CHECK_EQ_STR(g.installPath, "/user/app/PPSA27366");
    CHECK_EQ_INT(g.installedTime, 1791053174);
    CHECK_EQ_STR(g.contentId, "HP4226-PPSA27366_00-0697535294250993");
    CHECK_EQ_STR(g.contentVersion, "01.000.034");
    CHECK_EQ_STR(g.sdkVersion, "0x0900000000000000");
    CHECK_EQ_STR(g.requiredSystemSoftwareVersion, "0x1400000000000000");
    CHECK_EQ_STR(logic::installedFirmwareText(g.requiredSystemSoftwareVersion), "FW 14.00");
    CHECK(g.hasIcon && g.hasPic0);
    CHECK(!g.aprEmu.present && !g.aprEmu.canUpdate && !g.aprEmu.remoteAvailable);
    CHECK_EQ_STR(g.aprEmu.reason, "No installed storage link found");
    CHECK(logic::aprEmuState(g.aprEmu) == logic::AprState::missing);
    // A disk-image title with storage info.
    const InstalledGame *img = nullptr;
    for (const InstalledGame &t : a.value.titles)
        if (t.titleId == "PPSA02015")
            img = &t;
    CHECK(img != nullptr);
    if (img)
    {
        CHECK_EQ_STR(img->storageKind, "image");
        CHECK_EQ_STR(img->format, ".exfat");
        CHECK_EQ_STR(img->backingPath, "/mnt/ext0/homebrew/PPSA02015.exfat");
        CHECK_EQ_INT(img->sizeBytes, 170804641792LL);
        CHECK_EQ_STR(logic::installedLocationText(*img), "/mnt/ext0/homebrew/PPSA02015.exfat");
    }
    // Titles missing optional metadata still parse (4 of 15 lack contentId).
    int bare = 0;
    for (const InstalledGame &t : a.value.titles)
        if (t.contentId.empty())
            ++bare;
    CHECK_EQ_INT(bare, 4);

    // database errors are surfaced (app.js:4339)
    r.t->set("GET", "/appdb", 200, R"({"databaseAvailable":false,"databaseError":"app.db busy","titles":[]})");
    auto bad = r.api.getInstalledGames();
    CHECK(!bad.ok());
    CHECK_EQ_STR(bad.error.message, "app.db busy");
    r.t->set("GET", "/appdb", 200, R"({"databaseAvailable":false})");
    CHECK_EQ_STR(r.api.getInstalledGames().error.message, "Unable to read the PS5 app database.");
}

TEST(sample_apr_versions_503_is_an_error_not_a_crash)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/apr-emu/versions", sample("apr-emu_versions.json"), 503));
    auto v = r.api.getAprVersions();
    CHECK(!v.ok());
    CHECK(v.error.kind == ApiError::Kind::http);
    CHECK_EQ_INT(v.error.status, 503);
    CHECK_EQ_STR(v.error.message, "APR Emu manifest has too many versions");
    CHECK(!v.error.route_missing());
    CHECK(v.value.versions.empty());

    r.t->set("GET", "/api/apr-emu/versions", 200,
             R"({"latestReleaseId":"r2","versions":[{"id":"r2","version":"1.2.0","build":"release","size":1048576,"extra":1},{"id":"d2","version":"1.2.0","build":"debug"}]})");
    auto ok = r.api.getAprVersions();
    CHECK(ok.ok() && ok.value.versions.size() == 2);
    CHECK_EQ_STR(ok.value.latestReleaseId, "r2");
    CHECK_EQ_INT(ok.value.versions[0].size, 1048576);
    CHECK_EQ_STR(ok.value.versions[1].build, "debug");
}

TEST(sample_storage_files_logs_capture)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/mounts", sample("mounts.json")));
    auto m = r.api.getMounts();
    CHECK(m.ok() && m.value.size() == 2);
    CHECK_EQ_STR(m.value[0].path, "/data");
    CHECK_EQ_STR(m.value[0].source, "/user/data");
    CHECK_EQ_STR(m.value[0].type, "nullfs");
    CHECK(m.value[0].writable);
    CHECK_EQ_INT(m.value[0].totalBytes, 671222792192LL);
    CHECK_EQ_INT(m.value[1].freeBytes, 25769009152LL);

    CHECK(r.t->add_file("GET", "/api/browse?path=%2Fdata", sample("browse_path__data.json")));
    auto b = r.api.browse("/data");
    CHECK(b.ok());
    CHECK_EQ_STR(r.t->last().path, "/api/browse?path=%2Fdata");
    CHECK_EQ_STR(b.value.path, "/data");
    CHECK_EQ_INT(b.value.entries.size(), 19);
    CHECK_EQ_STR(b.value.entries[0].name, "logs");
    CHECK_EQ_STR(b.value.entries[0].type, "dir");
    const FileEntry *f = nullptr;
    for (const FileEntry &e : b.value.entries)
        if (e.name == "dpi.port")
            f = &e;
    CHECK(f && f->type == "file" && f->sizeBytes == 5);

    CHECK(r.t->add_file("GET", "/api/files/locations", sample("files_locations.json")));
    auto loc = r.api.fileLocations();
    CHECK(loc.ok() && loc.value.size() == 1240);
    CHECK_EQ_STR(loc.value[0].path, "/");
    CHECK_EQ_STR(loc.value[0].name, "Root");
    CHECK(!loc.value[0].writable);
    CHECK_EQ_STR(loc.value[0].type, "exfatfs");

    CHECK(r.t->add_file("GET", "/api/files/operations", sample("files_operations.json")));
    auto ops = r.api.fileOperations();
    CHECK(ops.ok() && ops.value.empty());

    CHECK(r.t->add_file("GET", "/api/logs?since=0", sample("logs_since_0.json")));
    auto logs = r.api.getLogs(0);
    CHECK(logs.ok());
    CHECK_EQ_INT(logs.value.logs.size(), 70);
    CHECK_EQ_INT(logs.value.count, 70);
    CHECK_EQ_INT(logs.value.capacity, 1024);
    CHECK_EQ_INT(logs.value.lastSeq, 70);
    CHECK_EQ_INT(logs.value.logs[0].seq, 1);
    CHECK_EQ_STR(logs.value.logs[0].level, "info");
    CHECK_EQ_STR(logs.value.logs[0].component, "config");
    CHECK_EQ_STR(logs.value.logs[0].message, "loaded 5 saved sources, skipped 0");

    CHECK(r.t->add_file("GET", "/api/browser/capture/active", sample("browser_capture_active.json")));
    auto cap = r.api.captureActive();
    CHECK(cap.ok() && !cap.value.active);
    CHECK(cap.value.state == CaptureState::idle);
    CHECK_EQ_INT(cap.value.sessionId, 0);
    CHECK_EQ_INT(cap.value.pendingCandidates, 0);
    CHECK(cap.value.dispatch.dispatchResults.empty());

    // The recorded GET /api/debrid/queue is a 404 (the route is POST-only): route_missing signature.
    CHECK(r.t->add_file("GET", "/api/debrid/queue", sample("debrid_queue.json"), 404));
    r.t->add("POST", "/api/debrid/queue", 404, R"({"error":"Route not found"})");
    auto dq = r.api.debridQueue(1, 0, DebridService::torBox);
    CHECK(!dq.ok() && dq.error.route_missing());
}

TEST(sample_mutating_responses)
{
    Rig r;
    CHECK(r.t->add_file("POST", "/api/files/mkdir", M("files_mkdir.json")));
    auto mk = r.api.fileMkdir("/data", "wings-test");
    CHECK(mk.ok());
    CHECK_EQ_STR(mk.value.path, "/data/wings-test");
    CHECK_EQ_STR(mk.value.name, "wings-test");
    CHECK(mk.value.directory && !mk.value.file && !mk.value.symlink && mk.value.writable);
    CHECK_EQ_INT(mk.value.mode, 511);
    CHECK_EQ_INT(mk.value.sizeBytes, 65536);
    CHECK_EQ_INT(mk.value.freeBytes, 290740502528LL);

    CHECK(r.t->add_file("POST", "/api/files/rename", M("files_rename.json")));
    auto rn = r.api.fileRename("/data/wings-test/hello.txt", "hi.txt");
    CHECK(rn.ok() && rn.value.name == "hi.txt" && rn.value.file && rn.value.sizeBytes == 5);

    CHECK(r.t->add_file("PUT", "/api/files/upload?directory=%2Fdata%2Fwings-test&name=hello.txt", M("files_upload.json")));
    auto up = r.api.fileUpload("/data/wings-test", "hello.txt", "", "wings");
    CHECK(up.ok());

    CHECK(r.t->add_file("GET", "/api/files/list?path=%2Fdata%2Fwings-test", M("files_list.json")));
    auto ls = r.api.fileList("/data/wings-test");
    CHECK(ls.ok());
    CHECK_EQ_STR(ls.value.path, "/data/wings-test");
    CHECK(ls.value.writable);
    CHECK_EQ_INT(ls.value.entries.size(), 2);
    CHECK_EQ_STR(ls.value.entries[0].name, "a");
    CHECK(ls.value.entries[0].directory);
    CHECK_EQ_STR(ls.value.entries[1].path, "/data/wings-test/hello.txt");

    CHECK(r.t->add_file("POST", "/api/files/copy", M("files_copy.json")));
    auto cp = r.api.fileCopy({"/data/wings-test/hi.txt"}, "/data/wings-test/a", Collision::keepBoth);
    CHECK(cp.ok() && cp.value.size() == 5);
    const FileOperation &op = cp.value[0];
    CHECK_EQ_INT(op.operationId, 1);
    CHECK_EQ_STR(op.type, "copy");
    CHECK(op.status == FileOpStatus::completed);
    CHECK_EQ_STR(op.collision, "keep-both");
    CHECK_EQ_STR(op.source, "/data/wings-test/hi.txt");
    CHECK_EQ_STR(op.destination, "/data/wings-test/a");
    CHECK_EQ_INT(op.totalItems, 1);
    CHECK_EQ_INT(op.doneBytes, 5);
    CHECK(!op.cancelRequested && op.cancelable);
    CHECK(logic::isTerminalFileOp(op.status));
    CHECK_EQ_STR(cp.value[2].type, "delete");

    CHECK(r.t->add_file("POST", "/api/files/move", M("files_move.json")));
    CHECK(r.api.fileMove({"/data/wings-test/a/hi.txt"}, "/data/wings-test", Collision::keepBoth).value.size() == 6);
    CHECK(r.t->add_file("POST", "/api/files/delete", M("files_delete.json")));
    CHECK(r.api.fileDelete({"/data/wings-test"}).value.size() == 7);

    // POST /api/files/archive is not a route (GET only) - the sample records the 404.
    r.t->add_file("POST", "/api/files/archive", M("files_archive.json"));
    r.t->add("GET", "/api/files/archive?path=%2Fdata%2Fwings-test%2Fa", 200, "PK\x03\x04...zipbytes", "application/zip");
    auto zip = r.api.fileArchive("/data/wings-test/a", 1 << 20);
    CHECK(zip.ok() && zip.value.rfind("PK", 0) == 0);
    r.t->add("GET", "/api/files/download?path=%2Fdata%2Fwings-test%2Fhi.txt", 200, "wings", "application/octet-stream");
    auto dl = r.api.fileDownload("/data/wings-test/hi.txt", 1 << 20);
    CHECK(dl.ok() && dl.value == "wings");
    CHECK_EQ_STR(r.api.fileDownloadPath("/a b", false), "/api/files/download?path=%2Fa%20b");
    CHECK_EQ_STR(r.api.fileDownloadPath("/a b", true), "/api/files/archive?path=%2Fa%20b");

    CHECK(r.t->add_file("POST", "/api/resolve/probe", M("resolve_probe.json")));
    auto pr = r.api.resolveProbe("https://example.com/");
    CHECK(pr.ok());
    CHECK_EQ_STR(pr.value.requestedUrl, "https://example.com/");
    CHECK_EQ_STR(pr.value.finalUrl, "https://example.com/");
    CHECK_EQ_INT(pr.value.status, 200);
    CHECK_EQ_STR(pr.value.contentType, "text/html; charset=utf-8");
    CHECK(pr.value.isHtml && !pr.value.isDownload && !pr.value.hasContentLength);
    CHECK_EQ_STR(logic::probeRejection(pr.value), "Link points to a web page, not a direct download.");

    CHECK(r.t->add_file("POST", "/api/queue/pause", M("queue_pause_missing.json")));
    auto pm = r.api.pauseJob(999999);
    CHECK(!pm.ok() && pm.error.status == 400 && pm.error.message == "Pausable job not found");

    CHECK(r.t->add_file("POST", "/api/sources/upload", M("sources_upload.json")));
    auto su = r.api.uploadSourceCatalog("{}");
    CHECK(su.ok() && su.value.size() == 6);
    CHECK_EQ_STR(su.value[5].name, "Wings Contract Test");
    CHECK_EQ_STR(su.value[5].kind, "upload");
    CHECK(r.t->add_file("POST", "/api/sources/enabled", M("sources_enabled.json")));
    auto se = r.api.setSourceEnabled(7, false);
    CHECK(se.ok() && se.value.size() == 6 && !se.value[5].enabled);
    CHECK(r.t->add_file("POST", "/api/sources/delete", M("sources_delete.json")));
    auto sd = r.api.deleteSource(7);
    CHECK(sd.ok() && sd.value.size() == 5);
}
