// Wings core - The Pegasus adapter interface, capability probe and version selection.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI talks to IPegasus and the model types only. Pegasus' HTTP routes and
// JSON shapes are known to exactly one translation unit per adapter version
// (pegasus_v1.cpp for Pegasus 1.x). Pegasus::connect() reads /api/health, picks
// the adapter and probes cheap GET routes to build a Capabilities struct; a
// missing or broken feature disables that feature and never throws.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wings/http.hpp"
#include "wings/model.hpp"

namespace wings
{

struct Empty
{
};

template <class T> struct Result
{
    ApiError error;
    T value{};

    bool ok() const
    {
        return error.ok();
    }
    explicit operator bool() const
    {
        return error.ok();
    }
};

// Receives catalogue packages one at a time (IPegasus::streamLibrary).
class PackageSink
{
  public:
    virtual ~PackageSink() = default;
    virtual void package(Package &&package) = 0;
};

// One method per Pegasus route or UI flow (see parity.csv). Every method is
// synchronous and may block on the transport: call from a worker thread.
class IPegasus
{
  public:
    virtual ~IPegasus() = default;
    virtual const char *adapterName() const = 0;

    // ---- health / settings
    virtual Result<Health> health() = 0;                                            // GET /api/health
    virtual Result<Settings> getSettings() = 0;                                     // GET /api/settings
    virtual Result<Settings> updateSettings(const SettingsUpdate &update) = 0;      // POST /api/settings
    virtual Result<DebridStatus> refreshDebrid(DebridService service) = 0;          // POST /api/debrid/refresh
    Result<Settings> setDownloadRoot(const std::string &path);
    Result<Settings> setDownloadConnections(int value);
    Result<Settings> setDebridToken(DebridService service, const std::string &token); // "" clears
    Result<Settings> clearDebridToken(DebridService service);

    // ---- sources
    virtual Result<std::vector<Source>> listSources() = 0;                          // GET /api/sources
    virtual Result<std::vector<Source>> addSourceUrl(const std::string &url) = 0;   // POST /api/sources/url {url}
    virtual Result<std::vector<Source>> uploadSourceCatalog(const std::string &catalogJson) = 0; // POST /api/sources/upload (raw)
    virtual Result<std::vector<Source>> deleteSource(std::int64_t sourceId) = 0;    // POST /api/sources/delete
    virtual Result<std::vector<Source>> refreshSource(std::int64_t sourceId) = 0;   // POST /api/sources/refresh
    virtual Result<std::vector<Source>> setSourceEnabled(std::int64_t sourceId, bool enabled) = 0; // POST /api/sources/enabled

    // ---- default (included) sources
    virtual Result<DefaultSources> getDefaultSources(bool forceRefresh) = 0;        // GET /api/default-sources[?refresh=1]
    virtual Result<DefaultSourcesApplyResult> applyDefaultSources(std::int64_t revision, const std::vector<std::string> &selectedIds) = 0;
    virtual Result<Empty> markDefaultSourcesSeen() = 0;                             // POST /api/default-sources/seen

    // ---- library (package catalogue)
    virtual Result<std::vector<Package>> getLibrary() = 0;                          // GET /api/library
    // Same route, but the 7 MB reply is read array-wise: each package is parsed on its own
    // and handed to the sink, so no DOM of the whole catalogue (about 36 MB) is ever built.
    // On error some packages may already have been delivered: discard them. The default
    // implementation goes through getLibrary().
    virtual Result<Empty> streamLibrary(PackageSink &sink);

    // ---- queue
    virtual Result<QueueSnapshot> getQueue() = 0;                                   // GET /api/queue
    virtual Result<DispatchResponse> enqueuePackage(std::int64_t packageId, std::int64_t linkIndex) = 0; // POST /api/queue (native .pkg handoff)
    virtual Result<DispatchResponse> queueDirect(const std::string &url, const std::string &fileName, const std::string &contentType) = 0; // POST /api/queue/direct
    virtual Result<QueueResponse> pauseJob(std::int64_t jobId) = 0;                 // POST /api/queue/pause
    virtual Result<QueueResponse> resumeJob(std::int64_t jobId) = 0;                // POST /api/queue/resume
    virtual Result<QueueResponse> cancelJob(std::int64_t jobId) = 0;                // POST /api/queue/cancel
    virtual Result<QueueResponse> retryJob(std::int64_t jobId) = 0;                 // POST /api/queue/retry
    virtual Result<QueueResponse> clearFinished() = 0;                              // POST /api/queue/clear-finished
    virtual Result<QueueResponse> submitArchivePassword(std::int64_t jobId, const std::string &password) = 0; // POST /api/queue/archive-password
    virtual Result<QueueResponse> submitArchiveChoice(std::int64_t jobId, std::int64_t candidateIndex) = 0;   // POST /api/queue/archive-choice

    // ---- link flows
    virtual Result<ResolveProbe> resolveProbe(const std::string &url) = 0;          // POST /api/resolve/probe
    virtual Result<DebridQueueResult> debridQueue(std::int64_t packageId, std::int64_t linkIndex, DebridService service) = 0; // POST /api/debrid/queue
    virtual Result<DebridQueueResult> debridQueueSelection(const std::string &selectionId, const std::vector<std::int64_t> &indexes, DebridService service) = 0;
    virtual Result<CaptureStart> providerResolve(std::int64_t packageId, std::int64_t linkIndex) = 0; // POST /api/provider/resolve
    // packageId/linkIndex are omitted from the body when not set (the "open unsupported link" flow, app.js:4929).
    virtual Result<CaptureStart> browserOpen(const std::string &url, bool guarded, std::optional<std::int64_t> packageId, std::optional<std::int64_t> linkIndex) = 0; // POST /api/browser/open
    virtual Result<CaptureStatus> captureStatus(std::int64_t sessionId) = 0;        // GET /api/browser/capture/status
    virtual Result<CaptureStatus> captureActive() = 0;                              // GET /api/browser/capture/active
    virtual Result<Empty> captureCancel(std::int64_t sessionId) = 0;                // POST /api/browser/capture/cancel

    // ---- storage browser (download folder picker)
    virtual Result<std::vector<Mount>> getMounts() = 0;                             // GET /api/mounts
    virtual Result<BrowseResult> browse(const std::string &path) = 0;               // GET /api/browse?path=
    virtual Result<Empty> mkdir(const std::string &fullPath) = 0;                   // POST /api/mkdir {path}

    // ---- logs
    virtual Result<LogBatch> getLogs(std::int64_t sinceSeq) = 0;                    // GET /api/logs?since=
    virtual Result<Empty> clearLogs() = 0;                                          // POST /api/logs/clear
    virtual Result<Empty> clientLog(const std::string &level, const std::string &message, const std::string &detail, const std::string &source = "frontend") = 0; // POST /api/client-log

    // ---- installed games and APR Emu
    virtual Result<InstalledGames> getInstalledGames() = 0;                         // GET /appdb
    virtual Result<std::string> fetchAppdbArt(const std::string &urlOrPath) = 0;    // GET /appdb/icon|pic0?id= (bytes)
    virtual Result<AprVersions> getAprVersions() = 0;                               // GET /api/apr-emu/versions
    virtual Result<AprJob> aprUpdateStart(const std::string &titleId, const std::string &versionId) = 0; // POST /api/apr-emu/update/start
    virtual Result<AprJob> aprUpdateStatus(const JobRef &ref) = 0;                  // GET /api/apr-emu/update/status
    virtual Result<AprJob> aprRestoreStatus(const JobRef &ref) = 0;                 // GET /api/apr-emu/restore/status
    virtual Result<DeleteJob> installedGameDelete(const std::string &titleId) = 0;  // POST /api/installed-games/delete  (DESTRUCTIVE)
    virtual Result<DeleteJob> installedGameDeleteStatus(const JobRef &ref) = 0;     // GET /api/installed-games/delete/status

    // ---- file manager
    virtual Result<std::vector<Location>> fileLocations() = 0;                      // GET /api/files/locations
    virtual Result<FileList> fileList(const std::string &path) = 0;                 // GET /api/files/list?path=
    virtual Result<std::vector<FileOperation>> fileOperations() = 0;                // GET /api/files/operations
    virtual Result<FileEntry> fileMkdir(const std::string &parent, const std::string &name) = 0;   // POST /api/files/mkdir
    virtual Result<FileEntry> fileRename(const std::string &path, const std::string &name) = 0;    // POST /api/files/rename
    virtual Result<std::vector<FileOperation>> fileDelete(const std::vector<std::string> &paths) = 0; // POST /api/files/delete (DESTRUCTIVE)
    virtual Result<std::vector<FileOperation>> fileCopy(const std::vector<std::string> &paths, const std::string &destination, Collision collision) = 0;    // POST /api/files/copy
    virtual Result<std::vector<FileOperation>> fileMove(const std::vector<std::string> &paths, const std::string &destination, Collision collision) = 0;    // POST /api/files/move
    virtual Result<std::vector<FileOperation>> fileExtract(const std::vector<std::string> &paths, const std::string &destination, Collision collision) = 0; // POST /api/files/extract
    virtual Result<std::vector<FileOperation>> fileExtractPassword(std::int64_t operationId, const std::string &password) = 0; // POST /api/files/extract/password
    virtual Result<std::vector<FileOperation>> filePkgInstall(const std::string &path) = 0;       // POST /api/files/pkg/install
    virtual Result<Empty> fileOperationCancel(std::int64_t operationId) = 0;        // POST /api/files/operation/cancel
    // PUT /api/files/upload?directory=&name=[&relativePath=] with the raw bytes as body.
    virtual Result<Empty> fileUpload(const std::string &directory, const std::string &name, const std::string &relativePath, const std::string &data) = 0;
    // GET /api/files/download?path= (file) and /api/files/archive?path= (folder, zip) as bytes, capped at maxBytes.
    virtual Result<std::string> fileDownload(const std::string &path, std::size_t maxBytes) = 0;
    virtual Result<std::string> fileArchive(const std::string &path, std::size_t maxBytes) = 0;
    // URL builders for clients that stream the download themselves (fm.js:1363-1365).
    virtual std::string fileDownloadPath(const std::string &path, bool isDirectory) const = 0;
};

// ---------------------------------------------------------------- connect ---

enum class Compat : std::uint8_t
{
    tested,        // an adapter exists for exactly this minor version (1.11.x)
    untestedMinor, // same major, different minor: adapter used, features probed
    unsupported,   // no adapter for this major version: no api
    unreachable,   // /api/health failed: no api
};

struct ConnectOptions
{
    bool probeAppDb = true; // GET /appdb can take ~1s on a full console
};

struct Connection
{
    std::unique_ptr<IPegasus> api; // null when !usable()
    Health health;
    Capabilities caps;
    Compat compat = Compat::unreachable;
    std::string adapter; // "pegasus-v1" or ""
    std::string message; // human-readable status for the About screen
    ApiError error;      // set when unreachable

    bool usable() const
    {
        return api != nullptr;
    }
};

// Parses "1.11.0" => {1, 11, 0}; false for anything else.
bool parseVersion(const std::string &text, int *major, int *minor, int *patch);

// Reads /api/health, selects the adapter, probes capabilities. Never throws.
Connection connect(std::shared_ptr<http::Transport> transport, const ConnectOptions &options = {});

// Exposed for tests and the About screen: probe an existing adapter.
Capabilities probeCapabilities(IPegasus &api, const Health &health, const ConnectOptions &options = {});

} // namespace wings
