// Wings core - Stable internal model. The UI only ever sees these types.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plain structs, no behaviour. Defaults describe "field missing from the
// response": the adapter's tolerant parsing leaves a default in place when
// Pegasus omits a field, and ignores fields it does not know.
//
// Where the Pegasus web UI treats an absent flag as true (`x !== false`),
// the default here is true as well; this is noted per field.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wings
{

// ---------------------------------------------------------------- errors ----

struct ApiError
{
    enum class Kind : std::uint8_t
    {
        none,
        transport, // request never completed (status == 0)
        http,      // non-2xx, message from {"error": ".."} when present
        parse,     // 2xx but the body was not the expected JSON
        client,    // rejected locally before sending (same checks the web UI does)
    };
    Kind kind = Kind::none;
    int status = 0; // HTTP status; 0 for transport/client errors
    std::string message;

    bool ok() const
    {
        return kind == Kind::none;
    }
    // Pegasus' "route does not exist" signature: 404 {"error":"Route not found"}.
    // Other 404s (unknown job, missing path) mean the route exists.
    bool route_missing() const
    {
        return kind == Kind::http && status == 404 && message == "Route not found";
    }
};

// -------------------------------------------------------- health / config ---

struct Health
{
    std::string status; // "running"
    int port = 0;
    bool downloadOnly = false;
    bool nativePkgHandoff = false;
    std::string version; // "1.11.0"
};

struct DnsHelper
{
    bool present = false; // false when the response had no dnsHelper object
    bool available = false;
    bool nanodnsRunning = false;
    bool managed = false;
    bool autostartAttempted = false;
    std::string startError;
    std::string recommendedPrimaryDns;
};

// realDebrid / torBox blocks of GET /api/settings, and the reply of
// POST /api/debrid/refresh. Tokens are never returned by Pegasus.
struct DebridStatus
{
    bool present = false; // false when the response had no such object
    bool configured = false;
    bool valid = false;
    bool premium = false;
    std::string accountType;
    std::int64_t hosterCount = 0;
    std::int64_t domainCount = 0;
    std::int64_t lastCheckedAt = 0;
    std::string error;
    std::vector<std::string> domains;
};

struct Settings
{
    std::string downloadRoot;
    int downloadConnections = 16; // JS: Number(downloadConnections || concurrency || 16)
    int concurrency = 0;
    bool downloadOnly = false;
    bool nativePkgHandoff = false;
    bool queuePersists = false;
    bool rangeResume = false;
    DnsHelper dnsHelper;
    DebridStatus realDebrid;
    DebridStatus torBox;
};

// Fields for POST /api/settings. Only set fields are sent.
struct SettingsUpdate
{
    bool hasDownloadRoot = false;
    std::string downloadRoot;
    bool hasDownloadConnections = false;
    int downloadConnections = 0;
    bool hasRealDebridToken = false;
    std::string realDebridToken; // "" clears
    bool hasTorBoxToken = false;
    std::string torBoxToken; // "" clears
};

enum class DebridService : std::uint8_t
{
    realDebrid,
    torBox,
};

// ------------------------------------------------------------- catalogue ----

struct DownloadLink
{
    // Only meaningful when hasIndex (response had an integer "index"); otherwise
    // logic::packageLinks() falls back to the position, like the web UI.
    bool hasIndex = false;
    std::int64_t index = 0;
    std::string name;
    std::string url; // as served; may be base64 (see logic::downloadLinkUrl)
};

struct Package
{
    std::int64_t id = 0;
    std::int64_t sourceId = 0;
    std::string sourceName;
    std::string titleId;
    std::string title;
    std::string version;
    std::string posterUrl;
    std::string downloadSource;
    std::string description; // may contain HTML; sanitise before display
    std::int64_t downloadLinkCount = 0;
    std::int64_t sizeBytes = 0;
    std::vector<DownloadLink> downloadLinks;
};

struct Source
{
    std::int64_t id = 0;
    std::string name;
    std::string kind; // "url" | "file" | ...
    std::string url;
    bool enabled = true; // JS: `source.enabled !== false`
    bool managed = false;
    bool deletable = true;
    bool isNew = false;
    bool hasCatalog = false;
    std::string managedId;
    std::string description;
    std::int64_t lastRefreshAt = 0;
    std::int64_t lastRefreshAttemptAt = 0;
    std::int64_t packageCount = 0;
    std::int64_t skippedCount = 0;
    std::string status;
};

struct DefaultSource
{
    std::string id;
    std::string name;
    std::string description;
    std::string url;
    bool defaultEnabled = false;
    bool configured = false;
    std::int64_t sourceId = 0;
    bool enabled = false;
    bool isNew = false;
    bool hasCatalog = false;
    std::int64_t packageCount = 0;
};

struct DefaultSources
{
    std::string status; // "remote" | "unavailable" | ...
    bool available = false;
    std::int64_t schemaVersion = 0;
    std::int64_t revision = 0;
    std::int64_t onboardingRevision = 0;
    std::int64_t acceptedOnboardingRevision = 0;
    bool onboardingRequired = false;
    bool hasNew = false;
    std::string error;
    std::vector<DefaultSource> sources;
};

struct DefaultSourcesApplyResult
{
    std::vector<std::string> failures; // one human-readable line per failed catalog
    bool hasSources = false;
    std::vector<Source> sources;
};

// ----------------------------------------------------------------- queue ----

enum class JobStatus : std::uint8_t
{
    unknown,
    queued,
    starting,
    downloading,
    merging,
    extracting,
    finalizing,
    waitingPassword, // "waiting_password"
    waitingChoice,   // "waiting_choice"
    pausing,
    cancelling,
    paused,
    completed,
    failed,
    cancelled,
};

enum class JobPhase : std::uint8_t
{
    none, // "" (not started)
    download,
    merge,
    extract,
    finish,
    unknown,
};

struct ArchiveCandidate
{
    std::int64_t index = 0;
    std::string name;
};

struct QueueJob
{
    std::int64_t id = 0;
    std::int64_t packageId = 0;
    std::int64_t downloadLinkIndex = 0;
    std::string downloadLinkName;
    std::string title;
    std::string titleId;
    std::string version;
    std::string sourceName;
    std::string fileName;
    JobStatus status = JobStatus::unknown;
    std::string statusRaw;
    std::string detail;
    std::string error;
    std::string destinationPath;
    std::string contentKind;   // "file" | "game" | ...
    std::string archiveFormat; // "none" | "rar" | ...
    std::string archiveMode;   // "stream" => streamed extraction (JS 1797)
    bool streamMode = false;
    bool hasStreamMode = false; // JS 1795-1796: explicit true/false wins over archiveMode
    std::int64_t extractedBytesDone = 0;
    std::int64_t extractedTotalBytes = 0;
    std::int64_t currentArchiveEntryBytesDone = 0;
    std::string currentArchiveEntry;
    std::int64_t currentArchiveEntrySize = 0;
    bool archivePasswordRequired = false;
    std::int64_t sizeBytes = 0;
    std::int64_t bytesDone = 0;
    std::int64_t speedBps = 0;
    std::int64_t etaSeconds = 0;
    double progress = 0.0; // 0..1
    JobPhase phase = JobPhase::none;
    std::string phaseRaw;
    double phaseProgress = 0.0;
    std::int64_t phaseBytesDone = 0;
    std::int64_t phaseTotalBytes = 0;
    std::int64_t phaseSpeedBps = 0;
    std::int64_t attempts = 0;
    std::int64_t maxAttempts = 0;
    bool supportsResume = false;
    bool resumed = false;
    std::int64_t createdAt = 0;
    std::int64_t startedAt = 0;
    std::int64_t updatedAt = 0;
    std::int64_t finishedAt = 0;
    std::vector<ArchiveCandidate> archiveCandidates; // when status == waitingChoice
};

struct QueueSummary
{
    std::int64_t total = 0;
    std::int64_t queued = 0;
    std::int64_t starting = 0;
    std::int64_t downloading = 0;
    std::int64_t merging = 0;
    std::int64_t extracting = 0;
    std::int64_t finalizing = 0;
    std::int64_t waitingPassword = 0;
    std::int64_t waitingChoice = 0;
    std::int64_t pausing = 0;
    std::int64_t cancelling = 0;
    std::int64_t paused = 0;
    std::int64_t completed = 0;
    std::int64_t failed = 0;
    std::int64_t cancelled = 0;
    std::int64_t etaSeconds = 0;
};

struct QueueSnapshot
{
    QueueSummary summary;
    std::vector<QueueJob> jobs;
};

enum class DispatchKind : std::uint8_t
{
    other,
    nativePkg, // "native-pkg": handed to PS5 Downloads
    queued,    // "queued": added to Pegasus' own queue
};

// One element of `dispatchResult` / `dispatchResults` (JS 3429-3445).
struct DispatchResult
{
    bool ok = false;
    DispatchKind kind = DispatchKind::other;
    std::string kindRaw;
    bool accepted = false;
    std::string title;
    std::string message;
    std::string contentId;
    std::int64_t jobId = 0;  // kind == queued
    std::string fileName;    // kind == queued
    std::string error;
};

// Reply of every route that can start or change downloads: POST /api/queue,
// /queue/direct, /queue/{pause|resume|...}, /debrid/queue, capture status.
struct QueueResponse
{
    bool hasQueue = false; // true when the reply carried a `jobs` array (JS 3503)
    QueueSnapshot queue;
    std::vector<DispatchResult> dispatchResults; // dispatchResult first, then dispatchResults[]
};
using DispatchResponse = QueueResponse;

struct ResolveProbe
{
    std::string requestedUrl;
    std::string finalUrl;
    std::int64_t status = 0;
    std::string contentType;
    std::string contentDisposition;
    std::string fileName;
    bool hasContentLength = false;
    std::int64_t contentLength = 0;
    std::int64_t bodyLength = 0;
    std::string htmlTitle;
    bool isHtml = false;
    bool isDownload = false;
    std::int64_t redirectCount = 0;
};

struct DebridOption
{
    std::int64_t index = 0;
    std::string filename;
    std::int64_t filesize = 0;
    std::string host;
};

// Reply of POST /api/debrid/queue. Either the link was dispatched (response
// carries the usual queue/dispatch data) or the service needs the user to
// pick files (needsSelection + selectionId + options).
struct DebridQueueResult
{
    bool needsSelection = false;
    std::string selectionId;
    std::vector<DebridOption> options;
    QueueResponse response;
};

// -------------------------------------------------------- browser capture ---

enum class CaptureState : std::uint8_t
{
    unknown,
    idle,
    priming,
    watching,
    queueing,
    completed,
    failed,
    cancelled,
};

struct CaptureStatus
{
    bool active = false; // only meaningful for /capture/active
    std::int64_t sessionId = 0;
    CaptureState state = CaptureState::unknown;
    std::string statusRaw;
    std::string message;
    std::int64_t packageId = 0;
    std::int64_t linkIndex = 0;
    std::string startUrl;
    bool authCaptured = false;
    std::string authHost;
    std::int64_t pendingCandidates = 0;
    std::int64_t droppedCandidates = 0;
    QueueResponse dispatch; // present on `completed` (JS 4040-4042)
};

// Reply of POST /api/browser/open and /api/provider/resolve.
struct CaptureStart
{
    std::int64_t sessionId = 0;
};

// ------------------------------------------------- storage / file manager ---

// GET /api/mounts entries and GET /api/files/locations entries.
struct Mount
{
    std::string path;
    std::string name; // locations only
    std::string source;
    std::string type; // filesystem type
    bool writable = true; // JS: `writable !== false`
    std::int64_t totalBytes = 0;
    std::int64_t freeBytes = 0;
    std::int64_t usedBytes = 0;
};
using Location = Mount;

struct FileEntry
{
    std::string name;
    std::string path; // empty in /api/browse results
    std::string type; // "dir" | "file" | "symlink" | other
    bool directory = false;
    bool file = false;
    bool symlink = false;
    std::int64_t mode = 0;
    std::int64_t modifiedTime = 0;
    std::int64_t sizeBytes = 0;
    bool writable = true;
    // Present on mkdir/rename replies (volume stats of the containing mount).
    std::int64_t totalBytes = 0;
    std::int64_t freeBytes = 0;
    std::int64_t usedBytes = 0;
};

struct BrowseResult // GET /api/browse
{
    std::string path;
    std::vector<FileEntry> entries;
};

struct FileList // GET /api/files/list
{
    std::string path;
    bool writable = true; // JS: `data.writable !== false`
    std::vector<FileEntry> entries;
};

enum class FileOpStatus : std::uint8_t
{
    unknown,
    queued,
    starting,
    running,
    copying,
    installing,
    waitingPassword,
    accepted,
    completed,
    failed,
    cancelled,
};

struct FileOperation
{
    std::int64_t operationId = 0;
    std::string type; // copy | move | delete | extract | pkg-install ...
    FileOpStatus status = FileOpStatus::unknown;
    std::string statusRaw;
    std::string collision;
    std::string source;
    std::string destination;
    std::string currentPath;
    std::string error;
    std::int64_t totalItems = 0;
    std::int64_t doneItems = 0;
    std::int64_t totalBytes = 0;
    std::int64_t doneBytes = 0;
    std::int64_t startedAt = 0;
    std::int64_t updatedAt = 0;
    bool cancelRequested = false;
    bool cancelable = true; // JS: `op.cancelable !== false`
};

enum class Collision : std::uint8_t
{
    keepBoth,
    replace,
    skip,
};

// ----------------------------------------------------------------- logs -----

struct LogEntry
{
    std::int64_t seq = 0;
    std::int64_t ts = 0;
    std::string level;
    std::string component;
    std::string message;
};

struct LogBatch
{
    std::vector<LogEntry> logs;
    std::int64_t lastSeq = 0;
    std::int64_t count = 0;
    std::int64_t capacity = 0;
};

// --------------------------------------------------- installed games / APR --

struct AprEmu
{
    bool present = false;
    std::string version;
    std::string build;
    std::string sha256;
    bool runtimeOverride = false;
    std::string latestVersion;
    std::string latestVersionId;
    std::string latestBuild;
    std::string currentVersionId;
    bool updateAvailable = false;
    bool canUpdate = false;
    bool canReinstall = false;
    bool remoteAvailable = true; // JS 855 only treats an explicit `false` as unavailable
    std::string reason;
    std::string manifestError;
    bool remountRequired = false;
    bool remountAttempted = false;
    bool remounted = false;
    std::string remountReason;
};

struct InstalledGame
{
    std::string titleId;
    std::string titleName;
    std::string title;
    std::int64_t sortPriority = 0;
    std::string iconUrl;
    std::string pic0Url;
    std::string installPath;
    std::int64_t installedTime = 0;
    std::string contentId;
    std::string contentVersion;
    std::string version;
    std::string masterVersion;
    std::string originContentVersion;
    std::string sdkVersion;                  // hex string, e.g. "0x0900000000000000"
    std::string requiredSystemSoftwareVersion; // hex string
    std::string metadataPath;
    std::int64_t modifiedTime = 0;
    bool hasIcon = false;
    bool hasPic0 = false;
    // Image-backed titles only (ShadowMount etc).
    std::string storageKind; // "folder" | "image" | ""
    std::string format;
    std::string backingPath;
    std::string mountLinkPath;
    std::int64_t sizeBytes = 0;
    AprEmu aprEmu;
};

struct InstalledGames
{
    bool databaseAvailable = true; // JS 4339: only an explicit false is an error
    std::string databaseError;
    std::string error;
    std::int64_t count = 0;
    std::int64_t elapsedMs = 0;
    std::vector<InstalledGame> titles;
};

struct AprVersion
{
    std::string id;
    std::string version;
    std::string build; // "release" | "debug"
    std::int64_t size = 0;
};

struct AprVersions
{
    std::string latestReleaseId;
    std::vector<AprVersion> versions;
};

// Reference to a background job: poll by jobId when known, else by titleId
// (JS 4454-4456, 4509-4511, 4646-4648).
struct JobRef
{
    std::string jobId;
    std::string titleId;
};

// Reply of apr-emu/update/start, update/status, restore/status.
struct AprJob
{
    std::string jobId; // number or string on the wire; normalised to text
    std::string titleId;
    std::string status; // JS default 'starting'; terminal: completed | failed
    int progress = 0;   // clamped 0..100 (JS 4368-4380)
    std::string detail;
    std::string error;
    std::string operation;
    bool hasAprEmu = false;
    AprEmu aprEmu;
};

// Reply of installed-games/delete and delete/status.
struct DeleteJob
{
    std::string jobId;
    std::string titleId;
    std::string status;
    std::int64_t filesDeleted = 0;
    std::string detail;
    std::string error;
};

// ----------------------------------------------------- capabilities / misc --

// Per-feature availability, filled by Pegasus::connect() from cheap GET probes.
// false = disabled (route missing, errored, or version unsupported).
struct Capabilities
{
    bool health = false;
    bool settings = false;
    bool sources = false;
    bool defaultSources = false;
    bool library = false;       // inferred from /api/sources (the 7 MB route is not probed)
    bool queue = false;
    bool debrid = false;        // settings carry realDebrid/torBox blocks
    bool debridQueue = false;   // inferred from debrid + queue
    bool providerResolve = false;
    bool browserCapture = false;
    bool mounts = false;
    bool browse = false;
    bool logs = false;
    bool appdb = false;         // installed games
    bool aprEmu = false;        // APR Emu update/restore (route family present)
    bool aprEmuVersions = false;// versions list usable (503 manifest error => false)
    bool installedGameDelete = false;
    bool fileManager = false;
    bool fileOperations = false;
    bool dnsHelper = false;
    // Human-readable notes for anything degraded or disabled, "feature: reason".
    std::vector<std::string> notes;
};

} // namespace wings
