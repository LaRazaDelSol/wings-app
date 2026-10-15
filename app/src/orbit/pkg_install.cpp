#include "orbit/pkg_install.hpp"

#include "orbit/i18n.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace orbit
{
namespace
{

bool under(std::string_view path, std::string_view mount)
{
    while (mount.size() > 1 && mount.back() == '/')
        mount.remove_suffix(1);
    if (mount.empty() || path.substr(0, mount.size()) != mount)
        return false;
    return path.size() == mount.size() || mount == "/" || path[mount.size()] == '/';
}

const StorageMount *mount_for(std::string_view path, const std::vector<StorageMount> &mounts)
{
    const StorageMount *best = nullptr;
    for (const StorageMount &m : mounts)
    {
        if (under(path, m.path) && (best == nullptr || m.path.size() > best->path.size()))
            best = &m;
    }
    return best;
}

bool ends_with_nocase(std::string_view text, std::string_view tail)
{
    if (text.size() < tail.size())
        return false;
    for (std::size_t i = 0; i < tail.size(); ++i)
    {
        char a = text[text.size() - tail.size() + i];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (a != tail[i])
            return false;
    }
    return true;
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

std::string gb(double bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GB", bytes / 1e9);
    return buf;
}

double steady_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

constexpr int kMissingPollsBeforeFail = 5;
constexpr int kConfirmChecks = 90;          // about 30 minutes
constexpr double kConfirmEverySeconds = 20.0;

} // namespace

// ------------------------------------------------------------------- route --

bool pkg_space_ok(double size_bytes, const std::string &download_root,
                  const std::vector<StorageMount> &mounts, std::string *why)
{
    if (size_bytes <= 0.0 || mounts.empty())
        return true; // unknown: let Pegasus try
    const StorageMount *root = mount_for(download_root, mounts);
    const StorageMount *data = mount_for("/data", mounts);
    const bool root_on_data = under(download_root, "/data") || under(download_root, "/user/data");
    // The .pkg in the root; the installed app on internal storage; a staging copy there too
    // when the root is not under /data (Pegasus copies it before installing).
    const double root_need = root_on_data ? size_bytes * 2.0 : size_bytes;
    const double data_need = size_bytes * 2.0;
    if (root != nullptr && root->free_bytes < root_need)
    {
        if (why)
            *why = "needs " + gb(root_need) + " free on " + root->path + ", has " + gb(root->free_bytes);
        return false;
    }
    if (!root_on_data && data != nullptr && data->free_bytes < data_need)
    {
        if (why)
            *why = "needs " + gb(data_need) + " free on " + data->path + ", has " + gb(data->free_bytes);
        return false;
    }
    return true;
}

PkgRouteChoice pkg_route_before_queue(bool probe_ok, std::string_view probe_rejection,
                                      double size_bytes, const std::string &download_root,
                                      const std::vector<StorageMount> &mounts)
{
    PkgRouteChoice out;
    if (!probe_ok)
    {
        out.reason = "the link probe failed";
        return out;
    }
    if (!probe_rejection.empty())
    {
        out.reason = "the probe refused the link: " + std::string(probe_rejection);
        return out;
    }
    std::string why;
    if (!pkg_space_ok(size_bytes, download_root, mounts, &why))
    {
        out.reason = "not enough free space (" + why + ")";
        return out;
    }
    out.route = PkgRoute::direct;
    out.reason = size_bytes > 0.0 ? "probe ok, " + gb(size_bytes) + " fits" : "probe ok, size unknown";
    return out;
}

PkgDirectAnswer pkg_classify_direct(bool call_ok, bool native, bool queued, std::int64_t job_id)
{
    if (!call_ok)
        return PkgDirectAnswer::use_handoff;
    if (native)
        return PkgDirectAnswer::handed_off;
    if (queued && job_id > 0)
        return PkgDirectAnswer::track;
    return PkgDirectAnswer::use_handoff;
}

std::string pkg_file_stem(std::string_view name)
{
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    if (ends_with_nocase(name, ".fpkg"))
        name.remove_suffix(5);
    else if (ends_with_nocase(name, ".pkg"))
        name.remove_suffix(4);
    std::string out;
    for (const char c : name)
    {
        const auto u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || c == '/' || c == '\\';
        const char put = bad ? ' ' : c;
        if (put == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out += put;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.size() > 120)
        out.resize(120); // may cut a UTF-8 sequence's tail; trim it back to a lead byte
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0xC0)
        out.pop_back();
    return out.empty() ? std::string("package") : out;
}

std::string pkg_folder_of(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string_view::npos)
        return std::string(path);
    return std::string(path.substr(0, slash == 0 ? 1 : slash));
}

bool pkg_install_seen(const std::string &title_id, const std::vector<std::string> &before,
                      const std::vector<std::string> &now)
{
    for (const std::string &id : now)
    {
        if (!title_id.empty())
        {
            if (id == title_id)
                return true;
            continue;
        }
        bool old = false;
        for (const std::string &b : before)
            old = old || b == id;
        if (!old)
            return true;
    }
    return false;
}

// ------------------------------------------------------------------- model --

void PkgInstallModel::track(const std::string &job_id, const std::string &title,
                            const std::string &title_id, const std::string &wanted_name)
{
    Entry e;
    e.job_id = job_id;
    e.title = title;
    e.title_id = title_id;
    e.wanted_name = wanted_name;
    jobs_[job_id] = std::move(e);
    log("job " + job_id + ": fast download to disk, then install as \"" + wanted_name + "\"");
}

const PkgInstallModel::Entry *PkgInstallModel::find(const std::string &job_id) const
{
    const auto it = jobs_.find(job_id);
    return it == jobs_.end() ? nullptr : &it->second;
}

PkgInstallModel::Entry *PkgInstallModel::find_mut(const std::string &job_id)
{
    const auto it = jobs_.find(job_id);
    return it == jobs_.end() ? nullptr : &it->second;
}

void PkgInstallModel::fail(Entry &entry, const std::string &reason)
{
    entry.stage = PkgStage::failed;
    entry.error = reason;
    entry.cleanup_done = true; // a failed install never deletes the file
    log("job " + entry.job_id + ": install failed: " + reason + "; the file stays at " + entry.path);
}

void PkgInstallModel::observe_queue(const std::vector<PkgJobSeen> &jobs)
{
    for (auto it = jobs_.begin(); it != jobs_.end();)
    {
        Entry &e = it->second;
        if (e.stage != PkgStage::downloading)
        {
            ++it;
            continue;
        }
        const PkgJobSeen *seen = nullptr;
        for (const PkgJobSeen &j : jobs)
        {
            if (j.id == e.job_id)
                seen = &j;
        }
        if (seen == nullptr)
        {
            log("job " + e.job_id + ": left the queue before it finished; no install");
            it = jobs_.erase(it);
            continue;
        }
        if (seen->status == "completed")
        {
            if (seen->destination.empty())
            {
                fail(e, tr("Pegasus DL did not say where the file is"));
            }
            else
            {
                e.path = seen->destination;
                const bool named = ends_with_nocase(e.path, ".pkg");
                e.stage = named ? PkgStage::installing : PkgStage::renaming;
                log("job " + e.job_id + ": downloaded to " + e.path);
            }
        }
        ++it;
    }
}

PkgStep PkgInstallModel::next(double now_seconds) const
{
    PkgStep step;
    // An install in flight blocks everything else: Pegasus runs one at a time.
    for (const auto &[id, e] : jobs_)
    {
        if (e.stage == PkgStage::installing && !e.op_id.empty())
        {
            step.kind = PkgStep::Kind::poll;
            step.job_id = id;
            return step;
        }
    }
    for (const auto &[id, e] : jobs_)
    {
        if (e.stage == PkgStage::installing)
        {
            step.kind = PkgStep::Kind::install;
            step.job_id = id;
            step.path = e.path;
            step.title_id = e.title_id;
            return step;
        }
    }
    for (const auto &[id, e] : jobs_)
    {
        if (e.stage == PkgStage::renaming)
        {
            step.kind = PkgStep::Kind::rename;
            step.job_id = id;
            step.path = e.path;
            step.name = e.wanted_name;
            return step;
        }
    }
    if (keep_files_)
        return step;
    for (const auto &[id, e] : jobs_)
    {
        if (e.stage != PkgStage::installed || e.cleanup_done)
            continue;
        if (e.on_console)
        {
            step.kind = PkgStep::Kind::remove;
            step.job_id = id;
            step.path = e.path;
            return step;
        }
        if (now_seconds >= e.next_confirm)
        {
            step.kind = PkgStep::Kind::confirm;
            step.job_id = id;
            step.title_id = e.title_id;
            step.before = e.before;
            return step;
        }
    }
    return step;
}

void PkgInstallModel::renamed(const std::string &job_id, bool ok, const std::string &new_path,
                              const std::string &error)
{
    Entry *e = find_mut(job_id);
    if (e == nullptr || e->stage != PkgStage::renaming)
        return;
    if (!ok)
    {
        fail(*e, tr("could not rename it to .pkg") + (error.empty() ? std::string() : " (" + error + ")"));
        return;
    }
    e->path = !new_path.empty() ? new_path : pkg_folder_of(e->path) + "/" + e->wanted_name;
    e->stage = PkgStage::installing;
    log("job " + job_id + ": renamed to " + e->path);
}

void PkgInstallModel::titles_before(const std::string &job_id, const std::vector<std::string> &title_ids)
{
    Entry *e = find_mut(job_id);
    if (e == nullptr || e->stage != PkgStage::installing || !e->op_id.empty())
        return;
    e->before = title_ids;
    e->has_before = true;
}

void PkgInstallModel::install_started(const std::string &job_id, bool ok,
                                      const std::vector<PkgOpSeen> &ops, const std::string &error)
{
    Entry *e = find_mut(job_id);
    if (e == nullptr || e->stage != PkgStage::installing || !e->op_id.empty())
        return;
    if (!ok)
    {
        if (contains(error, "already running"))
        {
            log("job " + job_id + ": another install is running; waiting");
            return;
        }
        fail(*e, error.empty() ? std::string(tr("Pegasus DL refused the install")) : error);
        return;
    }
    e->op_id = "any";
    for (const PkgOpSeen &op : ops)
    {
        if (contains(op.type, "pkg") || ops.size() == 1)
            e->op_id = op.id;
    }
    e->missing_polls = 0;
    log("job " + job_id + ": install started (operation " + e->op_id + ")");
}

void PkgInstallModel::observe_ops(const std::vector<PkgOpSeen> &ops)
{
    for (auto &[id, e] : jobs_)
    {
        if (e.stage != PkgStage::installing || e.op_id.empty())
            continue;
        const PkgOpSeen *op = nullptr;
        for (const PkgOpSeen &o : ops)
        {
            if (o.id == e.op_id || (e.op_id == "any" && contains(o.type, "pkg")))
                op = &o;
        }
        if (op == nullptr)
        {
            if (++e.missing_polls >= kMissingPollsBeforeFail)
                fail(e, tr("Pegasus DL lost track of the install"));
            continue;
        }
        e.missing_polls = 0;
        if (op->status == "accepted" || op->status == "completed")
        {
            e.stage = PkgStage::installed;
            log("job " + id + ": the PS5 accepted the package" +
                std::string(keep_files_ ? "; keeping the file (setting)" : ""));
            if (e.title_id.empty() && !e.has_before && !keep_files_)
            {
                e.cleanup_done = true;
                log("job " + id + ": no way to confirm the install; keeping " + e.path);
            }
        }
        else if (op->status == "failed" || op->status == "cancelled")
        {
            fail(e, op->error.empty() ? std::string(tr("the PS5 refused the package")) : op->error);
        }
    }
}

void PkgInstallModel::confirmed(const std::string &job_id, bool found, double now_seconds)
{
    Entry *e = find_mut(job_id);
    if (e == nullptr || e->stage != PkgStage::installed || e->cleanup_done)
        return;
    if (found)
    {
        e->on_console = true;
        log("job " + job_id + ": " + (e->title_id.empty() ? std::string("a new title") : e->title_id) +
            " is on the console");
        return;
    }
    e->next_confirm = now_seconds + kConfirmEverySeconds;
    if (++e->confirm_checks >= kConfirmChecks)
    {
        e->cleanup_done = true;
        log("job " + job_id + ": " + e->title_id + " never showed as installed; keeping " + e->path);
    }
}

void PkgInstallModel::removed(const std::string &job_id, bool ok, const std::string &error)
{
    Entry *e = find_mut(job_id);
    if (e == nullptr || e->stage != PkgStage::installed)
        return;
    e->cleanup_done = true;
    if (ok)
        log("job " + job_id + ": deleted " + e->path);
    else
        log("job " + job_id + ": could not delete " + e->path + ": " + error);
}

std::string PkgInstallModel::row_text(const std::string &job_id) const
{
    const Entry *e = find(job_id);
    if (e == nullptr)
        return {};
    switch (e->stage)
    {
    case PkgStage::downloading:
        return {};
    case PkgStage::renaming:
    case PkgStage::installing:
        return tr("Installing...");
    case PkgStage::installed:
        return tr("Installed - find it on your home screen");
    case PkgStage::failed:
        return tr("Install failed: {reason} (the file is kept in {folder})",
                  {{"reason", e->error}, {"folder", pkg_folder_of(e->path)}});
    }
    return {};
}

std::vector<std::string> PkgInstallModel::take_log()
{
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

// ------------------------------------------------------------ shared copy --

void PkgInstalls::set_keep_files(bool keep)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.set_keep_files(keep);
}

bool PkgInstalls::keep_files() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return model_.keep_files();
}

void PkgInstalls::track(const std::string &job_id, const std::string &title,
                        const std::string &title_id, const std::string &wanted_name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.track(job_id, title, title_id, wanted_name);
    flush();
}

bool PkgInstalls::tracked(const std::string &job_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return model_.find(job_id) != nullptr;
}

std::string PkgInstalls::row_text(const std::string &job_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return model_.row_text(job_id);
}

PkgStage PkgInstalls::stage(const std::string &job_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const PkgInstallModel::Entry *e = model_.find(job_id);
    return e == nullptr ? PkgStage::downloading : e->stage;
}

void PkgInstalls::observe_queue(const std::vector<PkgJobSeen> &jobs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.observe_queue(jobs);
    flush();
}

PkgStep PkgInstalls::next() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return model_.next(steady_seconds());
}

void PkgInstalls::renamed(const std::string &job_id, bool ok, const std::string &new_path,
                          const std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.renamed(job_id, ok, new_path, error);
    flush();
}

void PkgInstalls::titles_before(const std::string &job_id, const std::vector<std::string> &title_ids)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.titles_before(job_id, title_ids);
}

void PkgInstalls::install_started(const std::string &job_id, bool ok, const std::vector<PkgOpSeen> &ops,
                                  const std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.install_started(job_id, ok, ops, error);
    flush();
}

void PkgInstalls::observe_ops(const std::vector<PkgOpSeen> &ops)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.observe_ops(ops);
    flush();
}

void PkgInstalls::confirmed(const std::string &job_id, bool found)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.confirmed(job_id, found, steady_seconds());
    flush();
}

void PkgInstalls::removed(const std::string &job_id, bool ok, const std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    model_.removed(job_id, ok, error);
    flush();
}

void PkgInstalls::log(const std::string &line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string full = "[WINGS-PKG] " + line;
    if (sink_)
        sink_(full);
    else
    {
        std::printf("%s\n", full.c_str());
        std::fflush(stdout);
    }
}

void PkgInstalls::set_log(std::function<void(const std::string &)> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void PkgInstalls::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const bool keep = model_.keep_files();
    model_ = PkgInstallModel();
    model_.set_keep_files(keep);
}

void PkgInstalls::flush()
{
    for (const std::string &line : model_.take_log())
    {
        if (sink_)
            sink_(line);
        else
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    }
}

PkgInstalls &pkg_installs()
{
    static PkgInstalls instance;
    return instance;
}

} // namespace orbit
