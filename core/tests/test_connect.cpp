// Wings core tests - version selection and capability probing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_harness.hpp"
#include "test_support.hpp"
#include "wings/pegasus.hpp"

using namespace wings;

namespace
{
// A console shaped like the user's: every recorded sample, APR versions 503.
std::shared_ptr<http::ReplayTransport> user_console(const std::string &version = "1.11.0")
{
    auto t = std::make_shared<http::ReplayTransport>();
    t->add("GET", "/api/health", 200, "{\"status\":\"running\",\"port\":6970,\"downloadOnly\":false,\"nativePkgHandoff\":true,\"version\":\"" + version + "\"}");
    t->add_file("GET", "/api/settings", fixture("settings.json"));
    t->add_file("GET", "/api/sources", sample("sources.json"));
    t->add_file("GET", "/api/default-sources", sample("default-sources.json"));
    t->add_file("GET", "/api/queue", sample("queue.json"));
    t->add_file("GET", "/api/mounts", sample("mounts.json"));
    t->add_file("GET", "/api/browse?path=%2F", sample("browse_path__data.json"));
    t->add_file("GET", "/api/logs?since=2000000000", sample("logs_since_0.json"));
    t->add_file("GET", "/api/browser/capture/active", sample("browser_capture_active.json"));
    t->add_file("GET", "/appdb", sample("appdb.json"));
    t->add_file("GET", "/api/apr-emu/versions", sample("apr-emu_versions.json"), 503);
    t->add("GET", "/api/apr-emu/update/status?titleId=WINGS-PROBE", 400, R"({"error":"Update job not found"})");
    t->add("GET", "/api/apr-emu/restore/status?titleId=WINGS-PROBE", 400, R"({"error":"Restore job not found"})");
    t->add("GET", "/api/installed-games/delete/status?titleId=WINGS-PROBE", 404, R"({"error":"Delete job not found"})");
    t->add_file("GET", "/api/files/operations", sample("files_operations.json"));
    return t;
}

bool has_note(const Capabilities &c, const std::string &needle)
{
    for (const std::string &n : c.notes)
        if (n.find(needle) != std::string::npos)
            return true;
    return false;
}
} // namespace

TEST(connect_tested_version_full_probe)
{
    auto t = user_console();
    Connection c = connect(t);
    CHECK(c.usable());
    CHECK(c.compat == Compat::tested);
    CHECK_EQ_STR(c.adapter, "pegasus-v1");
    CHECK_EQ_STR(c.health.version, "1.11.0");
    const Capabilities &k = c.caps;
    CHECK(k.health && k.settings && k.sources && k.library && k.defaultSources && k.queue);
    CHECK(k.debrid && k.debridQueue && k.dnsHelper);
    CHECK(k.mounts && k.browse && k.logs && k.browserCapture && k.providerResolve);
    CHECK(k.appdb && k.fileManager && k.fileOperations);
    CHECK(k.installedGameDelete);
    // APR manifest 503: update flows stay on, the version list is off, with a note.
    CHECK(k.aprEmu);
    CHECK(!k.aprEmuVersions);
    CHECK(has_note(k, "aprEmuVersions: degraded (APR Emu manifest has too many versions)"));
    CHECK(c.message.find("tested") != std::string::npos);
    // The probe never touched the 7 MB library or any write route.
    for (const auto &q : t->log())
    {
        CHECK_MSG(q.method == "GET", q.method + " " + q.path);
        CHECK_MSG(q.path != "/api/library", q.path);
    }
}

TEST(connect_untested_minor_uses_adapter)
{
    auto t = user_console("1.12.3");
    Connection c = connect(t);
    CHECK(c.usable());
    CHECK(c.compat == Compat::untestedMinor);
    CHECK(c.caps.queue && c.caps.sources);
}

TEST(connect_unsupported_major_has_no_adapter_and_no_features)
{
    auto t = user_console("2.0.0");
    Connection c = connect(t);
    CHECK(!c.usable());
    CHECK(c.compat == Compat::unsupported);
    CHECK(c.health.version == "2.0.0");
    CHECK(!c.caps.queue && !c.caps.sources && !c.caps.fileManager && !c.caps.appdb);
    CHECK(!c.message.empty());
    CHECK_EQ_INT(t->log().size(), 1); // only /api/health was requested

    auto t2 = user_console("weird");
    c = connect(t2);
    CHECK(!c.usable() && c.compat == Compat::unsupported);
}

TEST(connect_unreachable)
{
    auto t = std::make_shared<http::ReplayTransport>();
    t->add_failure("GET", "/api/health", "connection refused");
    Connection c = connect(t);
    CHECK(!c.usable() && c.compat == Compat::unreachable);
    CHECK(c.error.kind == ApiError::Kind::transport);
    CHECK(c.message.find("connection refused") != std::string::npos);
    CHECK(!c.caps.health);

    // Health answers but is an error body.
    auto t2 = std::make_shared<http::ReplayTransport>();
    t2->add("GET", "/api/health", 503, R"({"error":"starting"})");
    c = connect(t2);
    CHECK(!c.usable() && c.error.status == 503 && c.error.message == "starting");

    CHECK(!connect(nullptr).usable());
}

TEST(connect_missing_routes_disable_features_without_crashing)
{
    // An older/stripped Pegasus: health + queue only, everything else 404 "Route not found".
    auto t = std::make_shared<http::ReplayTransport>();
    t->add("GET", "/api/health", 200, R"({"status":"running","version":"1.9.0"})");
    t->add_file("GET", "/api/queue", sample("queue.json"));
    Connection c = connect(t);
    CHECK(c.usable() && c.compat == Compat::untestedMinor);
    CHECK(c.caps.health && c.caps.queue);
    CHECK(!c.caps.settings && !c.caps.sources && !c.caps.library && !c.caps.defaultSources);
    CHECK(!c.caps.debrid && !c.caps.debridQueue && !c.caps.mounts && !c.caps.browse && !c.caps.logs);
    CHECK(!c.caps.browserCapture && !c.caps.providerResolve && !c.caps.appdb && !c.caps.aprEmu);
    CHECK(!c.caps.aprEmuVersions && !c.caps.installedGameDelete && !c.caps.fileManager);
    CHECK(has_note(c.caps, "settings: route not available"));
    CHECK(has_note(c.caps, "aprEmu: update/restore routes not available"));
    // Calling a disabled feature is a clean error, not a crash.
    auto lib = c.api->getLibrary();
    CHECK(!lib.ok() && lib.error.route_missing());
}

TEST(connect_transport_drop_mid_probe)
{
    auto t = std::make_shared<http::ReplayTransport>();
    t->add("GET", "/api/health", 200, R"({"status":"running","version":"1.11.0"})");
    t->add_failure("GET", "/api/mounts", "timed out");
    Connection c = connect(t);
    CHECK(c.usable());
    CHECK(!c.caps.mounts);
    CHECK(has_note(c.caps, "mounts: unreachable (timed out)"));
}

TEST(connect_apr_versions_ok_enables_reinstall)
{
    auto t = std::make_shared<http::ReplayTransport>();
    t->add("GET", "/api/health", 200, R"({"status":"running","version":"1.11.0"})");
    t->add("GET", "/api/apr-emu/versions", 200, R"({"latestReleaseId":"r1","versions":[{"id":"r1","version":"1.0.0","build":"release"}]})");
    t->add("GET", "/api/apr-emu/update/status?titleId=WINGS-PROBE", 400, R"({"error":"nope"})");
    t->add("GET", "/api/apr-emu/restore/status?titleId=WINGS-PROBE", 400, R"({"error":"nope"})");
    Connection c = connect(t);
    CHECK(c.caps.aprEmu && c.caps.aprEmuVersions);
}

TEST(parse_version)
{
    int a, b, c;
    CHECK(parseVersion("1.11.0", &a, &b, &c) && a == 1 && b == 11 && c == 0);
    CHECK(parseVersion("2.0.1-beta", &a, &b, &c) && a == 2 && b == 0 && c == 1);
    CHECK(!parseVersion("1.11", &a, &b, &c));
    CHECK(!parseVersion("", &a, &b, &c));
    CHECK(!parseVersion("v1.11.0", &a, &b, &c));
    CHECK(!parseVersion("1.11.0x", &a, &b, &c));
}
