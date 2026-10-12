// Wings core - Adapter for Pegasus DL 1.x (tested against 1.11.0).
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "wings/pegasus.hpp"

namespace wings
{

class PegasusV1 final : public IPegasus
{
  public:
    explicit PegasusV1(std::shared_ptr<http::Transport> transport);

    const char *adapterName() const override
    {
        return "pegasus-v1";
    }

    Result<Health> health() override;
    Result<Settings> getSettings() override;
    Result<Settings> updateSettings(const SettingsUpdate &update) override;
    Result<DebridStatus> refreshDebrid(DebridService service) override;

    Result<std::vector<Source>> listSources() override;
    Result<std::vector<Source>> addSourceUrl(const std::string &url) override;
    Result<std::vector<Source>> uploadSourceCatalog(const std::string &catalogJson) override;
    Result<std::vector<Source>> deleteSource(std::int64_t sourceId) override;
    Result<std::vector<Source>> refreshSource(std::int64_t sourceId) override;
    Result<std::vector<Source>> setSourceEnabled(std::int64_t sourceId, bool enabled) override;

    Result<DefaultSources> getDefaultSources(bool forceRefresh) override;
    Result<DefaultSourcesApplyResult> applyDefaultSources(std::int64_t revision, const std::vector<std::string> &selectedIds) override;
    Result<Empty> markDefaultSourcesSeen() override;

    Result<std::vector<Package>> getLibrary() override;
    Result<Empty> streamLibrary(PackageSink &sink) override;

    Result<QueueSnapshot> getQueue() override;
    Result<DispatchResponse> enqueuePackage(std::int64_t packageId, std::int64_t linkIndex) override;
    Result<DispatchResponse> queueDirect(const std::string &url, const std::string &fileName, const std::string &contentType) override;
    Result<QueueResponse> pauseJob(std::int64_t jobId) override;
    Result<QueueResponse> resumeJob(std::int64_t jobId) override;
    Result<QueueResponse> cancelJob(std::int64_t jobId) override;
    Result<QueueResponse> retryJob(std::int64_t jobId) override;
    Result<QueueResponse> clearFinished() override;
    Result<QueueResponse> submitArchivePassword(std::int64_t jobId, const std::string &password) override;
    Result<QueueResponse> submitArchiveChoice(std::int64_t jobId, std::int64_t candidateIndex) override;

    Result<ResolveProbe> resolveProbe(const std::string &url) override;
    Result<DebridQueueResult> debridQueue(std::int64_t packageId, std::int64_t linkIndex, DebridService service) override;
    Result<DebridQueueResult> debridQueueSelection(const std::string &selectionId, const std::vector<std::int64_t> &indexes, DebridService service) override;
    Result<CaptureStart> providerResolve(std::int64_t packageId, std::int64_t linkIndex) override;
    Result<CaptureStart> browserOpen(const std::string &url, bool guarded, std::optional<std::int64_t> packageId, std::optional<std::int64_t> linkIndex) override;
    Result<CaptureStatus> captureStatus(std::int64_t sessionId) override;
    Result<CaptureStatus> captureActive() override;
    Result<Empty> captureCancel(std::int64_t sessionId) override;

    Result<std::vector<Mount>> getMounts() override;
    Result<BrowseResult> browse(const std::string &path) override;
    Result<Empty> mkdir(const std::string &fullPath) override;

    Result<LogBatch> getLogs(std::int64_t sinceSeq) override;
    Result<Empty> clearLogs() override;
    Result<Empty> clientLog(const std::string &level, const std::string &message, const std::string &detail, const std::string &source = "frontend") override;

    Result<InstalledGames> getInstalledGames() override;
    Result<std::string> fetchAppdbArt(const std::string &urlOrPath) override;
    Result<AprVersions> getAprVersions() override;
    Result<AprJob> aprUpdateStart(const std::string &titleId, const std::string &versionId) override;
    Result<AprJob> aprUpdateStatus(const JobRef &ref) override;
    Result<AprJob> aprRestoreStatus(const JobRef &ref) override;
    Result<DeleteJob> installedGameDelete(const std::string &titleId) override;
    Result<DeleteJob> installedGameDeleteStatus(const JobRef &ref) override;

    Result<std::vector<Location>> fileLocations() override;
    Result<FileList> fileList(const std::string &path) override;
    Result<std::vector<FileOperation>> fileOperations() override;
    Result<FileEntry> fileMkdir(const std::string &parent, const std::string &name) override;
    Result<FileEntry> fileRename(const std::string &path, const std::string &name) override;
    Result<std::vector<FileOperation>> fileDelete(const std::vector<std::string> &paths) override;
    Result<std::vector<FileOperation>> fileCopy(const std::vector<std::string> &paths, const std::string &destination, Collision collision) override;
    Result<std::vector<FileOperation>> fileMove(const std::vector<std::string> &paths, const std::string &destination, Collision collision) override;
    Result<std::vector<FileOperation>> fileExtract(const std::vector<std::string> &paths, const std::string &destination, Collision collision) override;
    Result<std::vector<FileOperation>> fileExtractPassword(std::int64_t operationId, const std::string &password) override;
    Result<std::vector<FileOperation>> filePkgInstall(const std::string &path) override;
    Result<Empty> fileOperationCancel(std::int64_t operationId) override;
    Result<Empty> fileUpload(const std::string &directory, const std::string &name, const std::string &relativePath, const std::string &data) override;
    Result<std::string> fileDownload(const std::string &path, std::size_t maxBytes) override;
    Result<std::string> fileArchive(const std::string &path, std::size_t maxBytes) override;
    std::string fileDownloadPath(const std::string &path, bool isDirectory) const override;

  private:
    std::shared_ptr<http::Transport> transport_;
};

} // namespace wings
