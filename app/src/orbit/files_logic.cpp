// Wings TV app - The Files tab's rules, apart from drawing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/files_logic.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace orbit::files
{

namespace
{
constexpr std::size_t kTraceLines = 60;

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ends_with(const std::string &text, std::string_view tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// "/mnt/ext" or "/mnt/usb" followed by digits only.
bool numbered(const std::string &path, std::string_view prefix)
{
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
        return false;
    for (std::size_t i = prefix.size(); i < path.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(path[i])))
            return false;
    }
    return true;
}
} // namespace

// ---- paths ----

std::string normalize(std::string_view path)
{
    std::string out;
    out.reserve(path.size() + 1);
    for (char c : path)
    {
        if (c == '/' && !out.empty() && out.back() == '/')
            continue;
        out.push_back(c);
    }
    if (out.empty() || out.front() != '/')
        out.insert(out.begin(), '/');
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
    return out;
}

std::string parent(const std::string &path)
{
    if (path.empty() || path == "/")
        return std::string();
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string base_name(const std::string &path)
{
    if (path == "/" || path.empty())
        return "/";
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join(const std::string &folder, const std::string &name)
{
    if (folder.empty() || folder == "/")
        return "/" + name;
    return folder + "/" + name;
}

bool inside(const std::string &path, const std::string &folder)
{
    if (folder == "/")
        return !path.empty() && path.front() == '/';
    return path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
                              path[folder.size()] == '/');
}

std::vector<Crumb> crumbs(const std::string &path)
{
    std::vector<Crumb> out;
    out.push_back({"/", "/"});
    std::string at;
    std::size_t start = 1;
    while (start < path.size())
    {
        std::size_t slash = path.find('/', start);
        if (slash == std::string::npos)
            slash = path.size();
        const std::string part = path.substr(start, slash - start);
        if (!part.empty())
        {
            at += "/" + part;
            out.push_back({part, at});
        }
        start = slash + 1;
    }
    return out;
}

// ---- drives ----

bool quick_location(const std::string &path)
{
    return path == "/data" || numbered(path, "/mnt/ext") || numbered(path, "/mnt/usb");
}

std::vector<Drive> drives(const std::vector<StorageMount> &locations)
{
    std::vector<Drive> out;
    Drive main;
    main.path = "/data";
    main.label = tr("Main drive");
    out.push_back(main);
    std::vector<Drive> ext;
    std::vector<Drive> usb;
    for (const StorageMount &m : locations)
    {
        const std::string path = normalize(m.path);
        if (!quick_location(path))
            continue;
        Drive d;
        d.path = path;
        d.label = path == "/data" ? std::string(tr("Main drive")) : base_name(path);
        if (m.total_bytes > 0.0)
        {
            d.total_bytes = m.total_bytes;
            d.free_bytes = m.free_bytes;
        }
        d.writable = m.writable;
        if (path == "/data")
            out[0] = d;
        else if (path.compare(0, 8, "/mnt/ext") == 0)
        {
            if (std::none_of(ext.begin(), ext.end(), [&](const Drive &x) { return x.path == path; }))
                ext.push_back(d);
        }
        else if (std::none_of(usb.begin(), usb.end(), [&](const Drive &x) { return x.path == path; }))
        {
            usb.push_back(d);
        }
    }
    const auto by_path = [](const Drive &a, const Drive &b)
    { return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path; };
    std::sort(ext.begin(), ext.end(), by_path);
    std::sort(usb.begin(), usb.end(), by_path);
    out.insert(out.end(), ext.begin(), ext.end());
    out.insert(out.end(), usb.begin(), usb.end());
    Drive root;
    root.path = "/";
    root.label = tr("Root");
    root.writable = false;
    out.push_back(root);
    return out;
}

const StorageMount *mount_of(const std::string &path, const std::vector<StorageMount> &locations)
{
    const StorageMount *best = nullptr;
    std::size_t best_len = 0;
    for (const StorageMount &m : locations)
    {
        const std::string where = normalize(m.path);
        if (!inside(path, where))
            continue;
        if (best == nullptr || where.size() > best_len)
        {
            best = &m;
            best_len = where.size();
        }
    }
    return best;
}

// ---- entries ----

std::string format_size(double bytes)
{
    if (!(bytes >= 0.0))
        return std::string();
    if (bytes < 1000.0)
        return i18n::count(static_cast<long>(bytes)) + " B";
    if (bytes < 1e6)
        return i18n::decimal(bytes / 1e3) + " KB";
    if (bytes < 1e9)
        return i18n::decimal(bytes / 1e6) + " " + std::string(i18n::unit(false));
    if (bytes < 1e12)
        return i18n::decimal(bytes / 1e9) + " " + std::string(i18n::unit(true));
    return i18n::decimal(bytes / 1e12) + " TB";
}

Kind kind_of(const FileItem &item)
{
    if (item.directory)
        return Kind::folder;
    if (item.symlink)
        return Kind::link;
    const std::string name = lower(item.name);
    if (ends_with(name, ".pkg"))
        return Kind::package;
    if (ends_with(name, ".rar"))
        return Kind::archive;
    if (ends_with(name, ".exfat") || ends_with(name, ".ffpkg") || ends_with(name, ".ffpfsc"))
        return Kind::image;
    return Kind::file;
}

const char *kind_label(Kind kind)
{
    switch (kind)
    {
    case Kind::folder:
        return tr("Folder");
    case Kind::package:
        return tr("Package");
    case Kind::archive:
        return tr("Archive");
    case Kind::image:
        return tr("Game image");
    case Kind::link:
        return tr("Link");
    case Kind::file:
        break;
    }
    return tr("File");
}

const char *sort_label(SortKey key)
{
    switch (key)
    {
    case SortKey::date:
        return tr("Date");
    case SortKey::size:
        return tr("Size");
    case SortKey::type:
        return tr("Type");
    case SortKey::name:
        break;
    }
    return tr("Name");
}

void sort_entries(std::vector<FileItem> &entries, SortKey key, bool descending)
{
    std::stable_sort(entries.begin(), entries.end(),
                     [key, descending](const FileItem &a, const FileItem &b)
                     {
                         if (a.directory != b.directory)
                             return a.directory;
                         const std::string an = lower(a.name);
                         const std::string bn = lower(b.name);
                         int order = 0;
                         switch (key)
                         {
                         case SortKey::date:
                             order = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0);
                             break;
                         case SortKey::size:
                             // Folder sizes are inode sizes, not content: folders keep name order.
                             if (!a.directory)
                                 order = a.size_bytes < b.size_bytes ? -1 : (a.size_bytes > b.size_bytes ? 1 : 0);
                             break;
                         case SortKey::type:
                         {
                             const int ak = static_cast<int>(kind_of(a));
                             const int bk = static_cast<int>(kind_of(b));
                             order = ak < bk ? -1 : (ak > bk ? 1 : 0);
                             break;
                         }
                         case SortKey::name:
                             break;
                         }
                         if (order == 0)
                             order = an < bn ? -1 : (an > bn ? 1 : 0);
                         else if (descending)
                             order = -order;
                         if (key == SortKey::name && descending)
                             order = -order;
                         return order < 0;
                     });
}

bool matches(const FileItem &item, std::string_view filter)
{
    if (filter.empty())
        return true;
    return lower(item.name).find(lower(filter)) != std::string::npos;
}

// ---- operations ----

std::string canonical_status(std::string_view raw)
{
    if (raw == "waitingPassword" || raw == "waiting-password")
        return "waiting_password";
    return std::string(raw);
}

bool op_waiting_password(const FileOp &op)
{
    return op.status == "waiting_password" || op.status == "waitingPassword";
}

bool op_terminal(const FileOp &op)
{
    return op.status == "accepted" || op.status == "completed" || op.status == "failed" ||
           op.status == "cancelled" || op.status == "gone";
}

bool op_can_cancel(const FileOp &op)
{
    return (op.status == "running" || op_waiting_password(op)) && op.cancelable;
}

bool op_touches(const FileOp &op, const std::string &folder)
{
    if (folder.empty())
        return false;
    if (!op.source.empty() && (files::parent(op.source) == folder || inside(op.source, folder)))
        return true;
    if (!op.destination.empty() && (op.destination == folder || inside(folder, op.destination) ||
                                    inside(op.destination, folder)))
        return true;
    return false;
}

std::string op_title(const FileOp &op)
{
    const std::string name = op.source.empty() ? std::string() : base_name(op.source);
    if (op.type == "copy")
        return tr("Copying {name}", {{"name", name}});
    if (op.type == "move")
        return tr("Moving {name}", {{"name", name}});
    if (op.type == "delete")
        return tr("Deleting {name}", {{"name", name}});
    if (op.type == "extract")
        return tr("Extracting {name}", {{"name", name}});
    if (op.type == "pkg-install")
        return tr("Installing {name}", {{"name", name}});
    return op.type + " " + name;
}

std::string op_done_title(const FileOp &op)
{
    const std::string name = op.source.empty() ? std::string() : base_name(op.source);
    if (op.type == "copy")
        return tr("Copied {name}", {{"name", name}});
    if (op.type == "move")
        return tr("Moved {name}", {{"name", name}});
    if (op.type == "delete")
        return tr("Deleted {name}", {{"name", name}});
    if (op.type == "extract")
        return tr("Extracted {name}", {{"name", name}});
    if (op.type == "pkg-install")
        return tr("Handed {name} to the installer", {{"name", name}});
    return op.type + " " + name;
}

float op_fraction(const FileOp &op)
{
    if (op.status == "completed" || op.status == "accepted")
        return 1.0f; // a same-drive move completes with doneBytes 0
    if (op.total_bytes > 0.0)
        return static_cast<float>(std::clamp(op.done_bytes / op.total_bytes, 0.0, 1.0));
    if (op.total_items > 0)
        return std::clamp(static_cast<float>(op.done_items) / static_cast<float>(op.total_items), 0.0f, 1.0f);
    return -1.0f;
}

// ---- tracker ----

std::vector<FileOp> OpsTracker::take(const std::vector<FileOp> &ops, double now)
{
    std::vector<FileOp> finished_now;
    failures_ = 0;
    poll_error_.clear();
    polled_ = true;
    std::vector<bool> listed(ops_.size(), false);
    for (FileOp op : ops)
    {
        op.status = canonical_status(op.status);
        Tracked *have = nullptr;
        for (std::size_t i = 0; i < ops_.size(); ++i)
        {
            if (ops_[i].op.id == op.id && ops_[i].key == op.id)
            {
                have = &ops_[i];
                listed[i] = true;
                break;
            }
        }
        // Pegasus restarted and numbers from 1 again: the old row stays as history.
        if (have != nullptr && !have->active() &&
            (have->op.type != op.type || have->op.source != op.source))
        {
            have->key = have->op.id + "~" + std::to_string(++reused_);
            have = nullptr;
        }
        if (have == nullptr)
        {
            Tracked t;
            t.op = op;
            t.key = op.id;
            t.seen_at = now;
            if (!t.active())
                t.finished_at = now;
            t.sample_bytes = op.done_bytes;
            t.sample_time = now;
            ops_.push_back(std::move(t));
            listed.push_back(true);
            continue;
        }
        const bool was_active = have->active();
        // Rate: smoothed bytes per second across polls (no clock shared with the console).
        if (have->sample_bytes >= 0.0 && now > have->sample_time + 0.2 && op.done_bytes >= have->sample_bytes)
        {
            const double rate = (op.done_bytes - have->sample_bytes) / (now - have->sample_time);
            have->rate = have->rate <= 0.0 ? rate : have->rate * 0.7 + rate * 0.3;
            have->sample_bytes = op.done_bytes;
            have->sample_time = now;
        }
        have->op = op;
        if (was_active && !have->active())
        {
            have->finished_at = now;
            finished_now.push_back(have->op);
        }
    }
    // An active one Pegasus no longer lists (a restart) is not running any more.
    for (std::size_t i = 0; i < listed.size() && i < ops_.size(); ++i)
    {
        if (!listed[i] && ops_[i].active())
        {
            ops_[i].op.status = "gone";
            ops_[i].op.error = tr("No longer listed by Pegasus DL");
            ops_[i].finished_at = now;
            finished_now.push_back(ops_[i].op);
        }
    }
    // Keep the newest finished ones.
    int finished = 0;
    for (auto it = ops_.rbegin(); it != ops_.rend(); ++it)
        finished += it->active() ? 0 : 1;
    while (finished > kKeepFinished)
    {
        auto oldest = ops_.end();
        for (auto it = ops_.begin(); it != ops_.end(); ++it)
        {
            if (!it->active() && (oldest == ops_.end() || it->finished_at < oldest->finished_at))
                oldest = it;
        }
        if (oldest == ops_.end())
            break;
        ops_.erase(oldest);
        --finished;
    }
    return finished_now;
}

void OpsTracker::poll_failed(const std::string &error)
{
    ++failures_;
    poll_error_ = error;
}

bool OpsTracker::any_active() const
{
    return std::any_of(ops_.begin(), ops_.end(), [](const Tracked &t) { return t.active(); });
}

std::vector<const OpsTracker::Tracked *> OpsTracker::active() const
{
    std::vector<const Tracked *> out;
    for (auto it = ops_.rbegin(); it != ops_.rend(); ++it)
    {
        if (it->active())
            out.push_back(&*it);
    }
    return out;
}

std::vector<const OpsTracker::Tracked *> OpsTracker::finished() const
{
    std::vector<const Tracked *> out;
    for (const Tracked &t : ops_)
    {
        if (!t.active())
            out.push_back(&t);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Tracked *a, const Tracked *b) { return a->finished_at > b->finished_at; });
    return out;
}

const OpsTracker::Tracked *OpsTracker::find(const std::string &id) const
{
    for (const Tracked &t : ops_)
    {
        if (t.key == id)
            return &t;
    }
    return nullptr;
}

double OpsTracker::eta_seconds(const Tracked &tracked)
{
    const FileOp &op = tracked.op;
    if (tracked.rate <= 1.0 || op.total_bytes <= 0.0 || op.done_bytes >= op.total_bytes)
        return -1.0;
    return (op.total_bytes - op.done_bytes) / tracked.rate;
}

// ---- guards ----

DestCheck check_destination(const std::vector<std::string> &sources,
                            const std::vector<bool> &directories, const std::string &destination,
                            bool move, bool writable, double free_bytes, double need_bytes)
{
    DestCheck out;
    if (!writable)
    {
        out.verdict = Verdict::refused;
        out.reason = tr("Read-only: nothing can be put here");
        return out;
    }
    bool all_here = !sources.empty();
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const std::string &source = sources[i];
        all_here = all_here && parent(source) == destination;
        const bool directory = i < directories.size() && directories[i];
        if (directory && inside(destination, source))
        {
            out.verdict = Verdict::refused;
            out.reason = move ? tr("A folder cannot be moved into itself")
                              : tr("A folder cannot be copied into itself");
            return out;
        }
    }
    if (all_here)
    {
        out.verdict = Verdict::refused;
        out.reason = tr("Already here: choose another folder");
        return out;
    }
    if (free_bytes >= 0.0 && need_bytes > 0.0 && free_bytes < need_bytes)
    {
        out.verdict = Verdict::warn;
        out.reason = tr("Only {free} free, needs at least {need}",
                        {{"free", format_size(free_bytes)}, {"need", format_size(need_bytes)}});
    }
    return out;
}

// ---- trace ----

void Trace::add(std::string line)
{
    if (sink)
        sink(line);
    lines_.push_back(std::move(line));
    while (lines_.size() > kTraceLines)
        lines_.pop_front();
}

} // namespace orbit::files
