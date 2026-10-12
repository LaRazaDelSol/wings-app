// Wings core - Read-only live smoke tool.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   wings_smoke [host] [port] [--library]
//
// Connects to a Pegasus DL console, prints version/compat/capabilities and a
// summary of every GET route the adapter wraps. It only ever issues GET
// requests (connect() probes are GET-only too). It never prints tokens.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "wings/http.hpp"
#include "wings/logic.hpp"
#include "wings/pegasus.hpp"

using namespace wings;

namespace
{
int g_failures = 0;

template <class T> bool show(const char *what, const Result<T> &r)
{
    if (r.ok())
        return true;
    std::printf("  %-28s ERROR kind=%d status=%d %s\n", what, static_cast<int>(r.error.kind), r.error.status,
                r.error.message.c_str());
    ++g_failures;
    return false;
}
} // namespace

int main(int argc, char **argv)
{
    std::string host = "10.0.0.113";
    int port = 6970;
    bool library = false;
    int positional = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--library") == 0)
            library = true;
        else if (positional == 0)
            host = argv[i], ++positional;
        else if (positional == 1)
            port = std::atoi(argv[i]), ++positional;
    }

    auto transport = std::make_shared<http::SocketTransport>(host, static_cast<std::uint16_t>(port), 20000);
    Connection c = connect(transport);
    std::printf("connect: %s\n", c.message.c_str());
    if (!c.usable())
        return 2;
    std::printf("version=%s adapter=%s compat=%d\n", c.health.version.c_str(), c.adapter.c_str(), static_cast<int>(c.compat));
    const Capabilities &k = c.caps;
    std::printf("capabilities: settings=%d sources=%d library=%d defaultSources=%d queue=%d debrid=%d debridQueue=%d\n"
                "              providerResolve=%d browserCapture=%d mounts=%d browse=%d logs=%d appdb=%d\n"
                "              aprEmu=%d aprEmuVersions=%d installedGameDelete=%d fileManager=%d fileOperations=%d dnsHelper=%d\n",
                k.settings, k.sources, k.library, k.defaultSources, k.queue, k.debrid, k.debridQueue, k.providerResolve,
                k.browserCapture, k.mounts, k.browse, k.logs, k.appdb, k.aprEmu, k.aprEmuVersions, k.installedGameDelete,
                k.fileManager, k.fileOperations, k.dnsHelper);
    for (const std::string &n : k.notes)
        std::printf("  note: %s\n", n.c_str());

    IPegasus &api = *c.api;
    if (k.settings)
    {
        auto s = api.getSettings();
        if (show("settings", s))
            std::printf("settings: root=%s connections=%d realDebrid(configured=%d valid=%d domains=%zu) torBox(configured=%d valid=%d domains=%zu)\n",
                        s.value.downloadRoot.c_str(), s.value.downloadConnections, s.value.realDebrid.configured,
                        s.value.realDebrid.valid, s.value.realDebrid.domains.size(), s.value.torBox.configured,
                        s.value.torBox.valid, s.value.torBox.domains.size());
    }
    if (k.sources)
    {
        auto s = api.listSources();
        if (show("sources", s))
            std::printf("sources: %zu\n", s.value.size());
    }
    if (k.defaultSources)
    {
        auto s = api.getDefaultSources(false);
        if (show("default-sources", s))
            std::printf("default sources: %zu (revision %lld)\n", s.value.sources.size(), static_cast<long long>(s.value.revision));
    }
    if (library)
    {
        auto l = api.getLibrary();
        if (show("library", l))
        {
            size_t links = 0, native = 0;
            for (const Package &p : l.value)
                for (const DownloadLink &d : logic::packageLinks(p))
                {
                    ++links;
                    native += logic::urlHasPkgExtension(logic::downloadLinkUrl(d.url)) ? 1 : 0;
                }
            std::printf("library: %zu packages, %zu links (%zu native .pkg)\n", l.value.size(), links, native);
        }
    }
    if (k.queue)
    {
        auto q = api.getQueue();
        if (show("queue", q))
            std::printf("queue: %zu jobs (total=%lld)\n", q.value.jobs.size(), static_cast<long long>(q.value.summary.total));
    }
    if (k.mounts)
    {
        auto m = api.getMounts();
        if (show("mounts", m))
            std::printf("mounts: %zu\n", m.value.size());
    }
    if (k.logs)
    {
        auto l = api.getLogs(0);
        if (show("logs", l))
            std::printf("logs: %zu buffered (capacity %lld)\n", l.value.logs.size(), static_cast<long long>(l.value.capacity));
    }
    if (k.appdb)
    {
        auto a = api.getInstalledGames();
        if (show("appdb", a))
            std::printf("installed games: %zu\n", a.value.titles.size());
    }
    if (k.aprEmuVersions)
    {
        auto v = api.getAprVersions();
        if (show("apr versions", v))
            std::printf("apr versions: %zu\n", v.value.versions.size());
    }
    if (k.browserCapture)
    {
        auto a = api.captureActive();
        if (show("capture active", a))
            std::printf("capture: active=%d status=%s\n", a.value.active, a.value.statusRaw.c_str());
    }
    if (k.fileManager)
    {
        auto o = api.fileOperations();
        if (show("file operations", o))
            std::printf("file operations: %zu\n", o.value.size());
        auto l = api.fileList("/data");
        if (show("file list /data", l))
            std::printf("files in /data: %zu (writable=%d)\n", l.value.entries.size(), l.value.writable);
    }
    std::printf(g_failures ? "SMOKE: %d route(s) failed\n" : "SMOKE OK (%d failures)\n", g_failures);
    return g_failures ? 1 : 0;
}
