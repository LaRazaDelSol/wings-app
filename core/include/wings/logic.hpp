// Wings core - Behaviour that lives in the Pegasus web-UI JavaScript, not in the backend.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything here is a port of recon/pegasus-app.js (Pegasus DL 1.11.0) or
// recon/pegasus-file-manager.js. Each function names the JS function and
// line numbers it mirrors ("app.js:NNN" / "fm.js:NNN"). Where the C++ port
// deliberately differs (no DOM, no window.location), the comment says so.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wings/model.hpp"

namespace wings::logic
{

// ------------------------------------------------------------------ URLs ----

struct UrlParts
{
    std::string scheme; // "http" | "https"
    std::string host;   // lower-case hostname, no port, IPv6 keeps brackets
    std::string port;   // "" when absent or default
    std::string path;   // pathname, always starts with '/'
    std::string query;  // without '?'
    bool hasQuery = false;
    std::string fragment; // without '#'
    bool hasFragment = false;
    std::string userinfo;
    std::string href; // serialised, like URL.href
};

// Absolute http(s) URL parser approximating the WHATWG URL parser (what
// `new URL(value)` does). Returns false for anything else. Not handled:
// IDNA/punycode, percent-encoded hosts, relative references.
bool parseHttpUrl(std::string_view text, UrlParts *out);

// app.js:337 safeHttpUrl. Returns URL.href for http(s) URLs, "" otherwise.
// Difference: the JS resolves relative input against window.location; here
// relative or empty input yields "".
std::string safeHttpUrl(std::string_view value);

// app.js:376 safeAbsoluteHttpUrl. Trims, requires an absolute http(s) URL,
// returns href with trailing slashes removed.
std::string safeAbsoluteHttpUrl(std::string_view value);

// app.js:346 urlHasPkgExtension: pathname ends in ".pkg" (case-insensitive).
// This is the rule that routes a link to the native PS5 handoff.
bool urlHasPkgExtension(std::string_view value);

// app.js:356 decodeBase64DownloadUrl. Catalog links may be base64 (standard or
// url-safe alphabet) encodings of an http(s) URL. Returns the decoded URL when
// the value is not already http(s), is valid base64 and decodes to an http(s)
// URL; otherwise returns the input unchanged.
std::string decodeBase64DownloadUrl(std::string_view value);

// app.js:372 downloadLinkUrl.
std::string downloadLinkUrl(std::string_view value);

// app.js:385 hostFromUrl: hostname without a leading "www.", "" if not http(s).
std::string hostFromUrl(std::string_view value);

// app.js:395 hostFromDownloadUrl: decodes base64 first; "" unless result is http(s).
std::string hostFromDownloadUrl(std::string_view value);

// app.js:400 providerHostMatches: equal, or host ends with "." + providerHost
// (after lower-casing and removing a leading "www." from both).
bool providerHostMatches(std::string_view host, std::string_view providerHost);

// app.js:324 packageLinks. Drops links without a url; index is the link's own
// integer index when it has one, else its position AFTER filtering; name
// defaults to "Link N". Returned DownloadLink::url is the raw catalog value.
std::vector<DownloadLink> packageLinks(const Package &pkg);

// app.js:479 packageLinkByIndex (`Number(linkIndex) || 0`). nullptr if absent.
const DownloadLink *packageLinkByIndex(const std::vector<DownloadLink> &links, std::int64_t linkIndex);

// -------------------------------------------------------------- providers ---

struct Provider
{
    std::string id;
    std::string name;
    std::vector<std::string> hosts;
    bool matchSubdomains = true; // app.js:40 mediafire sets false
    bool requiresBrowser = true; // app.js:40 mediafire sets false (automated only)
    bool guarded = true;         // app.js:43,45 filek/vikingfile set false (direct only)
};

// app.js:37-47 tables (Pegasus 1.11.0).
const std::vector<Provider> &automatedProviders();
const std::vector<Provider> &directBrowserProviders();
const std::vector<Provider> &unsupportedProviders(); // empty in 1.11.0

// app.js:406 providerMatchesHost. `matchSubdomains == false` => exact host only.
bool providerMatchesHost(const Provider &provider, std::string_view host);

enum class SupportKind : std::uint8_t
{
    automated,     // Pegasus resolves it itself (POST /api/provider/resolve)
    directBrowser, // opened in the PS5 browser and captured (POST /api/browser/open)
    unsupported,   // browser only, no capture
    unknown,       // not in any table: manual capture, guarded
};

// app.js:458 downloadSupportForUrl result.
struct DownloadSupport
{
    SupportKind kind = SupportKind::unknown;
    bool automated = false;
    bool blocked = false;     // always false in 1.11.0
    bool browserOnly = false; // true only for unsupported providers
    bool guarded = true;
    std::string state; // "supported" | "unsupported" | "manual" (JS `state`)
    std::string label; // "Supported" | "Not supported" | "Unknown"
    bool hasProvider = false;
    Provider provider;
};

// app.js:458-473. Order: unsupported table, automated table, direct table, unknown.
DownloadSupport downloadSupportForUrl(std::string_view url);

// ----------------------------------------------------------------- debrid ---

struct DebridServiceInfo
{
    DebridService service;
    const char *id;           // value sent as `service` ("real-debrid" | "torbox")
    const char *label;        // "Real-Debrid" | "TorBox"
    const char *tokenSetting; // key of the token in POST /api/settings
};

// app.js:48-73.
const std::vector<DebridServiceInfo> &debridServices();
const DebridServiceInfo &debridServiceInfo(DebridService service);
// app.js:417 debridServiceById: unknown id falls back to real-debrid.
const DebridServiceInfo &debridServiceById(std::string_view id);
const DebridStatus &debridStatusFor(const Settings &settings, DebridService service);

// app.js:429 debridReady: configured && valid && has domains.
bool debridReady(const DebridStatus &status);

// app.js:438 debridServicesForUrl: services that are ready and list a domain
// matching the link's host, in DEBRID_SERVICES order.
std::vector<DebridService> debridServicesForUrl(std::string_view url, const Settings &settings);

// app.js:447.
bool realDebridSupportsUrl(std::string_view url, const Settings &settings);

// app.js:4850-4900 queueDownloadLink: what the web UI does when the user
// activates a download link. Pure decision; the caller performs the request.
enum class LinkActionKind : std::uint8_t
{
    linkNotFound,    // no such package link / empty url ("Download link not found.")
    nativePkg,       // .pkg => POST /api/queue {packageId, linkIndex}
    debrid,          // exactly one ready service => POST /api/debrid/queue
    chooseDebrid,    // several services => ask the user, then /api/debrid/queue
    browserOnly,     // POST /api/browser/open {url, guarded:false}
    blocked,         // "<provider> is not supported."
    automatedCapture,// POST /api/provider/resolve {packageId, linkIndex}
    manualCapture,   // POST /api/browser/open {url, guarded, packageId, linkIndex}
};

struct LinkAction
{
    LinkActionKind kind = LinkActionKind::linkNotFound;
    std::string url; // decoded url (downloadLinkUrl)
    std::int64_t packageId = 0;
    std::int64_t linkIndex = 0; // link.index
    std::vector<DebridService> services; // for debrid / chooseDebrid
    DownloadSupport support;
    bool guarded = true;          // app.js:4152 `!support || support.guarded !== false`
    bool requiresBrowser = true;  // app.js:4153
};

LinkAction decideLinkAction(const Package &pkg, std::int64_t linkIndex, const Settings &settings);

// ------------------------------------------------------- dispatch handling --

// app.js:3429. dispatchResult first, then dispatchResults[]; keeps entries with
// ok && kind == "native-pkg" && accepted.
std::vector<DispatchResult> nativeDispatchResults(const QueueResponse &response);
// app.js:3438. ok && kind == "queued".
std::vector<DispatchResult> queuedDispatchResults(const QueueResponse &response);

struct NativePkgConfirmation
{
    std::string title;
    std::string message;
    std::vector<std::string> contentIds;
};
// app.js:3481 openNativePkgConfirmation (text only). Empty input => empty title/message.
NativePkgConfirmation nativePkgConfirmation(const std::vector<DispatchResult> &nativeResults);

struct DispatchOutcome
{
    std::vector<DispatchResult> native;
    std::vector<DispatchResult> queued;
    bool queueIncluded = false;    // reply carried `jobs`: use response.queue directly (3503-3504)
    bool needsQueueReload = false; // queued results without jobs: GET /api/queue (3505-3507)
    bool schedulePoll = false;     // queued results: poll the queue soon (3508-3511)
    std::string toast;             // message shown to the user (3512-3519)
    bool showNativeConfirmation = false;
    NativePkgConfirmation confirmation;
};
// app.js:3500 finishDispatchResponse.
DispatchOutcome finishDispatchResponse(const QueueResponse &response, const std::string &queuedMessage);

// app.js:3551-3557 submitManualLink: why a probe result cannot be queued as a
// direct download. "" when it can.
std::string probeRejection(const ResolveProbe &probe);

// app.js:4006-4020 captureStatusText.
std::string captureStatusText(CaptureState state, bool automated, bool requiresBrowser);
bool captureIsTerminal(CaptureState state); // completed | failed | cancelled (app.js:3994, 4038-4051)
CaptureState captureStateFromString(std::string_view text);

// ------------------------------------------------------------------ queue ---

const char *toString(JobStatus status);
JobStatus jobStatusFromString(std::string_view text);
JobPhase jobPhaseFromString(std::string_view text);
const char *toString(JobPhase phase);

bool isActiveJobStatus(JobStatus status);   // app.js:8 ACTIVE_QUEUE_STATUSES
bool isRunningJobStatus(JobStatus status);  // app.js:9 RUNNING_QUEUE_STATUSES
bool isPausableJobStatus(JobStatus status); // app.js:10 PAUSABLE_QUEUE_STATUSES
bool isTerminalJobStatus(JobStatus status); // completed | failed | cancelled
// app.js:4685 queuePollDelay: 1000 ms with active jobs, else 2500 ms.
int queuePollDelayMs(const std::vector<QueueJob> &jobs);
// app.js:1795-1797 queueJobStreamMode.
bool queueJobStreamMode(const QueueJob &job);

// ------------------------------------------------------------ installed/APR -

enum class AprState : std::uint8_t
{
    update,
    unavailable,
    current,
    unsupported,
    missing,
    unknown,
};
// app.js:851 aprEmuState.
AprState aprEmuState(const AprEmu &apr);
// app.js:862 aprEmuVersionText.
std::string aprEmuVersionText(const AprEmu &apr);
// app.js:1008-1041 version label / ordering (latest release first, then newest, release before debug).
std::string aprVersionLabel(const AprVersion &version);
std::vector<AprVersion> sortedAprVersions(std::vector<AprVersion> versions, const std::string &latestReleaseId);

// app.js:763 installedArtUrl. kind: "icon" or "pic0".
std::string installedArtUrl(const InstalledGame &game, const std::string &kind);
// app.js:769.
std::string installedVersionText(const InstalledGame &game);
// app.js:806 psFirmwareVersion: "9.60" from "0x0900000000000000" etc., "" if unknown.
std::string psFirmwareVersion(std::string_view raw);
// app.js:832 installedFirmwareText: "FW 9.60" or "".
std::string installedFirmwareText(std::string_view raw);
// app.js:838-849.
std::string installedFormatText(const InstalledGame &game);
std::string installedLocationText(const InstalledGame &game);

// --------------------------------------------------------- settings/browse --

constexpr int kDefaultDownloadConnections = 16; // app.js:6
constexpr int kMaxDownloadConnections = 64;     // app.js:7
// app.js:5109-5115: integer in 1..64.
bool validDownloadConnections(std::int64_t value);

// app.js:251 pathParent / :257 pathJoin.
std::string pathParent(std::string_view path);
std::string pathJoin(std::string_view base, std::string_view name);
// app.js:261 mountForPath: longest mount path that equals or prefixes `path`.
const Mount *mountForPath(const std::vector<Mount> &mounts, std::string_view path);
// app.js:275 canSelectBrowsePath: containing mount exists and writable !== false.
bool canSelectBrowsePath(const std::vector<Mount> &mounts, std::string_view path);
// app.js:5088 / fm.js:1226 validateName: trimmed; "" when empty, contains '/', or is "." / "..".
std::string validateName(std::string_view name);

// -------------------------------------------------------------- file manager -

// fm.js:80.
std::string normalizeFilePath(std::string_view path);
// fm.js:204: normalise each path, drop empties and duplicates, keep order.
std::vector<std::string> uniquePaths(const std::vector<std::string> &paths);
// fm.js:318, :322.
bool isPkgEntry(const FileEntry &entry);
bool isExtractableRarEntry(const FileEntry &entry);
// fm.js:3-4 ACTIVE_/TERMINAL_OPERATION_STATUSES; fm.js:556 cancel rule.
bool isActiveFileOp(FileOpStatus status);
bool isTerminalFileOp(FileOpStatus status);
bool canCancelFileOp(const FileOperation &op);
FileOpStatus fileOpStatusFromString(std::string_view text);
const char *toString(Collision collision);
Collision collisionFromString(std::string_view text);

// ------------------------------------------------------------ URL encoding --

// JS encodeURIComponent (unreserved: A-Z a-z 0-9 - _ . ! ~ * ' ( )).
std::string encodeURIComponent(std::string_view text);
// URLSearchParams value encoding (application/x-www-form-urlencoded): space => '+',
// unreserved: A-Z a-z 0-9 * - . _ . Used by the upload query (fm.js:1377).
std::string formUrlEncode(std::string_view text);

} // namespace wings::logic
