// Wings TV app - The real backend: Pegasus DL through the wings/core adapter.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Implements the app's Backend on top of wings::IPegasus. It connects lazily
// (GET /api/health, version check, capability probe) and again whenever the
// connection was lost, so a Pegasus that starts late, or restarts, is picked up
// by the Store's ordinary polling. Wings only ever waits for Pegasus; nothing
// here starts it.
//
// The catalogue (GET /api/library, about 7 MB) is read array-wise through
// IPegasus::streamLibrary, so the app never holds a DOM of it. What the grid
// needs goes into the Package list that library() returns; the download links
// stay here, keyed by package id, and package() builds a PackageDetail from them
// with the link rules ported from Pegasus' web UI (wings::logic::decideLinkAction).
//
// Thread safety: calls may come from the Store worker, its catalogue worker and
// the art worker at once. Shared state is guarded by one mutex and is only
// swapped or copied under it; network calls never hold it.

#pragma once

#include "orbit/backend.hpp"
#include "orbit/thread.hpp"
#include "wings/pegasus.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace orbit
{

class PegasusBackend final : public Backend
{
  public:
    PegasusBackend(std::shared_ptr<wings::http::Transport> transport, std::string endpoint);

    std::string endpoint() const override
    {
        return endpoint_;
    }
    Result<PegasusInfo> health() override;
    Result<std::vector<Job>> queue() override;
    Result<std::vector<Source>> sources() override;
    Result<SettingsView> settings() override;
    Result<std::vector<InstalledGame>> installed() override;
    Result<std::vector<LogLine>> logs(std::int64_t since) override;
    Result<std::vector<Package>> library() override;
    Result<PackageDetail> package(std::string_view id) override;
    Result<std::vector<std::uint8_t>> package_tiers() override;

    Result<std::vector<Job>> job_action(std::string_view job_id, std::string_view action) override;
    Result<std::vector<Job>> clear_finished() override;
    Result<std::vector<Job>> archive_password(std::string_view job_id,
                                              std::string_view password) override;
    Result<std::vector<Job>> archive_choice(std::string_view job_id, int index) override;

    Result<LinkResult> start_link(std::string_view package_id, int link_index,
                                  std::string_view service) override;
    Result<LinkResult> debrid_select(std::string_view selection_id, const std::vector<int> &indexes,
                                     std::string_view service) override;
    Result<CaptureView> capture_status(std::string_view session_id) override;
    Result<CaptureView> capture_active() override;
    Result<bool> capture_cancel(std::string_view session_id) override;
    Result<Probe> probe_link(std::string_view url) override;
    Result<Dispatch> queue_direct(const Probe &probe, std::string_view file_name) override;

    Result<std::vector<Source>> source_add_url(std::string_view url) override;
    Result<std::vector<Source>> source_delete(std::string_view source_id) override;
    Result<std::vector<Source>> source_refresh(std::string_view source_id) override;
    Result<std::vector<Source>> source_enable(std::string_view source_id, bool enabled) override;
    Result<DefaultSources> default_sources(bool force_refresh) override;
    Result<std::vector<std::string>> default_sources_apply(
        std::int64_t revision, const std::vector<std::string> &ids) override;
    Result<bool> default_sources_seen() override;

    Result<Art> art(std::string_view key, ArtKind kind) override;

    Result<SettingsView> settings_update(const SettingsPatch &patch) override;
    Result<SettingsView> debrid_refresh(std::string_view service) override;
    Result<std::vector<StorageMount>> mounts() override;
    Result<FolderListing> browse(std::string_view path) override;
    Result<bool> mkdir(std::string_view full_path) override;
    Result<bool> logs_clear() override;
    Result<std::vector<AprVersionItem>> apr_versions() override;
    Result<AdminJob> apr_update_start(std::string_view title_id,
                                      std::string_view version_id) override;
    Result<AdminJob> apr_update_status(std::string_view job_id, std::string_view title_id) override;
    Result<AdminJob> game_delete(std::string_view title_id) override;
    Result<AdminJob> game_delete_status(std::string_view job_id,
                                        std::string_view title_id) override;
    Result<std::vector<StorageMount>> file_locations() override;
    Result<FolderListing> file_list(std::string_view path) override;
    Result<std::vector<FileOp>> file_operations() override;
    Result<FileItem> file_mkdir(std::string_view parent, std::string_view name) override;
    Result<FileItem> file_rename(std::string_view path, std::string_view name) override;
    Result<std::vector<FileOp>> file_delete(const std::vector<std::string> &paths) override;
    Result<std::vector<FileOp>> file_copy(const std::vector<std::string> &paths,
                                          std::string_view destination,
                                          std::string_view collision) override;
    Result<std::vector<FileOp>> file_move(const std::vector<std::string> &paths,
                                          std::string_view destination,
                                          std::string_view collision) override;
    Result<std::vector<FileOp>> file_extract(const std::vector<std::string> &paths,
                                             std::string_view destination,
                                             std::string_view collision) override;
    Result<std::vector<FileOp>> file_extract_password(std::string_view operation_id,
                                                      std::string_view password) override;
    Result<std::vector<FileOp>> file_pkg_install(std::string_view path) override;
    Result<bool> file_cancel(std::string_view operation_id) override;

    // ---- for tests and the About screen ----
    // Forgets the connection: the next call connects (and probes) again.
    void disconnect();
    // How many packages and links the catalogue cache holds, and an estimate of
    // the bytes they take (strings and containers).
    struct CacheStats
    {
        std::size_t packages = 0;
        std::size_t links = 0;
        std::size_t bytes = 0;
    };
    CacheStats cache_stats() const;

  private:
    using Api = std::shared_ptr<wings::IPegasus>;

    // What library() read: the stripped packages, in the order Pegasus sent them.
    struct Catalogue
    {
        std::vector<wings::Package> packages;
        std::unordered_map<std::int64_t, std::size_t> index;
    };
    // What a capture session needs to be described later.
    struct SessionInfo
    {
        bool automated = false;
        bool requires_browser = true;
        std::string title;
        std::string host;
    };

    // The connection, made on demand. Null with `error` filled when Pegasus is
    // not there or not supported.
    Api api(Result<bool> *error);
    template <typename T> Api need_api(Result<T> *out);
    template <typename T> void fail(Result<T> *out, const wings::ApiError &error);
    Result<std::vector<Job>> jobs_after(const wings::Result<wings::QueueResponse> &reply,
                                        wings::IPegasus &api);
    wings::Settings current_settings() const;
    std::shared_ptr<const Catalogue> catalogue() const;
    CaptureView capture_from(const wings::CaptureStatus &status, const SessionInfo &info) const;
    void remember_session(const std::string &id, const SessionInfo &info);
    SessionInfo session_info(const std::string &id) const;

    std::shared_ptr<wings::http::Transport> transport_;
    std::string endpoint_;

    mutable Mutex mutex_;
    Api api_;
    PegasusInfo info_;
    wings::Settings settings_;
    std::shared_ptr<const Catalogue> catalogue_;
    std::map<std::string, SessionInfo> sessions_;
};

// ---- helpers shared with the tests ----

// A package description as plain text: tags removed, common entities decoded,
// whitespace collapsed, shortened (at a character boundary) to max_bytes.
std::string plain_description(std::string_view html, std::size_t max_bytes);

} // namespace orbit
