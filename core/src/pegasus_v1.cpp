// Wings core - Pegasus DL 1.x adapter. The only file that knows Pegasus' routes and JSON.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Request bodies mirror the Pegasus web UI (recon/pegasus-app.js and
// recon/pegasus-file-manager.js) byte for byte: JSON.stringify key order,
// compact. Every method cites the JS line it follows. Parsing is tolerant:
// unknown fields are ignored, missing ones keep the model default, and errors
// surface as ApiError{kind, status, message} using the {"error": ".."} body.

#include "wings/pegasus_v1.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "wings/json.hpp"
#include "wings/json_writer.hpp"
#include "wings/logic.hpp"

namespace wings
{

using json::ObjectBuilder;
using json::Value;

namespace
{

// ---------------------------------------------------------------- plumbing --

struct Reply
{
    ApiError error;
    Value json;          // parsed body ({} object when the body was empty)
    std::string rawBody; // set for raw fetches
};

ApiError make_error(ApiError::Kind kind, int status, std::string message)
{
    ApiError e;
    e.kind = kind;
    e.status = status;
    e.message = std::move(message);
    return e;
}

ApiError client_error(const std::string &message)
{
    return make_error(ApiError::Kind::client, 0, message);
}

// app.js:597 api(): non-2xx => Error(json.error || statusText); body parsed as JSON.
Reply call(http::Transport &transport, const std::string &method, const std::string &path,
           const std::string &body = std::string(), const std::string &contentType = std::string(),
           std::size_t maxBytes = 0, bool raw = false)
{
    Reply reply;
    http::Request request;
    request.method = method;
    request.path = path;
    request.body = body;
    request.content_type = contentType;
    request.max_bytes = maxBytes;
    request.timeout_ms = http::timeout_for(method, path);
    const http::Response response = transport.send(request);
    if (!response.transported())
    {
        reply.error = make_error(ApiError::Kind::transport, 0, response.error.empty() ? "no response" : response.error);
        return reply;
    }
    const bool success = response.status >= 200 && response.status < 300;
    if (success && raw)
    {
        reply.rawBody = response.body;
        return reply;
    }
    Value parsed;
    std::string parseError;
    bool haveJson = false;
    if (response.body.empty())
    {
        haveJson = true; // `text ? JSON.parse(text) : {}`
    }
    else
    {
        haveJson = json::parse(response.body, &parsed, &parseError);
    }
    if (!success)
    {
        std::string message;
        if (haveJson && parsed.is_object() && parsed["error"].is_string())
            message = parsed["error"].as_string();
        if (message.empty())
            message = "HTTP " + std::to_string(response.status);
        reply.error = make_error(ApiError::Kind::http, response.status, message);
        reply.json = std::move(parsed);
        return reply;
    }
    if (!haveJson)
    {
        reply.error = make_error(ApiError::Kind::parse, response.status, "Invalid JSON response: " + parseError);
        return reply;
    }
    reply.json = std::move(parsed);
    return reply;
}

// ----------------------------------------------------------------- readers --

std::int64_t num(const Value &v, std::string_view key, std::int64_t fallback = 0)
{
    const Value &x = v[key];
    if (x.is_number())
        return x.as_int(fallback);
    if (x.is_string() && !x.as_string().empty())
    {
        char *end = nullptr;
        const long long n = std::strtoll(x.as_string().c_str(), &end, 10);
        if (end && *end == '\0')
            return n;
    }
    return fallback;
}

double dbl(const Value &v, std::string_view key, double fallback = 0.0)
{
    return v[key].is_number() ? v[key].as_number(fallback) : fallback;
}

bool flag(const Value &v, std::string_view key, bool fallback = false)
{
    return v[key].is_bool() ? v[key].as_bool(fallback) : fallback;
}

const std::string &text(const Value &v, std::string_view key)
{
    return v[key].as_string();
}

// A job/session id that may be a JSON number or a string.
std::string id_text(const Value &v, std::string_view key)
{
    const Value &x = v[key];
    if (x.is_string())
        return x.as_string();
    if (x.is_number())
    {
        const double d = x.as_number();
        if (std::floor(d) == d && std::fabs(d) < 9e15)
            return std::to_string(static_cast<long long>(d));
    }
    return {};
}

bool truthy(const Value &x)
{
    switch (x.type())
    {
    case json::Type::null: return false;
    case json::Type::boolean: return x.as_bool();
    case json::Type::number: return x.as_number() != 0.0;
    case json::Type::string: return !x.as_string().empty();
    default: return true;
    }
}

// ----------------------------------------------------------------- parsers --

Health parse_health(const Value &v)
{
    Health h;
    h.status = text(v, "status");
    h.port = static_cast<int>(num(v, "port"));
    h.downloadOnly = flag(v, "downloadOnly");
    h.nativePkgHandoff = flag(v, "nativePkgHandoff");
    h.version = text(v, "version");
    return h;
}

DebridStatus parse_debrid(const Value &v)
{
    DebridStatus d;
    if (!v.is_object())
        return d;
    d.present = true;
    d.configured = flag(v, "configured");
    d.valid = flag(v, "valid");
    d.premium = flag(v, "premium");
    d.accountType = text(v, "accountType");
    d.hosterCount = num(v, "hosterCount");
    d.domainCount = num(v, "domainCount");
    d.lastCheckedAt = num(v, "lastCheckedAt");
    d.error = text(v, "error");
    for (const Value &domain : v["domains"].items())
        if (domain.is_string())
            d.domains.push_back(domain.as_string());
    return d;
}

Settings parse_settings(const Value &v)
{
    Settings s;
    s.downloadRoot = text(v, "downloadRoot");
    // app.js:4722 Number(downloadConnections || concurrency || 16)
    const std::int64_t conn = num(v, "downloadConnections");
    const std::int64_t conc = num(v, "concurrency");
    s.concurrency = static_cast<int>(conc);
    s.downloadConnections = static_cast<int>(conn ? conn : (conc ? conc : logic::kDefaultDownloadConnections));
    s.downloadOnly = flag(v, "downloadOnly");
    s.nativePkgHandoff = flag(v, "nativePkgHandoff");
    s.queuePersists = flag(v, "queuePersists");
    s.rangeResume = flag(v, "rangeResume");
    const Value &dns = v["dnsHelper"];
    if (dns.is_object())
    {
        s.dnsHelper.present = true;
        s.dnsHelper.available = flag(dns, "available");
        s.dnsHelper.nanodnsRunning = flag(dns, "nanodnsRunning");
        s.dnsHelper.managed = flag(dns, "managed");
        s.dnsHelper.autostartAttempted = flag(dns, "autostartAttempted");
        s.dnsHelper.startError = text(dns, "startError");
        s.dnsHelper.recommendedPrimaryDns = text(dns, "recommendedPrimaryDns");
    }
    s.realDebrid = parse_debrid(v["realDebrid"]);
    s.torBox = parse_debrid(v["torBox"]);
    return s;
}

Source parse_source(const Value &v)
{
    Source s;
    s.id = num(v, "id");
    s.name = text(v, "name");
    s.kind = text(v, "kind");
    s.url = text(v, "url");
    s.enabled = flag(v, "enabled", true);
    s.managed = flag(v, "managed");
    s.deletable = flag(v, "deletable", true);
    s.isNew = flag(v, "isNew");
    s.hasCatalog = flag(v, "hasCatalog");
    s.managedId = text(v, "managedId");
    s.description = text(v, "description");
    s.lastRefreshAt = num(v, "lastRefreshAt");
    s.lastRefreshAttemptAt = num(v, "lastRefreshAttemptAt");
    s.packageCount = num(v, "packageCount");
    s.skippedCount = num(v, "skippedCount");
    s.status = text(v, "status");
    return s;
}

std::vector<Source> parse_sources(const Value &v)
{
    std::vector<Source> out;
    for (const Value &item : v["sources"].items())
        out.push_back(parse_source(item));
    return out;
}

DefaultSource parse_default_source(const Value &v)
{
    DefaultSource s;
    s.id = text(v, "id");
    s.name = text(v, "name");
    s.description = text(v, "description");
    s.url = text(v, "url");
    s.defaultEnabled = flag(v, "defaultEnabled");
    s.configured = flag(v, "configured");
    s.sourceId = num(v, "sourceId");
    s.enabled = flag(v, "enabled");
    s.isNew = flag(v, "isNew");
    s.hasCatalog = flag(v, "hasCatalog");
    s.packageCount = num(v, "packageCount");
    return s;
}

Package parse_package(const Value &v)
{
    Package p;
    p.id = num(v, "id");
    p.sourceId = num(v, "sourceId");
    p.sourceName = text(v, "sourceName");
    p.titleId = text(v, "titleId");
    p.title = text(v, "title");
    p.version = text(v, "version");
    p.posterUrl = text(v, "posterUrl");
    p.downloadSource = text(v, "downloadSource");
    p.description = text(v, "description");
    p.downloadLinkCount = num(v, "downloadLinkCount");
    p.sizeBytes = num(v, "sizeBytes");
    for (const Value &l : v["downloadLinks"].items())
    {
        DownloadLink link;
        const Value &idx = l["index"];
        // Number.isInteger(link.index)
        if (idx.is_number() && std::isfinite(idx.as_number()) && std::floor(idx.as_number()) == idx.as_number())
        {
            link.hasIndex = true;
            link.index = idx.as_int();
        }
        link.name = text(l, "name");
        link.url = text(l, "url");
        p.downloadLinks.push_back(std::move(link));
    }
    return p;
}

QueueJob parse_job(const Value &v)
{
    QueueJob j;
    j.id = num(v, "id");
    j.packageId = num(v, "packageId");
    j.downloadLinkIndex = num(v, "downloadLinkIndex");
    j.downloadLinkName = text(v, "downloadLinkName");
    j.title = text(v, "title");
    j.titleId = text(v, "titleId");
    j.version = text(v, "version");
    j.sourceName = text(v, "sourceName");
    j.fileName = text(v, "fileName");
    j.statusRaw = text(v, "status");
    j.status = logic::jobStatusFromString(j.statusRaw);
    j.detail = text(v, "detail");
    j.error = text(v, "error");
    j.destinationPath = text(v, "destinationPath");
    j.contentKind = text(v, "contentKind");
    j.archiveFormat = text(v, "archiveFormat");
    j.archiveMode = text(v, "archiveMode");
    if (v["streamMode"].is_bool())
    {
        j.hasStreamMode = true;
        j.streamMode = v["streamMode"].as_bool();
    }
    j.extractedBytesDone = num(v, "extractedBytesDone");
    j.extractedTotalBytes = num(v, "extractedTotalBytes");
    j.currentArchiveEntryBytesDone = num(v, "currentArchiveEntryBytesDone");
    j.currentArchiveEntry = text(v, "currentArchiveEntry");
    j.currentArchiveEntrySize = num(v, "currentArchiveEntrySize");
    j.archivePasswordRequired = flag(v, "archivePasswordRequired");
    j.sizeBytes = num(v, "sizeBytes");
    j.bytesDone = num(v, "bytesDone");
    j.speedBps = num(v, "speedBps");
    j.etaSeconds = num(v, "etaSeconds");
    j.progress = dbl(v, "progress");
    j.phaseRaw = text(v, "phase");
    j.phase = logic::jobPhaseFromString(j.phaseRaw);
    j.phaseProgress = dbl(v, "phaseProgress");
    j.phaseBytesDone = num(v, "phaseBytesDone");
    j.phaseTotalBytes = num(v, "phaseTotalBytes");
    j.phaseSpeedBps = num(v, "phaseSpeedBps");
    j.attempts = num(v, "attempts");
    j.maxAttempts = num(v, "maxAttempts");
    j.supportsResume = flag(v, "supportsResume");
    j.resumed = flag(v, "resumed");
    j.createdAt = num(v, "createdAt");
    j.startedAt = num(v, "startedAt");
    j.updatedAt = num(v, "updatedAt");
    j.finishedAt = num(v, "finishedAt");
    for (const Value &c : v["archiveCandidates"].items())
    {
        ArchiveCandidate cand;
        cand.index = num(c, "index");
        cand.name = text(c, "name");
        j.archiveCandidates.push_back(std::move(cand));
    }
    return j;
}

QueueSummary parse_summary(const Value &v)
{
    QueueSummary s;
    s.total = num(v, "total");
    s.queued = num(v, "queued");
    s.starting = num(v, "starting");
    s.downloading = num(v, "downloading");
    s.merging = num(v, "merging");
    s.extracting = num(v, "extracting");
    s.finalizing = num(v, "finalizing");
    s.waitingPassword = num(v, "waitingPassword");
    s.waitingChoice = num(v, "waitingChoice");
    s.pausing = num(v, "pausing");
    s.cancelling = num(v, "cancelling");
    s.paused = num(v, "paused");
    s.completed = num(v, "completed");
    s.failed = num(v, "failed");
    s.cancelled = num(v, "cancelled");
    s.etaSeconds = num(v, "etaSeconds");
    return s;
}

QueueSnapshot parse_queue(const Value &v)
{
    QueueSnapshot q;
    q.summary = parse_summary(v["summary"]);
    for (const Value &j : v["jobs"].items())
        q.jobs.push_back(parse_job(j));
    return q;
}

DispatchResult parse_dispatch_result(const Value &v)
{
    DispatchResult r;
    r.ok = flag(v, "ok");
    r.kindRaw = text(v, "kind");
    r.kind = r.kindRaw == "native-pkg" ? DispatchKind::nativePkg
                                       : (r.kindRaw == "queued" ? DispatchKind::queued : DispatchKind::other);
    r.accepted = flag(v, "accepted");
    r.title = text(v, "title");
    r.message = text(v, "message");
    r.contentId = text(v, "contentId");
    r.jobId = num(v, "jobId");
    r.fileName = text(v, "fileName");
    r.error = text(v, "error");
    return r;
}

// app.js:3429-3445: dispatchResult (if truthy) then dispatchResults[].
QueueResponse parse_queue_response(const Value &v)
{
    QueueResponse r;
    r.hasQueue = v["jobs"].is_array();
    if (r.hasQueue || v["summary"].is_object())
        r.queue = parse_queue(v);
    if (truthy(v["dispatchResult"]))
        r.dispatchResults.push_back(parse_dispatch_result(v["dispatchResult"]));
    for (const Value &item : v["dispatchResults"].items())
        r.dispatchResults.push_back(parse_dispatch_result(item));
    return r;
}

ResolveProbe parse_probe(const Value &v)
{
    ResolveProbe p;
    p.requestedUrl = text(v, "requestedUrl");
    p.finalUrl = text(v, "finalUrl");
    p.status = num(v, "status");
    p.contentType = text(v, "contentType");
    p.contentDisposition = text(v, "contentDisposition");
    p.fileName = text(v, "fileName");
    p.hasContentLength = flag(v, "hasContentLength");
    p.contentLength = num(v, "contentLength");
    p.bodyLength = num(v, "bodyLength");
    p.htmlTitle = text(v, "htmlTitle");
    p.isHtml = flag(v, "isHtml");
    p.isDownload = flag(v, "isDownload");
    p.redirectCount = num(v, "redirectCount");
    return p;
}

DebridQueueResult parse_debrid_queue(const Value &v)
{
    DebridQueueResult r;
    r.needsSelection = flag(v, "needsSelection");
    r.selectionId = text(v, "selectionId");
    for (const Value &o : v["options"].items())
    {
        DebridOption opt;
        opt.index = num(o, "index");
        opt.filename = text(o, "filename");
        opt.filesize = num(o, "filesize");
        opt.host = text(o, "host");
        r.options.push_back(std::move(opt));
    }
    r.response = parse_queue_response(v);
    return r;
}

CaptureStatus parse_capture(const Value &v)
{
    CaptureStatus c;
    c.active = flag(v, "active");
    c.sessionId = num(v, "sessionId");
    c.statusRaw = text(v, "status");
    c.state = logic::captureStateFromString(c.statusRaw);
    c.message = text(v, "message");
    c.packageId = num(v, "packageId");
    c.linkIndex = num(v, "linkIndex");
    c.startUrl = text(v, "startUrl");
    c.authCaptured = flag(v, "authCaptured");
    c.authHost = text(v, "authHost");
    c.pendingCandidates = num(v, "pendingCandidates");
    c.droppedCandidates = num(v, "droppedCandidates");
    c.dispatch = parse_queue_response(v);
    return c;
}

Mount parse_mount(const Value &v)
{
    Mount m;
    m.path = text(v, "path");
    m.name = text(v, "name");
    m.source = text(v, "source");
    m.type = text(v, "type");
    m.writable = flag(v, "writable", true);
    m.totalBytes = num(v, "totalBytes");
    m.freeBytes = num(v, "freeBytes");
    m.usedBytes = num(v, "usedBytes");
    return m;
}

FileEntry parse_entry(const Value &v)
{
    FileEntry e;
    e.name = text(v, "name");
    e.path = text(v, "path");
    e.type = text(v, "type");
    e.directory = flag(v, "directory");
    e.file = flag(v, "file");
    e.symlink = flag(v, "symlink");
    e.mode = num(v, "mode");
    e.modifiedTime = num(v, "modifiedTime");
    e.sizeBytes = num(v, "sizeBytes");
    e.writable = flag(v, "writable", true);
    e.totalBytes = num(v, "totalBytes");
    e.freeBytes = num(v, "freeBytes");
    e.usedBytes = num(v, "usedBytes");
    return e;
}

FileOperation parse_operation(const Value &v)
{
    FileOperation o;
    o.operationId = num(v, "operationId");
    o.type = text(v, "type");
    o.statusRaw = text(v, "status");
    o.status = logic::fileOpStatusFromString(o.statusRaw);
    o.collision = text(v, "collision");
    o.source = text(v, "source");
    o.destination = text(v, "destination");
    o.currentPath = text(v, "currentPath");
    o.error = text(v, "error");
    o.totalItems = num(v, "totalItems");
    o.doneItems = num(v, "doneItems");
    o.totalBytes = num(v, "totalBytes");
    o.doneBytes = num(v, "doneBytes");
    o.startedAt = num(v, "startedAt");
    o.updatedAt = num(v, "updatedAt");
    o.cancelRequested = flag(v, "cancelRequested");
    o.cancelable = flag(v, "cancelable", true);
    return o;
}

std::vector<FileOperation> parse_operations(const Value &v)
{
    std::vector<FileOperation> out;
    for (const Value &o : v["operations"].items())
        out.push_back(parse_operation(o));
    return out;
}

AprEmu parse_apr(const Value &v)
{
    AprEmu a;
    a.present = flag(v, "present");
    a.version = text(v, "version");
    a.build = text(v, "build");
    a.sha256 = text(v, "sha256");
    a.runtimeOverride = flag(v, "runtimeOverride");
    a.latestVersion = text(v, "latestVersion");
    a.latestVersionId = text(v, "latestVersionId");
    a.latestBuild = text(v, "latestBuild");
    a.currentVersionId = text(v, "currentVersionId");
    a.updateAvailable = flag(v, "updateAvailable");
    a.canUpdate = flag(v, "canUpdate");
    a.canReinstall = flag(v, "canReinstall");
    a.remoteAvailable = flag(v, "remoteAvailable", true);
    a.reason = text(v, "reason");
    a.manifestError = text(v, "manifestError");
    a.remountRequired = flag(v, "remountRequired");
    a.remountAttempted = flag(v, "remountAttempted");
    a.remounted = flag(v, "remounted");
    a.remountReason = text(v, "remountReason");
    return a;
}

InstalledGame parse_game(const Value &v)
{
    InstalledGame g;
    g.titleId = text(v, "titleId");
    g.titleName = text(v, "titleName");
    g.title = text(v, "title");
    g.sortPriority = num(v, "sortPriority");
    g.iconUrl = text(v, "iconUrl");
    g.pic0Url = text(v, "pic0Url");
    g.installPath = text(v, "installPath");
    g.installedTime = num(v, "installedTime");
    g.contentId = text(v, "contentId");
    g.contentVersion = text(v, "contentVersion");
    g.version = text(v, "version");
    g.masterVersion = text(v, "masterVersion");
    g.originContentVersion = text(v, "originContentVersion");
    g.sdkVersion = text(v, "sdkVersion");
    g.requiredSystemSoftwareVersion = text(v, "requiredSystemSoftwareVersion");
    g.metadataPath = text(v, "metadataPath");
    g.modifiedTime = num(v, "modifiedTime");
    g.hasIcon = flag(v, "hasIcon");
    g.hasPic0 = flag(v, "hasPic0");
    g.storageKind = text(v, "storageKind");
    g.format = text(v, "format");
    g.backingPath = text(v, "backingPath");
    g.mountLinkPath = text(v, "mountLinkPath");
    g.sizeBytes = num(v, "sizeBytes");
    g.aprEmu = parse_apr(v["aprEmu"]);
    return g;
}

// app.js:4368-4395 normalizeApr{Update,Restore}Job (status default 'starting', progress clamp 0..100).
AprJob parse_apr_job(const Value &v)
{
    AprJob j;
    j.jobId = id_text(v, "jobId");
    j.titleId = text(v, "titleId");
    j.status = text(v, "status").empty() ? "starting" : text(v, "status");
    const Value &p = v["progress"];
    if (p.is_number() && std::isfinite(p.as_number()))
        j.progress = static_cast<int>(std::max(0.0, std::min(100.0, std::floor(p.as_number() + 0.5))));
    j.detail = text(v, "detail");
    j.error = text(v, "error");
    j.operation = text(v, "operation");
    if (v["aprEmu"].is_object())
    {
        j.hasAprEmu = true;
        j.aprEmu = parse_apr(v["aprEmu"]);
    }
    return j;
}

DeleteJob parse_delete_job(const Value &v)
{
    DeleteJob j;
    j.jobId = id_text(v, "jobId");
    j.titleId = text(v, "titleId");
    j.status = text(v, "status");
    j.filesDeleted = num(v, "filesDeleted");
    j.detail = text(v, "detail");
    j.error = text(v, "error");
    return j;
}

// ------------------------------------------------------------- body helpers -

std::string job_query(const JobRef &ref)
{
    // app.js:4454: job.jobId ? jobId=<id> : titleId=<titleId>
    return ref.jobId.empty() ? "titleId=" + logic::encodeURIComponent(ref.titleId)
                             : "jobId=" + logic::encodeURIComponent(ref.jobId);
}

const char *service_id(DebridService s)
{
    return logic::debridServiceInfo(s).id;
}

template <class T> Result<T> fail(const ApiError &e)
{
    Result<T> r;
    r.error = e;
    return r;
}

template <class T> Result<T> fail_client(const std::string &message)
{
    return fail<T>(client_error(message));
}

template <class T, class F> Result<T> map(Reply &&reply, F &&parse)
{
    Result<T> r;
    if (!reply.error.ok())
    {
        r.error = reply.error;
        return r;
    }
    r.value = parse(reply.json);
    return r;
}

Result<Empty> ack(Reply &&reply)
{
    Result<Empty> r;
    r.error = reply.error;
    return r;
}

std::string stringify_job(std::int64_t jobId)
{
    return ObjectBuilder().num("jobId", jobId).done();
}

} // namespace

// ------------------------------------------------------- IPegasus helpers ---

Result<Settings> IPegasus::setDownloadRoot(const std::string &path)
{
    SettingsUpdate u;
    u.hasDownloadRoot = true;
    u.downloadRoot = path;
    return updateSettings(u);
}

Result<Settings> IPegasus::setDownloadConnections(int value)
{
    SettingsUpdate u;
    u.hasDownloadConnections = true;
    u.downloadConnections = value;
    return updateSettings(u);
}

Result<Settings> IPegasus::setDebridToken(DebridService service, const std::string &token)
{
    SettingsUpdate u;
    if (service == DebridService::torBox)
    {
        u.hasTorBoxToken = true;
        u.torBoxToken = token;
    }
    else
    {
        u.hasRealDebridToken = true;
        u.realDebridToken = token;
    }
    return updateSettings(u);
}

Result<Settings> IPegasus::clearDebridToken(DebridService service)
{
    return setDebridToken(service, std::string());
}

// ------------------------------------------------------------- PegasusV1 ----

PegasusV1::PegasusV1(std::shared_ptr<http::Transport> transport) : transport_(std::move(transport))
{
}


Result<Health> PegasusV1::health()
{
    return map<Health>(call(*transport_, "GET", "/api/health"), parse_health); // app.js:4293
}

Result<Settings> PegasusV1::getSettings()
{
    return map<Settings>(call(*transport_, "GET", "/api/settings"), parse_settings); // app.js:4715
}

Result<Settings> PegasusV1::updateSettings(const SettingsUpdate &u)
{
    // app.js:5103 (downloadRoot), 5116 (downloadConnections), 5129/5147 (tokens): one key per call in
    // the UI; here several may be combined, always in this order.
    ObjectBuilder body;
    bool any = false;
    if (u.hasDownloadRoot)
    {
        body.str("downloadRoot", u.downloadRoot);
        any = true;
    }
    if (u.hasDownloadConnections)
    {
        if (!logic::validDownloadConnections(u.downloadConnections)) // app.js:5112
            return fail_client<Settings>("Download connections must be between 1 and " +
                                         std::to_string(logic::kMaxDownloadConnections) + ".");
        body.num("downloadConnections", u.downloadConnections);
        any = true;
    }
    if (u.hasRealDebridToken)
    {
        body.str("realDebridToken", u.realDebridToken);
        any = true;
    }
    if (u.hasTorBoxToken)
    {
        body.str("torBoxToken", u.torBoxToken);
        any = true;
    }
    if (!any)
        return fail_client<Settings>("No settings to change.");
    Reply reply = call(*transport_, "POST", "/api/settings", body.done());
    if (!reply.error.ok())
        return fail<Settings>(reply.error);
    // The token flows apply the reply directly (app.js:5133); the folder/connections flows reload.
    if (reply.json.has("downloadRoot"))
        return map<Settings>(std::move(reply), parse_settings);
    return getSettings();
}

Result<DebridStatus> PegasusV1::refreshDebrid(DebridService service)
{
    // app.js:5165
    return map<DebridStatus>(call(*transport_, "POST", "/api/debrid/refresh", ObjectBuilder().str("service", service_id(service)).done()),
                             parse_debrid);
}

Result<std::vector<Source>> PegasusV1::listSources()
{
    return map<std::vector<Source>>(call(*transport_, "GET", "/api/sources"), parse_sources); // app.js:4301
}

Result<std::vector<Source>> PegasusV1::addSourceUrl(const std::string &url)
{
    // app.js:4831
    return map<std::vector<Source>>(call(*transport_, "POST", "/api/sources/url", ObjectBuilder().str("url", url).done()), parse_sources);
}

Result<std::vector<Source>> PegasusV1::uploadSourceCatalog(const std::string &catalogJson)
{
    // app.js:4804 body: await file.text() (raw catalog JSON, not re-serialised)
    return map<std::vector<Source>>(call(*transport_, "POST", "/api/sources/upload", catalogJson), parse_sources);
}

Result<std::vector<Source>> PegasusV1::deleteSource(std::int64_t sourceId)
{
    return map<std::vector<Source>>(call(*transport_, "POST", "/api/sources/delete", ObjectBuilder().num("sourceId", sourceId).done()),
                                    parse_sources); // app.js:4942
}

Result<std::vector<Source>> PegasusV1::refreshSource(std::int64_t sourceId)
{
    return map<std::vector<Source>>(call(*transport_, "POST", "/api/sources/refresh", ObjectBuilder().num("sourceId", sourceId).done()),
                                    parse_sources); // app.js:4955
}

Result<std::vector<Source>> PegasusV1::setSourceEnabled(std::int64_t sourceId, bool enabled)
{
    // app.js:4979
    return map<std::vector<Source>>(
        call(*transport_, "POST", "/api/sources/enabled", ObjectBuilder().num("sourceId", sourceId).boolean("enabled", enabled).done()),
        parse_sources);
}

Result<DefaultSources> PegasusV1::getDefaultSources(bool forceRefresh)
{
    // app.js:2774
    return map<DefaultSources>(call(*transport_, "GET", forceRefresh ? "/api/default-sources?refresh=1" : "/api/default-sources"),
                               [](const Value &v) {
                                   DefaultSources d;
                                   d.status = text(v, "status");
                                   d.available = flag(v, "available");
                                   d.schemaVersion = num(v, "schemaVersion");
                                   d.revision = num(v, "revision");
                                   d.onboardingRevision = num(v, "onboardingRevision");
                                   d.acceptedOnboardingRevision = num(v, "acceptedOnboardingRevision");
                                   d.onboardingRequired = flag(v, "onboardingRequired");
                                   d.hasNew = flag(v, "hasNew");
                                   d.error = text(v, "error");
                                   for (const Value &s : v["sources"].items())
                                       d.sources.push_back(parse_default_source(s));
                                   return d;
                               });
}

Result<DefaultSourcesApplyResult> PegasusV1::applyDefaultSources(std::int64_t revision, const std::vector<std::string> &selectedIds)
{
    // app.js:2819: { revision: Number(state.defaultSources.revision || 0), selectedIds }
    return map<DefaultSourcesApplyResult>(
        call(*transport_, "POST", "/api/default-sources/apply", ObjectBuilder().num("revision", revision).str_array("selectedIds", selectedIds).done()),
        [](const Value &v) {
            DefaultSourcesApplyResult r;
            for (const Value &f : v["failures"].items())
            {
                if (f.is_string())
                    r.failures.push_back(f.as_string());
                else if (f.is_object())
                {
                    std::string line = text(f, "name");
                    if (line.empty())
                        line = text(f, "id");
                    const std::string &msg = !text(f, "error").empty() ? text(f, "error") : text(f, "message");
                    if (!msg.empty())
                        line += line.empty() ? msg : ": " + msg;
                    r.failures.push_back(line);
                }
                else
                    r.failures.push_back(std::string());
            }
            if (v["sources"].is_array())
            {
                r.hasSources = true;
                r.sources = parse_sources(v);
            }
            return r;
        });
}

Result<Empty> PegasusV1::markDefaultSourcesSeen()
{
    return ack(call(*transport_, "POST", "/api/default-sources/seen")); // app.js:2860 (no body)
}

Result<std::vector<Package>> PegasusV1::getLibrary()
{
    // app.js:4311
    return map<std::vector<Package>>(call(*transport_, "GET", "/api/library", "", "", http::kLibraryLimit), [](const Value &v) {
        std::vector<Package> out;
        const std::vector<Value> &items = v["packages"].items();
        out.reserve(items.size());
        for (const Value &p : items)
            out.push_back(parse_package(p));
        return out;
    });
}

namespace
{
// Adapts the JSON array scanner to a PackageSink: one small DOM per package.
struct LibraryElements final : json::ElementSink
{
    PackageSink &sink;
    bool bad = false;
    explicit LibraryElements(PackageSink &s) : sink(s) {}
    bool element(std::string_view text) override
    {
        Value v;
        if (!json::parse(text, &v))
        {
            bad = true;
            return false;
        }
        sink.package(parse_package(v));
        return true;
    }
};
} // namespace

Result<Empty> IPegasus::streamLibrary(PackageSink &sink)
{
    Result<Empty> out;
    auto all = getLibrary();
    out.error = all.error;
    if (all.ok())
    {
        for (Package &p : all.value)
            sink.package(std::move(p));
    }
    return out;
}

Result<Empty> PegasusV1::streamLibrary(PackageSink &sink)
{
    Result<Empty> out;
    http::Request request;
    request.method = "GET";
    request.path = "/api/library"; // app.js:4311
    request.max_bytes = http::kLibraryLimit;
    const http::Response response = transport_->send(request);
    if (!response.transported())
    {
        out.error = make_error(ApiError::Kind::transport, 0, response.error.empty() ? "no response" : response.error);
        return out;
    }
    if (response.status < 200 || response.status >= 300)
    {
        std::string message;
        Value parsed;
        if (response.body.size() < (1u << 20) && json::parse(response.body, &parsed) && parsed.is_object() &&
            parsed["error"].is_string())
            message = parsed["error"].as_string();
        if (message.empty())
            message = "HTTP " + std::to_string(response.status);
        out.error = make_error(ApiError::Kind::http, response.status, message);
        return out;
    }
    if (response.body.empty())
        return out; // `{}`: no packages
    LibraryElements elements(sink);
    const json::ScanResult scan = json::for_each_array_element(response.body, "packages", elements);
    if (scan == json::ScanResult::malformed || elements.bad)
        out.error = make_error(ApiError::Kind::parse, response.status, "Invalid JSON response: malformed catalogue");
    return out;
}

Result<QueueSnapshot> PegasusV1::getQueue()
{
    return map<QueueSnapshot>(call(*transport_, "GET", "/api/queue"), parse_queue); // app.js:4678
}

Result<DispatchResponse> PegasusV1::enqueuePackage(std::int64_t packageId, std::int64_t linkIndex)
{
    // app.js:4869
    return map<DispatchResponse>(
        call(*transport_, "POST", "/api/queue", ObjectBuilder().num("packageId", packageId).num("linkIndex", linkIndex).done()),
        parse_queue_response);
}

Result<DispatchResponse> PegasusV1::queueDirect(const std::string &url, const std::string &fileName, const std::string &contentType)
{
    if (url.empty())
        return fail_client<DispatchResponse>("Enter an http:// or https:// link.");
    // app.js:3560
    return map<DispatchResponse>(
        call(*transport_, "POST", "/api/queue/direct",
             ObjectBuilder().str("url", url).str("fileName", fileName).str("contentType", contentType).done()),
        parse_queue_response);
}

Result<QueueResponse> PegasusV1::pauseJob(std::int64_t jobId)
{
    return map<QueueResponse>(call(*transport_, "POST", "/api/queue/pause", stringify_job(jobId)), parse_queue_response); // app.js:4997
}

Result<QueueResponse> PegasusV1::resumeJob(std::int64_t jobId)
{
    return map<QueueResponse>(call(*transport_, "POST", "/api/queue/resume", stringify_job(jobId)), parse_queue_response);
}

Result<QueueResponse> PegasusV1::cancelJob(std::int64_t jobId)
{
    return map<QueueResponse>(call(*transport_, "POST", "/api/queue/cancel", stringify_job(jobId)), parse_queue_response);
}

Result<QueueResponse> PegasusV1::retryJob(std::int64_t jobId)
{
    return map<QueueResponse>(call(*transport_, "POST", "/api/queue/retry", stringify_job(jobId)), parse_queue_response);
}

Result<QueueResponse> PegasusV1::clearFinished()
{
    return map<QueueResponse>(call(*transport_, "POST", "/api/queue/clear-finished"), parse_queue_response); // app.js:5250 (no body)
}

Result<QueueResponse> PegasusV1::submitArchivePassword(std::int64_t jobId, const std::string &password)
{
    if (password.empty()) // app.js:5013
        return fail_client<QueueResponse>("Archive password is required.");
    return map<QueueResponse>(
        call(*transport_, "POST", "/api/queue/archive-password", ObjectBuilder().num("jobId", jobId).str("password", password).done()),
        parse_queue_response); // app.js:5021
}

Result<QueueResponse> PegasusV1::submitArchiveChoice(std::int64_t jobId, std::int64_t candidateIndex)
{
    if (candidateIndex < 0) // app.js:5039
        return fail_client<QueueResponse>("Choose extracted content first.");
    return map<QueueResponse>(
        call(*transport_, "POST", "/api/queue/archive-choice", ObjectBuilder().num("jobId", jobId).num("candidateIndex", candidateIndex).done()),
        parse_queue_response); // app.js:5043
}

Result<ResolveProbe> PegasusV1::resolveProbe(const std::string &url)
{
    return map<ResolveProbe>(call(*transport_, "POST", "/api/resolve/probe", ObjectBuilder().str("url", url).done()), parse_probe); // app.js:3547
}

Result<DebridQueueResult> PegasusV1::debridQueue(std::int64_t packageId, std::int64_t linkIndex, DebridService service)
{
    // app.js:4907
    return map<DebridQueueResult>(
        call(*transport_, "POST", "/api/debrid/queue",
             ObjectBuilder().num("packageId", packageId).num("linkIndex", linkIndex).str("service", service_id(service)).done()),
        parse_debrid_queue);
}

Result<DebridQueueResult> PegasusV1::debridQueueSelection(const std::string &selectionId, const std::vector<std::int64_t> &indexes,
                                                          DebridService service)
{
    // app.js:3923-3939: indexes filtered to integers >= 0
    std::vector<std::int64_t> clean;
    for (const std::int64_t i : indexes)
        if (i >= 0)
            clean.push_back(i);
    if (clean.empty())
        return fail_client<DebridQueueResult>("Choose at least one file.");
    return map<DebridQueueResult>(
        call(*transport_, "POST", "/api/debrid/queue",
             ObjectBuilder().str("selectionId", selectionId).int_array("indexes", clean).str("service", service_id(service)).done()),
        parse_debrid_queue);
}

Result<CaptureStart> PegasusV1::providerResolve(std::int64_t packageId, std::int64_t linkIndex)
{
    // app.js:4078-4081 (automated)
    return map<CaptureStart>(
        call(*transport_, "POST", "/api/provider/resolve", ObjectBuilder().num("packageId", packageId).num("linkIndex", linkIndex).done()),
        [](const Value &v) {
            CaptureStart c;
            c.sessionId = num(v, "sessionId");
            return c;
        });
}

Result<CaptureStart> PegasusV1::browserOpen(const std::string &url, bool guarded, std::optional<std::int64_t> packageId,
                                            std::optional<std::int64_t> linkIndex)
{
    // app.js:4081-4086 {url, guarded, packageId, linkIndex}; app.js:4929 {url, guarded:false}
    ObjectBuilder body;
    body.str("url", url).boolean("guarded", guarded);
    if (packageId)
        body.num("packageId", *packageId);
    if (linkIndex)
        body.num("linkIndex", *linkIndex);
    return map<CaptureStart>(call(*transport_, "POST", "/api/browser/open", body.done()), [](const Value &v) {
        CaptureStart c;
        c.sessionId = num(v, "sessionId");
        return c;
    });
}

Result<CaptureStatus> PegasusV1::captureStatus(std::int64_t sessionId)
{
    // app.js:4032
    return map<CaptureStatus>(
        call(*transport_, "GET", "/api/browser/capture/status?sessionId=" + logic::encodeURIComponent(std::to_string(sessionId))), parse_capture);
}

Result<CaptureStatus> PegasusV1::captureActive()
{
    return map<CaptureStatus>(call(*transport_, "GET", "/api/browser/capture/active"), parse_capture); // app.js:4169
}

Result<Empty> PegasusV1::captureCancel(std::int64_t sessionId)
{
    return ack(call(*transport_, "POST", "/api/browser/capture/cancel", ObjectBuilder().num("sessionId", sessionId).done())); // app.js:3996
}

Result<std::vector<Mount>> PegasusV1::getMounts()
{
    // app.js:4730
    return map<std::vector<Mount>>(call(*transport_, "GET", "/api/mounts"), [](const Value &v) {
        std::vector<Mount> out;
        for (const Value &m : v["mounts"].items())
            out.push_back(parse_mount(m));
        return out;
    });
}

Result<BrowseResult> PegasusV1::browse(const std::string &path)
{
    // app.js:4740
    return map<BrowseResult>(call(*transport_, "GET", "/api/browse?path=" + logic::encodeURIComponent(path)), [](const Value &v) {
        BrowseResult b;
        b.path = text(v, "path");
        for (const Value &e : v["entries"].items())
            b.entries.push_back(parse_entry(e));
        return b;
    });
}

Result<Empty> PegasusV1::mkdir(const std::string &fullPath)
{
    return ack(call(*transport_, "POST", "/api/mkdir", ObjectBuilder().str("path", fullPath).done())); // app.js:5093
}

Result<LogBatch> PegasusV1::getLogs(std::int64_t sinceSeq)
{
    // app.js:4771
    return map<LogBatch>(call(*transport_, "GET", "/api/logs?since=" + std::to_string(sinceSeq)), [](const Value &v) {
        LogBatch b;
        b.count = num(v, "count");
        b.capacity = num(v, "capacity");
        b.lastSeq = num(v, "lastSeq");
        for (const Value &l : v["logs"].items())
        {
            LogEntry e;
            e.seq = num(l, "seq");
            e.ts = num(l, "ts");
            e.level = text(l, "level");
            e.component = text(l, "component");
            e.message = text(l, "message");
            b.logs.push_back(std::move(e));
        }
        return b;
    });
}

Result<Empty> PegasusV1::clearLogs()
{
    return ack(call(*transport_, "POST", "/api/logs/clear")); // app.js:5324 (no body)
}

Result<Empty> PegasusV1::clientLog(const std::string &level, const std::string &message, const std::string &detail,
                                   const std::string &source)
{
    // app.js:585: { level, source: 'frontend', message: String(message), detail }
    return ack(call(*transport_, "POST", "/api/client-log",
                    ObjectBuilder().str("level", level).str("source", source).str("message", message).str("detail", detail).done()));
}

Result<InstalledGames> PegasusV1::getInstalledGames()
{
    Reply reply = call(*transport_, "GET", "/appdb"); // app.js:4338
    if (!reply.error.ok())
        return fail<InstalledGames>(reply.error);
    Result<InstalledGames> r;
    const Value &v = reply.json;
    r.value.databaseAvailable = flag(v, "databaseAvailable", true);
    r.value.databaseError = text(v, "databaseError");
    r.value.error = text(v, "error");
    r.value.count = num(v, "count");
    r.value.elapsedMs = num(v, "elapsedMs");
    for (const Value &t : v["titles"].items())
        r.value.titles.push_back(parse_game(t));
    // app.js:4339-4340
    if (!r.value.error.empty() || !r.value.databaseAvailable)
    {
        std::string message = r.value.error;
        if (!r.value.databaseError.empty())
            message += (message.empty() ? "" : " ") + r.value.databaseError;
        if (message.empty())
            message = "Unable to read the PS5 app database.";
        r.error = make_error(ApiError::Kind::http, 200, message);
    }
    return r;
}

Result<std::string> PegasusV1::fetchAppdbArt(const std::string &urlOrPath)
{
    if (urlOrPath.rfind("/appdb/", 0) != 0)
        return fail_client<std::string>("Not an appdb art path.");
    Reply reply = call(*transport_, "GET", urlOrPath, "", "", 4u << 20, true);
    Result<std::string> r;
    r.error = reply.error;
    r.value = std::move(reply.rawBody);
    return r;
}

Result<AprVersions> PegasusV1::getAprVersions()
{
    // app.js:1095-1098 (latestReleaseId, versions[]). 503 {"error":"APR Emu manifest has too many versions"} is an http error.
    return map<AprVersions>(call(*transport_, "GET", "/api/apr-emu/versions"), [](const Value &v) {
        AprVersions a;
        a.latestReleaseId = text(v, "latestReleaseId");
        for (const Value &x : v["versions"].items())
        {
            AprVersion ver;
            ver.id = text(x, "id");
            ver.version = text(x, "version");
            ver.build = text(x, "build");
            ver.size = num(x, "size");
            a.versions.push_back(std::move(ver));
        }
        return a;
    });
}

Result<AprJob> PegasusV1::aprUpdateStart(const std::string &titleId, const std::string &versionId)
{
    if (titleId.empty())
        return fail_client<AprJob>("Title id required.");
    // app.js:4545: versionId ? { titleId, versionId } : { titleId }
    ObjectBuilder body;
    body.str("titleId", titleId);
    if (!versionId.empty())
        body.str("versionId", versionId);
    return map<AprJob>(call(*transport_, "POST", "/api/apr-emu/update/start", body.done()), parse_apr_job);
}

Result<AprJob> PegasusV1::aprUpdateStatus(const JobRef &ref)
{
    return map<AprJob>(call(*transport_, "GET", "/api/apr-emu/update/status?" + job_query(ref)), parse_apr_job); // app.js:4458
}

Result<AprJob> PegasusV1::aprRestoreStatus(const JobRef &ref)
{
    return map<AprJob>(call(*transport_, "GET", "/api/apr-emu/restore/status?" + job_query(ref)), parse_apr_job); // app.js:4513
}

Result<DeleteJob> PegasusV1::installedGameDelete(const std::string &titleId)
{
    if (titleId.empty())
        return fail_client<DeleteJob>("Title id required.");
    return map<DeleteJob>(call(*transport_, "POST", "/api/installed-games/delete", ObjectBuilder().str("titleId", titleId).done()),
                          parse_delete_job); // app.js:4592
}

Result<DeleteJob> PegasusV1::installedGameDeleteStatus(const JobRef &ref)
{
    return map<DeleteJob>(call(*transport_, "GET", "/api/installed-games/delete/status?" + job_query(ref)), parse_delete_job); // app.js:4650
}

Result<std::vector<Location>> PegasusV1::fileLocations()
{
    // fm.js:948
    return map<std::vector<Location>>(call(*transport_, "GET", "/api/files/locations", "", "", 8u << 20), [](const Value &v) {
        std::vector<Location> out;
        for (const Value &m : v["locations"].items())
            out.push_back(parse_mount(m));
        return out;
    });
}

Result<FileList> PegasusV1::fileList(const std::string &path)
{
    // fm.js:918-923: normalizeFilePath(path || '/') || '/'
    std::string normalized = logic::normalizeFilePath(path);
    if (normalized.empty())
        normalized = "/";
    return map<FileList>(call(*transport_, "GET", "/api/files/list?path=" + logic::encodeURIComponent(normalized), "", "", 8u << 20),
                         [](const Value &v) {
                             FileList l;
                             l.path = text(v, "path");
                             l.writable = flag(v, "writable", true);
                             for (const Value &e : v["entries"].items())
                                 l.entries.push_back(parse_entry(e));
                             return l;
                         });
}

Result<std::vector<FileOperation>> PegasusV1::fileOperations()
{
    return map<std::vector<FileOperation>>(call(*transport_, "GET", "/api/files/operations"), parse_operations); // fm.js:1062
}

Result<FileEntry> PegasusV1::fileMkdir(const std::string &parent, const std::string &name)
{
    const std::string clean = logic::validateName(name); // fm.js:1241
    if (clean.empty())
        return fail_client<FileEntry>("Invalid name.");
    return map<FileEntry>(call(*transport_, "POST", "/api/files/mkdir", ObjectBuilder().str("parent", parent.empty() ? "/" : parent).str("name", clean).done()),
                          parse_entry); // fm.js:1243
}

Result<FileEntry> PegasusV1::fileRename(const std::string &path, const std::string &name)
{
    const std::string clean = logic::validateName(name); // fm.js:1255
    if (clean.empty())
        return fail_client<FileEntry>("Invalid name.");
    return map<FileEntry>(call(*transport_, "POST", "/api/files/rename", ObjectBuilder().str("path", path).str("name", clean).done()),
                          parse_entry); // fm.js:1261
}

Result<std::vector<FileOperation>> PegasusV1::fileDelete(const std::vector<std::string> &paths)
{
    const std::vector<std::string> selected = logic::uniquePaths(paths); // fm.js:1273
    if (selected.empty())
        return fail_client<std::vector<FileOperation>>("No items selected.");
    return map<std::vector<FileOperation>>(call(*transport_, "POST", "/api/files/delete", ObjectBuilder().str_array("paths", selected).done()),
                                           parse_operations); // fm.js:1274
}

namespace
{
Result<std::vector<FileOperation>> transfer(http::Transport &t, const char *route, const std::vector<std::string> &paths,
                                            const std::string &destination, Collision collision)
{
    const std::vector<std::string> selected = logic::uniquePaths(paths);
    if (selected.empty())
        return fail_client<std::vector<FileOperation>>("No items selected.");
    const std::string dest = logic::normalizeFilePath(destination);
    if (dest.empty())
        return fail_client<std::vector<FileOperation>>("Choose a destination folder.");
    // fm.js:1291-1297 / 1311-1317: { paths, destination, collision }
    return map<std::vector<FileOperation>>(
        call(t, "POST", route,
             ObjectBuilder().str_array("paths", selected).str("destination", dest).str("collision", logic::toString(collision)).done()),
        parse_operations);
}
} // namespace

Result<std::vector<FileOperation>> PegasusV1::fileCopy(const std::vector<std::string> &paths, const std::string &destination, Collision collision)
{
    return transfer(*transport_, "/api/files/copy", paths, destination, collision);
}

Result<std::vector<FileOperation>> PegasusV1::fileMove(const std::vector<std::string> &paths, const std::string &destination, Collision collision)
{
    return transfer(*transport_, "/api/files/move", paths, destination, collision);
}

Result<std::vector<FileOperation>> PegasusV1::fileExtract(const std::vector<std::string> &paths, const std::string &destination, Collision collision)
{
    return transfer(*transport_, "/api/files/extract", paths, destination, collision);
}

Result<std::vector<FileOperation>> PegasusV1::fileExtractPassword(std::int64_t operationId, const std::string &password)
{
    if (password.empty()) // fm.js:1329
        return fail_client<std::vector<FileOperation>>("Archive password is required.");
    return map<std::vector<FileOperation>>(
        call(*transport_, "POST", "/api/files/extract/password", ObjectBuilder().num("operationId", operationId).str("password", password).done()),
        parse_operations); // fm.js:1333
}

Result<std::vector<FileOperation>> PegasusV1::filePkgInstall(const std::string &path)
{
    return map<std::vector<FileOperation>>(call(*transport_, "POST", "/api/files/pkg/install", ObjectBuilder().str("path", path).done()),
                                           parse_operations); // fm.js:1348
}

Result<Empty> PegasusV1::fileOperationCancel(std::int64_t operationId)
{
    return ack(call(*transport_, "POST", "/api/files/operation/cancel", ObjectBuilder().num("operationId", operationId).done())); // fm.js:1444
}

Result<Empty> PegasusV1::fileUpload(const std::string &directory, const std::string &name, const std::string &relativePath,
                                    const std::string &data)
{
    // fm.js:1377-1383: URLSearchParams({directory, name: file.name || 'upload.bin'}) [+ relativePath], PUT raw body.
    std::string query = "directory=" + logic::formUrlEncode(directory) + "&name=" +
                        logic::formUrlEncode(name.empty() ? "upload.bin" : name);
    if (!relativePath.empty())
        query += "&relativePath=" + logic::formUrlEncode(relativePath);
    return ack(call(*transport_, "PUT", "/api/files/upload?" + query, data, "application/octet-stream"));
}

Result<std::string> PegasusV1::fileDownload(const std::string &path, std::size_t maxBytes)
{
    Reply reply = call(*transport_, "GET", fileDownloadPath(path, false), "", "", maxBytes, true); // fm.js:1363
    Result<std::string> r;
    r.error = reply.error;
    r.value = std::move(reply.rawBody);
    return r;
}

Result<std::string> PegasusV1::fileArchive(const std::string &path, std::size_t maxBytes)
{
    Reply reply = call(*transport_, "GET", fileDownloadPath(path, true), "", "", maxBytes, true);
    Result<std::string> r;
    r.error = reply.error;
    r.value = std::move(reply.rawBody);
    return r;
}

std::string PegasusV1::fileDownloadPath(const std::string &path, bool isDirectory) const
{
    // fm.js:1363-1365: dir => /api/files/archive, else /api/files/download
    return std::string(isDirectory ? "/api/files/archive" : "/api/files/download") + "?path=" + logic::encodeURIComponent(path);
}


} // namespace wings
