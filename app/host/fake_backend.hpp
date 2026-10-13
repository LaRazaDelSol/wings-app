// Wings TV app - A fake backend for the PC preview and tests.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Answers the Backend interface with a handful of invented packages (each
// with links that take every route: native .pkg, automated provider, browser
// capture, debrid, debrid with file selection), queue jobs in every state,
// installed games, sources (which can be added, refreshed, switched and
// deleted) and log lines, and draws simple cover art for the installed games.
// Nothing here is a real product or real artwork. Preview and tests only:
// never part of the console app.

#pragma once

#include "orbit/backend.hpp"
#include "orbit/thread.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace orbit::host
{

enum class FakeMode
{
    normal,     // Pegasus answers, with sample data
    offline,    // Pegasus is not running
    empty,      // Pegasus answers, but has no sources, packages or jobs
    onboarding, // no sources yet, and Pegasus offers its default list
};

class FakeBackend final : public Backend
{
  public:
    explicit FakeBackend(FakeMode mode = FakeMode::normal);

    // Preview scripts flip this to show "waiting" turning into "connected".
    void set_mode(FakeMode mode);
    // Every capture status call moves the session one step; this many steps
    // before it completes (default 5).
    void set_capture_steps(int steps);
    // Number of /capture/cancel calls seen (tests).
    int cancelled_captures() const;
    // start_link() of this one link fails with a host error (tests: one part of a set).
    void set_failing_link(const std::string &package_id, int link_index);
    // start_link() calls that reached a link (tests).
    int started_links() const;
    // Scripted capture (tests): while `states` is not empty, an automated or
    // capture link starts a session with this requires_browser value, and each
    // capture_status() answers the next of `states` ("priming", "queueing",
    // "completed", "failed", ...), the last one repeating. `message` goes with a
    // "failed" or "cancelled" state; "completed" queues a job.
    void set_capture_script(std::vector<std::string> states, bool requires_browser = false,
                            std::string message = std::string());
    // Called at every scripted capture_status(), outside the fake's lock, with
    // the number of scripted reads so far (tests: act in the middle of a wait).
    void set_capture_hook(std::function<void(int)> hook);
    // Scripted capture_status() calls seen (tests).
    int capture_reads() const;
    // Store posters ("pkg:" keys). Off (default): no artwork, as with the HTTPS
    // art service switched off. On: stands in for the service, so the preview and
    // tests see covers arriving: the first two asks per key answer "pending",
    // then a generated picture; about one package in seven has no cover (stays a
    // placeholder).
    void set_store_art(bool on);
    bool store_art() const;
    // Asks seen for a key since the start (tests).
    int art_asks(const std::string &key) const;

    std::string endpoint() const override
    {
        return "127.0.0.1:6970";
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

    // ---- test hooks ----
    // Pegasus answers 503 on the APR version list (the user's real console does).
    void set_apr_manifest_ok(bool ok);
    std::vector<std::string> deleted_games() const;
    std::vector<std::string> deleted_paths() const;
    std::vector<std::string> pkg_installs() const;
    std::string last_token(bool torbox) const;
    int settings_writes() const;
    // Starts a test with this download root ("" = Pegasus reports none yet).
    void set_download_root(const std::string &root);
    // Every mkdir() call received, in order (also the refused ones).
    std::vector<std::string> mkdir_calls() const;
    // mkdir() answers a host error (403) without creating anything.
    void set_mkdir_fails(bool fails);
    // Folder of the last accepted root update ("" before any).
    std::string last_root_written() const;
    int settings_reads() const; // settings() calls that were answered
    int logs_cleared() const;
    int apr_updates() const;
    int cancelled_ops() const;
    std::string last_collision() const;
    bool path_exists(const std::string &path) const;
    // Files tab. Folders that answer an error when listed, by /api/files/list
    // and /api/browse alike (default: /mnt, which the real console refuses).
    void set_unlistable(const std::string &path, bool unlistable);
    // Listing this folder waits `milliseconds` in real time first (a busy
    // Pegasus); with timeout it then fails as the 8 s transport timeout does.
    void set_slow_listing(const std::string &path, int milliseconds, bool timeout);
    // Adds an operation as given (the id is assigned when empty). frozen: it
    // does not advance by itself. cancel_delay_s > 0: a cancel is accepted but
    // honoured only that many seconds later (the op keeps running meanwhile).
    std::string add_file_op(FileOp op, bool frozen = true, double cancel_delay_s = 0.0);
    // While on, the operations poll fails (Pegasus too busy to answer).
    void set_ops_failing(bool failing);
    int file_list_calls() const;

  private:
    struct FsNode
    {
        bool dir = false;
        double size = 0.0;
    };
    void seed_admin_locked();
    FolderListing list_locked(const std::string &path, bool dirs_only, bool *found) const;
    FileOp &add_op_locked(const char *type, const std::string &source,
                          const std::string &destination, double bytes);
    Result<std::vector<FileOp>> copy_or_move(const std::vector<std::string> &paths,
                                             const std::string &destination,
                                             const std::string &collision, bool move,
                                             const char *type);
    SettingsView settings_;
    std::vector<StorageMount> mounts_;
    std::map<std::string, FsNode> fs_;
    std::vector<InstalledGame> installed_;
    std::vector<FileOp> ops_;
    std::map<std::string, int> op_age_;
    std::map<std::string, std::pair<int, std::string>> apr_jobs_;
    std::map<std::string, int> delete_jobs_;
    std::vector<std::string> deleted_;
    std::vector<std::string> deleted_paths_;
    std::vector<std::string> installs_;
    std::string last_rd_token_;
    std::string last_tb_token_;
    std::string last_collision_;
    bool apr_manifest_ok_ = true;
    int settings_writes_ = 0;
    std::vector<std::string> mkdir_calls_;
    bool mkdir_fails_ = false;
    std::string last_root_written_;
    int settings_reads_ = 0;
    int logs_cleared_ = 0;
    int apr_updates_ = 0;
    int cancelled_ops_ = 0;
    int next_op_ = 1;
    std::map<std::string, bool> unlistable_;
    std::map<std::string, std::pair<int, bool>> slow_lists_;
    std::map<std::string, bool> frozen_ops_;
    std::map<std::string, double> cancel_delay_;
    std::map<std::string, double> cancel_at_;
    bool ops_failing_ = false;
    int file_list_calls_ = 0;

    template <typename T> Result<T> refuse() const;
    void advance_locked();
    void log_locked(const char *level, const char *component, std::string message);
    Job &add_job_locked(const std::string &name, const char *status, double total_gb);

    mutable Mutex mutex_;
    FakeMode mode_;
    std::vector<Job> jobs_;
    std::vector<LogLine> logs_;
    std::vector<Source> sources_;
    std::vector<DefaultSource> defaults_;
    bool onboarding_ = false;
    int polls_ = 0;
    int next_job_ = 100;
    int next_source_ = 10;
    // The one capture session the fake runs.
    int capture_step_ = -1;
    int capture_steps_ = 5;
    std::string capture_title_;
    bool capture_automated_ = false;
    int cancelled_ = 0;
    std::string failing_link_; // "<packageId>#<linkIndex>"
    std::vector<std::string> script_states_;
    std::size_t script_pos_ = 0;
    bool script_browser_ = false;
    std::string script_message_;
    int script_reads_ = 0;
    std::function<void(int)> capture_hook_;
    int started_links_ = 0;
    bool store_art_ = false;
    std::map<std::string, int> art_asks_;
};

} // namespace orbit::host
