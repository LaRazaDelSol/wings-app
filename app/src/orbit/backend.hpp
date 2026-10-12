// Wings TV app - The seam: what the app needs from a download backend.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Store (and ArtCache) talk to a Backend and nothing else. The PC preview and
// the unit tests inject a fake one (host/fake_backend.*); the console build
// uses PegasusBackend (pegasus_backend.*), which sits on the Pegasus adapter
// in wings/core. Every call blocks, so they run on worker threads, and
// implementations must be safe to call from several threads at once (the Store
// worker, its catalogue worker and the art worker).
//
// Ids are text (job ids, package ids, source ids) so the UI never depends on
// how Pegasus numbers things.
//
// Result<T> keeps the shape of Orbit Store's api::Result<T>.

#pragma once

#include "orbit/model.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

template <typename T> struct Result
{
    T value{};
    int status = 0;    // HTTP-like status, 0 when the backend was not reached
    std::string error; // empty on success
    int retry_after = -1;

    bool ok() const
    {
        return error.empty();
    }
    // The backend itself is not answering (not running, port closed).
    bool unreachable() const
    {
        return status == 0 && !error.empty();
    }
};

enum class ArtKind : std::uint8_t
{
    cover,
    hero,     // the full background
    hero_low, // a small, fast stand-in for it
};

struct Art
{
    bool pending = false; // the backend is still fetching it; ask again shortly
    std::string bytes;    // the encoded image (WebP, PNG or JPEG)
    std::string content_type;
};

// Where artwork comes from; a slice of Backend so the cache needs no more.
class ArtSource
{
  public:
    virtual ~ArtSource() = default;
    // key: "pkg:<packageId>" or "app:<titleId>".
    virtual Result<Art> art(std::string_view key, ArtKind kind) = 0;
};

class Backend : public ArtSource
{
  public:
    // The address the UI shows while waiting ("127.0.0.1:6970").
    virtual std::string endpoint() const = 0;

    // ---- reads: what the Store polls ----
    virtual Result<PegasusInfo> health() = 0;
    virtual Result<std::vector<Job>> queue() = 0;
    virtual Result<std::vector<Source>> sources() = 0;
    virtual Result<SettingsView> settings() = 0;
    virtual Result<std::vector<InstalledGame>> installed() = 0;
    // Log lines with seq greater than `since`.
    virtual Result<std::vector<LogLine>> logs(std::int64_t since) = 0;
    // The whole catalogue, trimmed to what the grid needs (large: read rarely, and
    // on its own thread). The links stay inside the backend; see package().
    virtual Result<std::vector<Package>> library() = 0;
    // One package with its links and how each would be downloaded. Answered from
    // what library() read, so it does not touch the network.
    virtual Result<PackageDetail> package(std::string_view id) = 0;
    // For every package of the last library() read, in the same order: its best
    // link tier under the player's current debrid accounts (pack_tier, model.hpp),
    // classified by the same rules as package(): .pkg, then a debrid service that
    // is configured, valid and lists the host, then the provider tables. Answered
    // from memory. Empty (and ok) when there is no catalogue or no classification;
    // the grid then falls back to the package's own flags.
    virtual Result<std::vector<std::uint8_t>> package_tiers()
    {
        Result<std::vector<std::uint8_t>> none;
        none.status = 200;
        return none;
    }

    // ---- queue actions; the value is the updated queue ----
    // pause, resume, cancel, retry.
    virtual Result<std::vector<Job>> job_action(std::string_view job_id,
                                                std::string_view action) = 0;
    virtual Result<std::vector<Job>> clear_finished() = 0;
    virtual Result<std::vector<Job>> archive_password(std::string_view job_id,
                                                      std::string_view password) = 0;
    virtual Result<std::vector<Job>> archive_choice(std::string_view job_id, int index) = 0;

    // ---- link flows ----
    // "Download this link" for a package link, exactly as Pegasus' own UI decides.
    // service: "real-debrid" or "torbox" when the player chose one, else empty.
    virtual Result<LinkResult> start_link(std::string_view package_id, int link_index,
                                          std::string_view service) = 0;
    virtual Result<LinkResult> debrid_select(std::string_view selection_id,
                                             const std::vector<int> &indexes,
                                             std::string_view service) = 0;
    virtual Result<CaptureView> capture_status(std::string_view session_id) = 0;
    // A session that is already running (the app was restarted mid-capture).
    virtual Result<CaptureView> capture_active() = 0;
    virtual Result<bool> capture_cancel(std::string_view session_id) = 0;
    virtual Result<Probe> probe_link(std::string_view url) = 0;
    virtual Result<Dispatch> queue_direct(const Probe &probe, std::string_view file_name) = 0;

    // ---- sources; the value is the updated list ----
    virtual Result<std::vector<Source>> source_add_url(std::string_view url) = 0;
    virtual Result<std::vector<Source>> source_delete(std::string_view source_id) = 0;
    virtual Result<std::vector<Source>> source_refresh(std::string_view source_id) = 0;
    virtual Result<std::vector<Source>> source_enable(std::string_view source_id, bool enabled) = 0;
    virtual Result<DefaultSources> default_sources(bool force_refresh) = 0;
    // The failures (one line each) of the sources that could not be added.
    virtual Result<std::vector<std::string>> default_sources_apply(
        std::int64_t revision, const std::vector<std::string> &ids) = 0;
    virtual Result<bool> default_sources_seen() = 0;

    // ---- settings (write); the value is the settings as Pegasus now has them ----
    // Validated like Pegasus' own page (connections 1..64, a folder, tokens); a
    // refused value is a status 400 error and nothing is sent.
    virtual Result<SettingsView> settings_update(const SettingsPatch &patch) = 0;
    // Asks Pegasus to check a debrid token again. service: "real-debrid", "torbox".
    virtual Result<SettingsView> debrid_refresh(std::string_view service) = 0;

    // ---- storage and folders ----
    virtual Result<std::vector<StorageMount>> mounts() = 0;
    virtual Result<FolderListing> browse(std::string_view path) = 0;
    virtual Result<bool> mkdir(std::string_view full_path) = 0;

    // ---- logs ----
    virtual Result<bool> logs_clear() = 0;

    // ---- installed games: APR Emu and delete ----
    // 503 (and a message) when Pegasus cannot read the APR manifest.
    virtual Result<std::vector<AprVersionItem>> apr_versions() = 0;
    // version_id empty: update to the latest; else reinstall that build.
    virtual Result<AdminJob> apr_update_start(std::string_view title_id,
                                              std::string_view version_id) = 0;
    virtual Result<AdminJob> apr_update_status(std::string_view job_id,
                                               std::string_view title_id) = 0;
    // DESTRUCTIVE: deletes the game from the console.
    virtual Result<AdminJob> game_delete(std::string_view title_id) = 0;
    virtual Result<AdminJob> game_delete_status(std::string_view job_id,
                                                std::string_view title_id) = 0;

    // ---- file manager ----
    virtual Result<std::vector<StorageMount>> file_locations() = 0;
    virtual Result<FolderListing> file_list(std::string_view path) = 0;
    virtual Result<std::vector<FileOp>> file_operations() = 0;
    virtual Result<FileItem> file_mkdir(std::string_view parent, std::string_view name) = 0;
    virtual Result<FileItem> file_rename(std::string_view path, std::string_view name) = 0;
    // DESTRUCTIVE. collision: "keep-both", "replace" or "skip" (copy, move, extract).
    virtual Result<std::vector<FileOp>> file_delete(const std::vector<std::string> &paths) = 0;
    virtual Result<std::vector<FileOp>> file_copy(const std::vector<std::string> &paths,
                                                  std::string_view destination,
                                                  std::string_view collision) = 0;
    virtual Result<std::vector<FileOp>> file_move(const std::vector<std::string> &paths,
                                                  std::string_view destination,
                                                  std::string_view collision) = 0;
    virtual Result<std::vector<FileOp>> file_extract(const std::vector<std::string> &paths,
                                                     std::string_view destination,
                                                     std::string_view collision) = 0;
    virtual Result<std::vector<FileOp>> file_extract_password(std::string_view operation_id,
                                                              std::string_view password) = 0;
    virtual Result<std::vector<FileOp>> file_pkg_install(std::string_view path) = 0;
    virtual Result<bool> file_cancel(std::string_view operation_id) = 0;
};

} // namespace orbit
