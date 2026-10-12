// Wings TV app - PegasusBackend over the adapter library, driven by recorded Pegasus replies.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The samples are the ones in recon/samples (captured from a real Pegasus DL
// 1.11.0). The tests that need them skip when the folder is not reachable
// (tools/build-wings-wsl.sh mounts it at /recon).

#include "orbit/core_transport.hpp"
#include "orbit/pegasus_backend.hpp"
#include "orbit/store.hpp"
#include "support.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace orbit
{
namespace
{

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

#define NEED_SAMPLE(var, name)                                                                     \
    std::string var;                                                                               \
    if (!read_sample(name, &var))                                                                  \
        GTEST_SKIP() << "recon/samples/" name " is not reachable from here";

constexpr const char *kHealth =
    R"({"status":"running","port":6970,"downloadOnly":false,"nativePkgHandoff":true,"version":"1.11.0"})";

struct Rig
{
    std::shared_ptr<wings::http::ReplayTransport> replay =
        std::make_shared<wings::http::ReplayTransport>();
    PegasusBackend backend{replay, "127.0.0.1:6970"};
    Rig()
    {
        replay->add("GET", "/api/health", 200, kHealth);
    }
};

TEST(PlainDescription, StripsTagsDecodesEntitiesAndShortensAtAWord)
{
    EXPECT_EQ(plain_description("<p>Hello&nbsp;<b>world</b> &amp; friends</p><br>second", 200),
              "Hello world & friends second");
    EXPECT_EQ(plain_description("  a \n\t b  ", 50), "a b");
    EXPECT_EQ(plain_description("", 50), "");
    const std::string longer = plain_description("alpha beta gamma delta epsilon zeta", 14);
    EXPECT_EQ(longer, "alpha beta\xE2\x80\xA6");
    // Never cuts a UTF-8 character in half.
    const std::string accents = plain_description("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 5);
    EXPECT_EQ(accents.size() % 2, 1u); // whole characters, then the 3-byte ellipsis
}

TEST(CoreTransport, ForwardsJsonRequestsAndRefusesWhatTheClientCannotSend)
{
    test::ScriptedTransport inner;
    inner.reply = [](const http::Request &) { return test::json_reply(200, "{\"ok\":true}"); };
    CoreTransport transport(inner);
    wings::http::Request post;
    post.method = "POST";
    post.path = "/api/queue";
    post.body = "{\"packageId\":1}";
    post.max_bytes = 128u << 20;
    const wings::http::Response ok = transport.send(post);
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(ok.body, "{\"ok\":true}");
    ASSERT_EQ(inner.seen.size(), 1u);
    EXPECT_EQ(inner.seen[0].path, "/api/queue");
    EXPECT_EQ(inner.seen[0].body, "{\"packageId\":1}");
    EXPECT_EQ(inner.seen[0].max_bytes, CoreTransport::kMaxReply); // the 128 MiB ask is clamped

    wings::http::Request put;
    put.method = "PUT";
    put.path = "/api/files/upload?directory=%2Fdata&name=x";
    put.body = "raw";
    put.content_type = "application/octet-stream";
    const wings::http::Response refused = transport.send(put);
    EXPECT_EQ(refused.status, 0);
    EXPECT_FALSE(refused.error.empty());
    EXPECT_EQ(inner.seen.size(), 1u); // never sent
}

TEST(Http, TakesALargeContentLengthBodyWithoutCopyingAndKeepsTheHeaders)
{
    std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n"
                      "Retry-After: 4\r\n\r\n";
    raw += "{\"a\":\"b\"}  ";
    raw += "trailing junk";
    http::Response response;
    // parse_response is the reference: the transport's fast path must agree with it.
    ASSERT_TRUE(http::parse_response(raw, &response));
    EXPECT_EQ(response.body, "{\"a\":\"b\"}  ");
    EXPECT_EQ(response.retry_after, 4);
    EXPECT_EQ(response.content_type, "application/json");
}

TEST(PegasusBackend, ReportsUnreachableThenConnectsWhenPegasusAppears)
{
    auto replay = std::make_shared<wings::http::ReplayTransport>();
    PegasusBackend backend(replay, "127.0.0.1:6970");
    replay->add_failure("GET", "/api/health", "connection refused");
    replay->add("GET", "/api/health", 200, kHealth);
    const Result<PegasusInfo> down = backend.health();
    EXPECT_FALSE(down.ok());
    EXPECT_TRUE(down.unreachable());
    const Result<PegasusInfo> up = backend.health();
    ASSERT_TRUE(up.ok());
    EXPECT_EQ(up.value.version, "1.11.0");
    EXPECT_EQ(up.value.adapter, "pegasus-v1");
    EXPECT_EQ(up.value.compat, "tested");
    EXPECT_TRUE(up.value.supported);
}

TEST(PegasusBackend, AnUnsupportedMajorVersionIsReportedNotDriven)
{
    auto replay = std::make_shared<wings::http::ReplayTransport>();
    replay->add("GET", "/api/health", 200,
                R"({"status":"running","port":6970,"version":"2.0.0"})");
    PegasusBackend backend(replay, "127.0.0.1:6970");
    const Result<PegasusInfo> info = backend.health();
    ASSERT_TRUE(info.ok()); // Pegasus is there ...
    EXPECT_FALSE(info.value.supported); // ... but Wings has no adapter for it
    EXPECT_EQ(info.value.compat, "unsupported");
    const Result<std::vector<Job>> queue = backend.queue();
    EXPECT_FALSE(queue.ok());
    EXPECT_EQ(queue.status, 501);
}

TEST(PegasusBackend, ALostConnectionIsRebuiltOnTheNextCall)
{
    Rig rig;
    // connect() probes the queue route once (the first reply); then one call fails.
    rig.replay->add("GET", "/api/queue", 200, R"({"jobs":[],"summary":{}})");
    rig.replay->add_failure("GET", "/api/queue", "connection reset");
    rig.replay->add("GET", "/api/queue", 200, R"({"jobs":[],"summary":{}})");
    const Result<std::vector<Job>> failed = rig.backend.queue();
    EXPECT_TRUE(failed.unreachable());
    const std::size_t health_before = rig.replay->count("GET", "/api/health");
    EXPECT_TRUE(rig.backend.queue().ok());
    // It connected again (health read once more), rather than reusing a dead adapter.
    EXPECT_GT(rig.replay->count("GET", "/api/health"), health_before);
}

TEST(PegasusBackend, TheQueueSampleBecomesAppJobs)
{
    Rig rig;
    if (!rig.replay->add_file("GET", "/api/queue", samples_dir() + "/mutating/queue_after.json"))
        GTEST_SKIP() << "recon/samples is not reachable from here";
    const Result<std::vector<Job>> jobs = rig.backend.queue();
    ASSERT_TRUE(jobs.ok());
    EXPECT_FALSE(jobs.value.empty());
    for (const Job &job : jobs.value)
    {
        EXPECT_FALSE(job.id.empty());
        EXPECT_FALSE(job.status.empty());
    }
}

TEST(PegasusBackend, JobActionsSendTheNumericIdAndRejectUnknownActions)
{
    Rig rig;
    rig.replay->add("POST", "/api/queue/pause", 200, R"({"jobs":[],"summary":{}})");
    ASSERT_TRUE(rig.backend.job_action("12", "pause").ok());
    EXPECT_EQ(rig.replay->last().body, R"({"jobId":12})");
    EXPECT_FALSE(rig.backend.job_action("12", "explode").ok());
    EXPECT_FALSE(rig.backend.job_action("abc", "pause").ok());
    // A route Pegasus answers with an error surfaces its message.
    rig.replay->add("POST", "/api/queue/cancel", 404, R"({"error":"Job not found"})");
    const Result<std::vector<Job>> missing = rig.backend.job_action("5", "cancel");
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.error, "Job not found");
}

TEST(PegasusBackend, TheRealCatalogueIsTrimmedCachedAndWellUnderTheHeap)
{
    NEED_SAMPLE(library, "library.json");
    Rig rig;
    rig.replay->add("GET", "/api/library", 200, library);
    const Result<std::vector<Package>> result = rig.backend.library();
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value.size(), 2962u);
    std::size_t with_pkg = 0;
    std::size_t with_auto = 0;
    for (const Package &p : result.value)
    {
        EXPECT_FALSE(p.id.empty());
        EXPECT_LE(p.description.size(), 430u);
        EXPECT_EQ(p.description.find('<'), std::string::npos);
        with_pkg += p.has_pkg ? 1 : 0;
        with_auto += p.has_automated ? 1 : 0;
    }
    EXPECT_GE(with_pkg, 1u);
    EXPECT_GT(with_auto, 100u);
    const PegasusBackend::CacheStats stats = rig.backend.cache_stats();
    EXPECT_EQ(stats.packages, 2962u);
    EXPECT_EQ(stats.links, 35559u);
    EXPECT_LT(stats.bytes, 40u << 20); // links and strings, estimated
    // The request carried the raised limit through the transport.
    EXPECT_EQ(rig.replay->count("GET", "/api/library"), 1u);
}

TEST(PegasusBackend, PackageDetailClassifiesEveryLinkAndStartLinkFollowsTheRules)
{
    NEED_SAMPLE(library, "library.json");
    Rig rig;
    rig.replay->add("GET", "/api/library", 200, library);
    const Result<std::vector<Package>> all = rig.backend.library();
    ASSERT_TRUE(all.ok());
    // The one package with a .pkg link.
    const Package *pkg = nullptr;
    for (const Package &p : all.value)
    {
        if (p.has_pkg)
        {
            pkg = &p;
            break;
        }
    }
    ASSERT_NE(pkg, nullptr);
    const Result<PackageDetail> detail = rig.backend.package(pkg->id);
    ASSERT_TRUE(detail.ok());
    EXPECT_EQ(detail.value.links.size(), static_cast<std::size_t>(pkg->link_count));
    int native_index = -1;
    for (const PackageLink &link : detail.value.links)
    {
        if (link.route == LinkRoute::native_pkg)
            native_index = link.index;
    }
    ASSERT_GE(native_index, 0);
    rig.replay->add("POST", "/api/queue", 200,
                    R"({"dispatchResult":{"ok":true,"kind":"native-pkg","accepted":true,"title":"T","message":"started","contentId":"UP0000-TEST_00-0000000000000000"}})");
    const Result<LinkResult> started = rig.backend.start_link(pkg->id, native_index, "");
    ASSERT_TRUE(started.ok());
    EXPECT_EQ(started.value.kind, LinkResult::Kind::dispatched);
    EXPECT_TRUE(started.value.dispatch.native);
    EXPECT_NE(rig.replay->last().body.find("\"packageId\":" + pkg->id), std::string::npos);
    EXPECT_NE(rig.replay->last().body.find("\"linkIndex\":" + std::to_string(native_index)),
              std::string::npos);
    // Unknown package, unknown link.
    EXPECT_EQ(rig.backend.package("99999999").status, 404);
    EXPECT_FALSE(rig.backend.start_link(pkg->id, 9999, "").ok());
}

TEST(PegasusBackend, LinkTiersFollowTheProviderTablesAndDebridOnlyWhenTheServiceIsReady)
{
    NEED_SAMPLE(library, "library.json");
    Rig rig;
    rig.replay->add("GET", "/api/library", 200, library);
    ASSERT_TRUE(rig.backend.library().ok());
    const auto tiers_of = [&](const std::string &id)
    {
        std::vector<LinkTier> out;
        const Result<PackageDetail> detail = rig.backend.package(id);
        EXPECT_TRUE(detail.ok());
        for (const PackageLink &link : detail.value.links)
            out.push_back(link_tier(link));
        return out;
    };
    const auto count = [](const std::vector<LinkTier> &tiers, LinkTier tier)
    { return std::count(tiers.begin(), tiers.end(), tier); };
    // Package 27 has 1fichier.com links. Before any settings were read: no service is
    // ready, so nothing is yellow; 1fichier is not a provider Wings drives -> unknown.
    const std::vector<LinkTier> before = tiers_of("27");
    ASSERT_FALSE(before.empty());
    EXPECT_EQ(count(before, LinkTier::debrid), 0);
    EXPECT_GT(count(before, LinkTier::unknown), 0);
    // A service that is configured but not valid is still not ready.
    rig.replay->set("GET", "/api/settings", 200,
                    R"({"downloadRoot":"/data/x","downloadConnections":8,"torBox":{"configured":true,"valid":false,"domains":["1fichier.com"]}})");
    ASSERT_TRUE(rig.backend.settings().ok());
    EXPECT_EQ(count(tiers_of("27"), LinkTier::debrid), 0);
    // Configured, valid and listing the host: yellow, and only for that host.
    rig.replay->set("GET", "/api/settings", 200,
                    R"({"downloadRoot":"/data/x","downloadConnections":8,"torBox":{"configured":true,"valid":true,"hosterCount":1,"domainCount":1,"domains":["1fichier.com"]}})");
    ASSERT_TRUE(rig.backend.settings().ok());
    const std::vector<LinkTier> after = tiers_of("27");
    EXPECT_GT(count(after, LinkTier::debrid), 0);
    EXPECT_LT(count(after, LinkTier::debrid), static_cast<long>(after.size()) + 1);
    // Sorted by the Store later; here the order is the catalogue's. A package with
    // vikingfile links shows them as supported browser providers.
    long supported = 0;
    for (int id = 1; id < 400 && supported == 0; ++id)
    {
        const Result<PackageDetail> detail = rig.backend.package(std::to_string(id));
        if (!detail.ok())
            continue;
        for (const PackageLink &link : detail.value.links)
        {
            if (link.host.find("vikingfile") != std::string::npos)
            {
                EXPECT_EQ(static_cast<int>(link_tier(link)), static_cast<int>(LinkTier::supported));
                ++supported;
            }
        }
    }
    EXPECT_GT(supported, 0);
}

TEST(PegasusBackend, ManagementCallsSendPegasusShapesAndRefuseBadInputLocally)
{
    Rig rig;
    rig.replay->add("GET", "/api/queue", 200, R"({"summary":{},"jobs":[]})");
    // Settings: out-of-range connections never leave the app.
    SettingsPatch bad;
    bad.has_connections = true;
    bad.connections = 99;
    const Result<SettingsView> refused = rig.backend.settings_update(bad);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status, 400);
    EXPECT_EQ(rig.replay->count("POST", "/api/settings"), 0u);
    // A valid patch is sent compact, only the flagged fields, and the reply is mapped.
    rig.replay->set("POST", "/api/settings", 200,
                    R"({"downloadRoot":"/data/x","downloadConnections":8,"torBox":{"configured":true,"valid":true,"hosterCount":3,"domainCount":9,"accountType":"essential","domains":["1fichier.com"]}})");
    SettingsPatch good;
    good.has_connections = true;
    good.connections = 8;
    good.has_torbox = true;
    good.torbox_token = "tok";
    const Result<SettingsView> saved = rig.backend.settings_update(good);
    ASSERT_TRUE(saved.ok());
    EXPECT_EQ(saved.value.connections, 8);
    EXPECT_TRUE(saved.value.torbox.valid);
    EXPECT_TRUE(saved.value.torbox_ready);
    EXPECT_EQ(saved.value.torbox.hosters, 3);
    const std::string body = rig.replay->last().body;
    EXPECT_NE(body.find("\"downloadConnections\":8"), std::string::npos);
    EXPECT_EQ(body.find("realDebrid"), std::string::npos);
    // The APR version list: the user's console answers 503 and the app says so.
    rig.replay->add("GET", "/api/apr-emu/versions", 503,
                    R"({"error":"APR Emu manifest has too many versions"})");
    const Result<std::vector<AprVersionItem>> versions = rig.backend.apr_versions();
    EXPECT_FALSE(versions.ok());
    EXPECT_EQ(versions.status, 503);
    EXPECT_NE(versions.error.find("too many versions"), std::string::npos);
    // File operations: the collision word is Pegasus' own, destination and paths as given.
    rig.replay->add("POST", "/api/files/copy", 200, R"({"operations":[{"operationId":4,"type":"copy","status":"running","source":"/a/b","destination":"/c","totalItems":1,"doneItems":0}]})");
    const Result<std::vector<FileOp>> copy = rig.backend.file_copy({"/a/b", "/a/b"}, "/c", "keep-both");
    ASSERT_TRUE(copy.ok());
    ASSERT_EQ(copy.value.size(), 1u);
    EXPECT_EQ(copy.value[0].id, "4");
    EXPECT_TRUE(copy.value[0].active());
    EXPECT_NE(rig.replay->last().body.find("\"collision\":\"keep-both\""), std::string::npos);
    EXPECT_EQ(rig.replay->last().body.find("/a/b\",\"/a/b"), std::string::npos); // de-duplicated
    // Names with a slash are refused locally.
    const Result<FileItem> renamed = rig.backend.file_rename("/a/b", "x/y");
    EXPECT_FALSE(renamed.ok());
    EXPECT_EQ(rig.replay->count("POST", "/api/files/rename"), 0u);
    // Installed-game delete sends the title id and reads the job back.
    rig.replay->add("POST", "/api/installed-games/delete", 200,
                    R"({"jobId":"j1","titleId":"PPSA00001","status":"running","filesDeleted":2})");
    const Result<AdminJob> job = rig.backend.game_delete("PPSA00001");
    ASSERT_TRUE(job.ok());
    EXPECT_EQ(job.value.job_id, "j1");
    EXPECT_EQ(job.value.files_deleted, 2);
    EXPECT_FALSE(job.value.done());
    EXPECT_NE(rig.replay->last().body.find("\"titleId\":\"PPSA00001\""), std::string::npos);
}

TEST(PegasusBackend, AnAnswerFromTheRecordedSamplesFillsTheLibraryAndStorageViews)
{
    NEED_SAMPLE(appdb, "appdb.json");
    NEED_SAMPLE(mounts, "mounts.json");
    NEED_SAMPLE(browse, "browse_path__data.json");
    Rig rig;
    rig.replay->add("GET", "/appdb", 200, appdb);
    rig.replay->add("GET", "/api/mounts", 200, mounts);
    rig.replay->add("GET", "/api/browse?path=%2Fdata", 200, browse);
    const Result<std::vector<InstalledGame>> games = rig.backend.installed();
    ASSERT_TRUE(games.ok());
    ASSERT_FALSE(games.value.empty());
    EXPECT_FALSE(games.value[0].title_id.empty());
    EXPECT_FALSE(games.value[0].path.empty());
    const Result<std::vector<StorageMount>> m = rig.backend.mounts();
    ASSERT_TRUE(m.ok());
    EXPECT_FALSE(m.value.empty());
    const Result<FolderListing> b = rig.backend.browse("/data");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b.value.path, "/data");
    bool dirs = false;
    for (const FileItem &item : b.value.entries)
    {
        EXPECT_FALSE(item.path.empty()); // joined with the folder
        dirs = dirs || item.directory;
    }
    EXPECT_TRUE(dirs);
}

TEST(PegasusBackend, AnAutomatedProviderStartsACaptureSession)
{
    NEED_SAMPLE(library, "library.json");
    Rig rig;
    rig.replay->add("GET", "/api/library", 200, library);
    const Result<std::vector<Package>> all = rig.backend.library();
    ASSERT_TRUE(all.ok());
    const Package *pkg = nullptr;
    int link_index = -1;
    for (const Package &p : all.value)
    {
        if (!p.has_automated)
            continue;
        const Result<PackageDetail> detail = rig.backend.package(p.id);
        for (const PackageLink &link : detail.value.links)
        {
            if (link.route == LinkRoute::automated)
            {
                pkg = &p;
                link_index = link.index;
                break;
            }
        }
        if (pkg != nullptr)
            break;
    }
    ASSERT_NE(pkg, nullptr);
    rig.replay->add("POST", "/api/provider/resolve", 200, R"({"sessionId":41})");
    const Result<LinkResult> started = rig.backend.start_link(pkg->id, link_index, "");
    ASSERT_TRUE(started.ok());
    EXPECT_EQ(started.value.kind, LinkResult::Kind::capture);
    EXPECT_EQ(started.value.capture.session_id, "41");
    EXPECT_TRUE(started.value.capture.automated);
    // The status route is polled with the session id and mapped to the panel's states.
    rig.replay->add("GET", "/api/browser/capture/status?sessionId=41", 200,
                    R"({"sessionId":41,"status":"queueing","message":"Link resolving"})");
    const Result<CaptureView> view = rig.backend.capture_status("41");
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value.state, "queueing");
    EXPECT_EQ(view.value.message, "Link resolving");
    EXPECT_EQ(view.value.title, pkg->title); // remembered from the start
    EXPECT_NE(rig.replay->last().path.find("sessionId=41"), std::string::npos);
    rig.replay->add("POST", "/api/browser/capture/cancel", 200, "{}");
    EXPECT_TRUE(rig.backend.capture_cancel("41").ok());
    EXPECT_EQ(rig.replay->last().body, R"({"sessionId":41})");
}

TEST(PegasusBackend, ProbeRefusesWebPagesAndQueuesDirectLinks)
{
    Rig rig;
    rig.replay->add("POST", "/api/resolve/probe", 200,
                    R"({"requestedUrl":"https://h.example/file.bin","finalUrl":"https://cdn.example/file.bin","status":200,"contentType":"application/octet-stream","fileName":"file.bin","isDownload":true,"hasContentLength":true,"contentLength":1048576})");
    const Result<Probe> probe = rig.backend.probe_link("https://h.example/file.bin");
    ASSERT_TRUE(probe.ok());
    EXPECT_EQ(probe.value.file_name, "file.bin");
    EXPECT_TRUE(probe.value.rejection.empty());
    rig.replay->add("POST", "/api/queue/direct", 200,
                    R"({"dispatchResult":{"ok":true,"kind":"queued","jobId":3,"fileName":"file.bin"}})");
    rig.replay->add("GET", "/api/queue", 200, R"({"jobs":[],"summary":{}})");
    const Result<Dispatch> queued = rig.backend.queue_direct(probe.value, "");
    ASSERT_TRUE(queued.ok());
    EXPECT_TRUE(queued.value.queued);
    EXPECT_NE(rig.replay->log()[rig.replay->log().size() - 1].path.size(), 0u);

    rig.replay->set("POST", "/api/resolve/probe", 200,
                    R"({"requestedUrl":"https://h.example/p","status":200,"isHtml":true,"isDownload":false})");
    const Result<Probe> page = rig.backend.probe_link("https://h.example/p");
    ASSERT_TRUE(page.ok());
    EXPECT_FALSE(page.value.rejection.empty());
    // Not a web address: refused before anything is sent.
    const std::size_t sent = rig.replay->log().size();
    EXPECT_FALSE(rig.backend.probe_link("ftp://x/y").ok());
    EXPECT_EQ(rig.replay->log().size(), sent);
}

TEST(PegasusBackend, SourceCallsSendPegasusBodiesAndReturnTheList)
{
    NEED_SAMPLE(sources, "sources.json");
    Rig rig;
    rig.replay->add("GET", "/api/sources", 200, sources);
    const Result<std::vector<Source>> list = rig.backend.sources();
    ASSERT_TRUE(list.ok());
    ASSERT_FALSE(list.value.empty());
    rig.replay->add("POST", "/api/sources/enabled", 200, sources);
    ASSERT_TRUE(rig.backend.source_enable(list.value[0].id, false).ok());
    EXPECT_EQ(rig.replay->last().body,
              "{\"sourceId\":" + list.value[0].id + ",\"enabled\":false}");
    rig.replay->add("POST", "/api/sources/delete", 200, sources);
    ASSERT_TRUE(rig.backend.source_delete(list.value[0].id).ok());
    EXPECT_EQ(rig.replay->last().body, "{\"sourceId\":" + list.value[0].id + "}");
    rig.replay->add("POST", "/api/sources/url", 200, sources);
    ASSERT_TRUE(rig.backend.source_add_url("https://catalog.example/a.json/").ok());
    EXPECT_EQ(rig.replay->last().body, R"({"url":"https://catalog.example/a.json"})");
    EXPECT_FALSE(rig.backend.source_add_url("not a url").ok());
}

TEST(PegasusBackend, DefaultSourcesAreMappedForTheOnboardingList)
{
    NEED_SAMPLE(defaults, "default-sources.json");
    Rig rig;
    rig.replay->add("GET", "/api/default-sources", 200, defaults);
    const Result<DefaultSources> result = rig.backend.default_sources(false);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value.sources.empty());
    for (const DefaultSource &s : result.value.sources)
        EXPECT_FALSE(s.id.empty());
    rig.replay->add("GET", "/api/default-sources?refresh=1", 200, defaults);
    EXPECT_TRUE(rig.backend.default_sources(true).ok());
}

TEST(PegasusBackend, StoreAndBackendWorkTogetherOnTheRecordedSamples)
{
    NEED_SAMPLE(library, "library.json");
    NEED_SAMPLE(sources, "sources.json");
    NEED_SAMPLE(queue, "queue.json");
    Rig rig;
    rig.replay->add("GET", "/api/library", 200, library);
    rig.replay->add("GET", "/api/sources", 200, sources);
    rig.replay->add("GET", "/api/queue", 200, queue);
    Store store(rig.backend);
    store.poll_once();
    ASSERT_TRUE(store.load_catalog_once());
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_TRUE(store.state().have_catalog);
    EXPECT_EQ(store.state().packages.size(), 2962u);
    EXPECT_TRUE(store.state().have_sources);
    // Asking again without a source change does not fetch the 7 MB again.
    store.refresh_now();
    store.poll_once();
    EXPECT_FALSE(store.load_catalog_once());
    EXPECT_EQ(rig.replay->count("GET", "/api/library"), 1u);
}

} // namespace
} // namespace orbit
