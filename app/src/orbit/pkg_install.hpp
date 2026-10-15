// .pkg links that download fast to disk and then install (B-17).
//
// Pegasus DL hands a link to the PS5's own (slow) Downloads when the URL or the file
// name ends in ".pkg", but a file name ending in ".fpkg" wins and keeps the download
// in Pegasus' fast segmented queue (POST /api/queue/direct). Wings asks for
// "<Title>.fpkg", renames the finished file to "<Title>.pkg", installs it with
// POST /api/files/pkg/install and watches GET /api/files/operations.
//
// PkgInstallModel is the pure part (no I/O, no clock): the per-link route choice and
// the per-job stages downloading -> renaming -> installing -> installed | failed.
// PkgInstalls is the shared, locked copy the Store worker drives and the UI reads.
#pragma once

#include "orbit/model.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// ---- route choice (per link; Pegasus' settings are never changed) ----

enum class PkgRoute : std::uint8_t
{
    direct,  // POST /api/queue/direct with "<Title>.fpkg"
    handoff, // the old POST /api/queue: the PS5's own Downloads
};

struct PkgRouteChoice
{
    PkgRoute route = PkgRoute::handoff;
    std::string reason; // for the [WINGS-PKG] log
};

// Before queueing: the probe answer and the free space. size_bytes < 0 means unknown
// (then space is not checked). mounts empty means the free space is unknown.
PkgRouteChoice pkg_route_before_queue(bool probe_ok, std::string_view probe_rejection,
                                      double size_bytes, const std::string &download_root,
                                      const std::vector<StorageMount> &mounts);

// After POST /api/queue/direct: Pegasus may refuse it or hand the file to the PS5 anyway.
enum class PkgDirectAnswer : std::uint8_t
{
    track,       // queued in Pegasus' own queue with a job id: watch it
    handed_off,  // Pegasus already sent it to the PS5's Downloads: show that, queue nothing more
    use_handoff, // refused or no job id: fall back to the old hand-off
};
PkgDirectAnswer pkg_classify_direct(bool call_ok, bool native, bool queued, std::int64_t job_id);

// Bytes a .pkg of this size needs: the file in the download root, the installed app on
// /data, and a staging copy on /data when the root is elsewhere.
bool pkg_space_ok(double size_bytes, const std::string &download_root,
                  const std::vector<StorageMount> &mounts, std::string *why);

// "God of War: Ragnarok (USA).pkg" -> "God of War Ragnarok (USA)". Never empty.
std::string pkg_file_stem(std::string_view title_or_file);
std::string pkg_folder_of(std::string_view path);

// ---- per-job stages ----

enum class PkgStage : std::uint8_t
{
    downloading,
    renaming,
    installing,
    installed,
    failed,
};

struct PkgJobSeen // one queue job, as Pegasus reports it
{
    std::string id;
    std::string status; // Job::status words
    std::string destination;
    std::string error;
};

struct PkgOpSeen // one file operation
{
    std::string id;
    std::string type;
    std::string status; // FileOp::status words
    std::string error;
};

struct PkgStep
{
    enum class Kind : std::uint8_t
    {
        none,
        rename,  // POST /api/files/rename {path, name}
        install, // POST /api/files/pkg/install {path}
        poll,    // GET /api/files/operations
        confirm, // GET /appdb: is title_id installed yet?
        remove,  // POST /api/files/delete {paths:[path]}
    } kind = Kind::none;
    std::string job_id;
    std::string path;
    std::string name;     // rename
    std::string title_id; // confirm; when empty, any title not in `before` counts
    std::vector<std::string> before; // confirm: the installed title ids from before the install
};

// Is the install on the console? By title id, or (no title id) a title that was not there before.
bool pkg_install_seen(const std::string &title_id, const std::vector<std::string> &before,
                      const std::vector<std::string> &now);

class PkgInstallModel
{
public:
    struct Entry
    {
        std::string job_id;
        std::string title;
        std::string title_id;
        std::string wanted_name; // "<Title>.pkg"
        std::string path;        // where the file is now
        PkgStage stage = PkgStage::downloading;
        std::string op_id;
        std::string error;
        int missing_polls = 0;
        int confirm_checks = 0;
        double next_confirm = 0.0;
        bool on_console = false;   // seen in the installed games
        bool cleanup_done = false; // deleted, kept on purpose, or given up
        bool has_before = false;   // `before` was read before the install started
        std::vector<std::string> before;
    };

    void set_keep_files(bool keep) { keep_files_ = keep; }
    bool keep_files() const { return keep_files_; }

    void track(const std::string &job_id, const std::string &title, const std::string &title_id,
               const std::string &wanted_name);
    const Entry *find(const std::string &job_id) const;

    void observe_queue(const std::vector<PkgJobSeen> &jobs);
    // What to do now. One rename/install at a time (Pegasus runs one install at a time).
    PkgStep next(double now_seconds) const;
    void renamed(const std::string &job_id, bool ok, const std::string &new_path, const std::string &error);
    // The installed title ids just before POST /api/files/pkg/install (for a package with no title id).
    void titles_before(const std::string &job_id, const std::vector<std::string> &title_ids);
    void install_started(const std::string &job_id, bool ok, const std::vector<PkgOpSeen> &ops,
                         const std::string &error);
    void observe_ops(const std::vector<PkgOpSeen> &ops);
    void confirmed(const std::string &job_id, bool found, double now_seconds);
    void removed(const std::string &job_id, bool ok, const std::string &error);

    // Player text for the download row; empty when the job is not a tracked .pkg or still downloading.
    std::string row_text(const std::string &job_id) const;

    std::vector<std::string> take_log();

private:
    Entry *find_mut(const std::string &job_id);
    void fail(Entry &entry, const std::string &reason);
    void log(const std::string &line) { log_.push_back("[WINGS-PKG] " + line); }

    std::map<std::string, Entry> jobs_;
    bool keep_files_ = false;
    std::vector<std::string> log_;
};

// The shared copy. Log lines go to stdout (the title log) unless a sink is set.
class PkgInstalls
{
public:
    void set_keep_files(bool keep);
    bool keep_files() const;
    void track(const std::string &job_id, const std::string &title, const std::string &title_id,
               const std::string &wanted_name);
    bool tracked(const std::string &job_id) const;
    std::string row_text(const std::string &job_id) const;
    PkgStage stage(const std::string &job_id) const; // downloading when untracked

    // Runs on the Store worker after every queue poll; call() performs one step's I/O.
    void observe_queue(const std::vector<PkgJobSeen> &jobs);
    PkgStep next() const;
    void renamed(const std::string &job_id, bool ok, const std::string &new_path, const std::string &error);
    void titles_before(const std::string &job_id, const std::vector<std::string> &title_ids);
    void install_started(const std::string &job_id, bool ok, const std::vector<PkgOpSeen> &ops,
                         const std::string &error);
    void observe_ops(const std::vector<PkgOpSeen> &ops);
    void confirmed(const std::string &job_id, bool found);
    void removed(const std::string &job_id, bool ok, const std::string &error);
    void log(const std::string &line); // adds the [WINGS-PKG] prefix
    void set_log(std::function<void(const std::string &)> sink);
    void reset(); // tests

private:
    void flush();

    mutable std::mutex mutex_;
    PkgInstallModel model_;
    std::function<void(const std::string &)> sink_;
};

PkgInstalls &pkg_installs();

} // namespace orbit
