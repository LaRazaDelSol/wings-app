// Wings core - Version selection and capability probe.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// connect() = GET /api/health -> pick adapter by version -> probe cheap GET
// routes. Anything that fails turns its feature off and adds a note; nothing
// here throws or aborts. The 7 MB /api/library route is deliberately not
// probed: `library` is inferred from /api/sources, which is served by the
// same catalogue subsystem.

#include <cstdio>
#include <cstdlib>

#include "wings/pegasus.hpp"
#include "wings/pegasus_v1.hpp"

namespace wings
{

bool parseVersion(const std::string &text, int *major, int *minor, int *patch)
{
    int parts[3] = {0, 0, 0};
    std::size_t pos = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
            return false;
        long value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
        {
            value = value * 10 + (text[pos] - '0');
            if (value > 100000)
                return false;
            ++pos;
        }
        parts[i] = static_cast<int>(value);
        if (i < 2)
        {
            if (pos >= text.size() || text[pos] != '.')
                return false;
            ++pos;
        }
    }
    // Allow a pre-release / build suffix ("1.11.0-beta"), but not garbage digits.
    if (pos < text.size() && text[pos] != '-' && text[pos] != '+')
        return false;
    *major = parts[0];
    *minor = parts[1];
    *patch = parts[2];
    return true;
}

namespace
{

// True when the route exists (anything but transport failure / "Route not found").
bool route_present(const ApiError &e)
{
    return e.ok() || (e.kind != ApiError::Kind::transport && !e.route_missing());
}

void note(Capabilities &caps, const std::string &feature, const ApiError &e)
{
    if (e.kind == ApiError::Kind::transport)
        caps.notes.push_back(feature + ": unreachable (" + e.message + ")");
    else if (e.route_missing())
        caps.notes.push_back(feature + ": route not available in this Pegasus version");
    else
        caps.notes.push_back(feature + ": " + e.message);
}

} // namespace

Capabilities probeCapabilities(IPegasus &api, const Health &health, const ConnectOptions &options)
{
    Capabilities caps;
    caps.health = !health.version.empty() || health.status == "running";

    {
        const auto r = api.getSettings();
        caps.settings = r.ok();
        if (r.ok())
        {
            caps.dnsHelper = r.value.dnsHelper.present;
            caps.debrid = r.value.realDebrid.present || r.value.torBox.present;
            if (!caps.debrid)
                caps.notes.push_back("debrid: settings carry no realDebrid/torBox block");
            if (!caps.dnsHelper)
                caps.notes.push_back("dnsHelper: settings carry no dnsHelper block");
        }
        else
            note(caps, "settings", r.error);
    }
    {
        const auto r = api.listSources();
        caps.sources = r.ok();
        caps.library = r.ok(); // inferred; /api/library is too large to probe
        if (!r.ok())
            note(caps, "sources", r.error);
    }
    {
        const auto r = api.getDefaultSources(false);
        caps.defaultSources = r.ok() && r.value.available;
        if (!r.ok())
            note(caps, "defaultSources", r.error);
        else if (!r.value.available)
            caps.notes.push_back("defaultSources: catalog list unavailable" +
                                 (r.value.error.empty() ? std::string() : " (" + r.value.error + ")"));
    }
    {
        const auto r = api.getQueue();
        caps.queue = r.ok();
        if (!r.ok())
            note(caps, "queue", r.error);
    }
    caps.debridQueue = caps.debrid && caps.queue;
    {
        const auto r = api.getMounts();
        caps.mounts = r.ok();
        if (!r.ok())
            note(caps, "mounts", r.error);
    }
    {
        const auto r = api.browse("/");
        caps.browse = r.ok();
        if (!r.ok())
            note(caps, "browse", r.error);
    }
    {
        // since=<huge> returns no entries: a cheap existence check.
        const auto r = api.getLogs(2000000000LL);
        caps.logs = r.ok();
        if (!r.ok())
            note(caps, "logs", r.error);
    }
    {
        const auto r = api.captureActive();
        caps.browserCapture = r.ok();
        caps.providerResolve = r.ok(); // same subsystem; inferred
        if (!r.ok())
            note(caps, "browserCapture", r.error);
    }
    if (options.probeAppDb)
    {
        const auto r = api.getInstalledGames();
        caps.appdb = r.ok();
        if (!r.ok())
            note(caps, "appdb", r.error);
    }
    {
        // 503 {"error":"APR Emu manifest has too many versions"} on some consoles: the route
        // exists (so update/restore may still work) but the version list is unusable.
        const auto r = api.getAprVersions();
        caps.aprEmuVersions = r.ok();
        if (r.ok())
        {
            caps.aprEmu = true;
        }
        else if (route_present(r.error))
        {
            caps.aprEmu = true;
            caps.notes.push_back("aprEmuVersions: degraded (" + r.error.message +
                                 "); update works without a version choice, reinstall-by-version is disabled");
        }
        else
        {
            caps.aprEmu = false;
            note(caps, "aprEmu", r.error);
        }
        // Status routes are GET-only and answer a bogus title id with a normal error.
        const JobRef probe{"", "WINGS-PROBE"};
        const auto upd = api.aprUpdateStatus(probe);
        const auto rst = api.aprRestoreStatus(probe);
        if (upd.error.route_missing() && rst.error.route_missing())
        {
            caps.aprEmu = false;
            caps.aprEmuVersions = false;
            caps.notes.push_back("aprEmu: update/restore routes not available in this Pegasus version");
        }
        const auto del = api.installedGameDeleteStatus(probe);
        caps.installedGameDelete = route_present(del.error);
        if (!caps.installedGameDelete)
            note(caps, "installedGameDelete", del.error);
    }
    {
        const auto r = api.fileOperations();
        caps.fileOperations = r.ok();
        caps.fileManager = r.ok(); // list/locations/etc. share the subsystem; operations is the cheap probe
        if (!r.ok())
            note(caps, "fileManager", r.error);
    }
    return caps;
}

Connection connect(std::shared_ptr<http::Transport> transport, const ConnectOptions &options)
{
    Connection c;
    if (!transport)
    {
        c.compat = Compat::unreachable;
        c.error.kind = ApiError::Kind::client;
        c.error.message = "no transport";
        c.message = "No transport configured.";
        return c;
    }
    // /api/health is the stable entry point: it is read with the v1 reader, which only
    // looks at status/port/downloadOnly/nativePkgHandoff/version.
    PegasusV1 healthReader(transport);
    const Result<Health> h = healthReader.health();
    if (!h.ok())
    {
        c.compat = Compat::unreachable;
        c.error = h.error;
        c.message = "Pegasus is not reachable: " + h.error.message;
        c.caps.notes.push_back("health: " + h.error.message);
        return c;
    }
    c.health = h.value;
    c.caps.health = true;

    int major = 0, minor = 0, patch = 0;
    if (!parseVersion(h.value.version, &major, &minor, &patch))
    {
        c.compat = Compat::unsupported;
        c.message = "Unrecognised Pegasus version \"" + h.value.version + "\"; no adapter selected.";
        c.caps.notes.push_back("version: " + c.message);
        return c;
    }
    if (major != 1)
    {
        c.compat = Compat::unsupported;
        c.message = "Pegasus " + h.value.version + " is not supported (adapters exist for 1.x).";
        c.caps.notes.push_back("version: " + c.message);
        return c;
    }
    c.compat = minor == 11 ? Compat::tested : Compat::untestedMinor;
    c.adapter = "pegasus-v1";
    c.api = std::make_unique<PegasusV1>(transport);
    c.caps = probeCapabilities(*c.api, c.health, options);
    c.message = "Pegasus " + h.value.version + " via " + c.adapter +
                (c.compat == Compat::tested ? " (tested)" : " (untested minor version; features probed)");
    if (h.value.status != "running")
        c.caps.notes.push_back("health: status is \"" + h.value.status + "\"");
    return c;
}

} // namespace wings
