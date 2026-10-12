// Wings TV app - The Files tab's rules, apart from drawing: paths, drives,
// sizes, operation states and the session's operation history.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Nothing here touches the network or the screen, so all of it is tested on
// the host. The rules follow Pegasus DL's own file manager
// (recon/pegasus-file-manager.js): its shortcut filter for drives
// (/data, /mnt/extN, /mnt/usbN), its operation states (active: queued,
// running, copying, installing, waiting_password; finished: accepted,
// completed, failed, cancelled) and its cancel rule (running or
// waiting_password, unless the operation says it cannot be stopped).

#pragma once

#include "orbit/model.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::files
{

// ---- paths (always absolute, "/" is the top) ----

// "//data//x/" -> "/data/x"; "" -> "/".
std::string normalize(std::string_view path);
// "/mnt/ext0" -> "/mnt", "/data" -> "/", "/" -> "" (nothing above the top).
std::string parent(const std::string &path);
// "/mnt/ext0" -> "ext0", "/" -> "/".
std::string base_name(const std::string &path);
std::string join(const std::string &folder, const std::string &name);
// path is folder or lies below it.
bool inside(const std::string &path, const std::string &folder);

struct Crumb
{
    std::string label;
    std::string path;
};
// "/mnt/ext0/homebrew" -> "/", "mnt", "ext0", "homebrew" with their paths.
std::vector<Crumb> crumbs(const std::string &path);

// ---- drives (the quick-jump chips) ----

// Pegasus's shortcut filter: /data, /mnt/extN, /mnt/usbN (N digits).
bool quick_location(const std::string &path);
struct Drive
{
    std::string path;
    std::string label; // "Main drive", "ext0", "usb0", "Root"
    double free_bytes = -1.0; // negative: unknown
    double total_bytes = -1.0;
    bool writable = true;
};
// Main drive (/data) first and always, then each /mnt/extN and /mnt/usbN the
// locations list, then the root "/". Works with no locations at all.
std::vector<Drive> drives(const std::vector<StorageMount> &locations);
// The location holding path (longest prefix), or null.
const StorageMount *mount_of(const std::string &path, const std::vector<StorageMount> &locations);

// ---- entries ----

// "512 B", "2.0 KB", "43.0 MB", "170.8 GB", "2.0 TB": decimal units, one decimal.
std::string format_size(double bytes);

enum class Kind : std::uint8_t
{
    folder,
    file,
    package, // .pkg: installable
    archive, // .rar
    image,   // .exfat .ffpkg .ffpfsc: game images
    link,
};
Kind kind_of(const FileItem &item);
const char *kind_label(Kind kind);

enum class SortKey : std::uint8_t
{
    name,
    date,
    size,
    type,
};
const char *sort_label(SortKey key);
// Folders first, then by the key (names break ties). Descending flips the key only.
void sort_entries(std::vector<FileItem> &entries, SortKey key, bool descending);
// Case-insensitive "name contains text".
bool matches(const FileItem &item, std::string_view filter);

// ---- operations ----

// Pegasus spells the password state waiting_password; older Wings code and
// fakes used waitingPassword. Both become waiting_password.
std::string canonical_status(std::string_view raw);
bool op_waiting_password(const FileOp &op);
bool op_terminal(const FileOp &op); // accepted completed failed cancelled (and "gone")
bool op_can_cancel(const FileOp &op); // running / waiting_password, and cancelable
// The operation changed what folder shows: its source's folder, its
// destination, or a removed path inside it (Pegasus operationTouchesPath).
bool op_touches(const FileOp &op, const std::string &folder);
std::string op_title(const FileOp &op);      // "Moving PPSA02015.exfat"
std::string op_done_title(const FileOp &op); // "Moved PPSA02015.exfat"
// 0..1 from bytes, else items; negative when Pegasus gives neither.
float op_fraction(const FileOp &op);

// What Wings has seen of Pegasus's operations this session. Pegasus keeps its
// list in memory, so finished ones may vanish (a restart); Wings keeps them,
// newest first, at most kKeepFinished.
class OpsTracker
{
  public:
    static constexpr int kKeepFinished = 10;
    struct Tracked
    {
        FileOp op;
        std::string key;          // op id, made unique again when Pegasus reuses one
        double seen_at = 0.0;     // first seen (seconds, the caller's clock)
        double finished_at = -1.0; // when it was first seen finished
        double rate = 0.0;        // bytes per second, smoothed across polls
        double sample_bytes = -1.0;
        double sample_time = 0.0;
        bool active() const
        {
            return !op_terminal(op);
        }
    };

    // A successful poll (or a mutating call's list). Returns the operations
    // that went from active to finished with it.
    std::vector<FileOp> take(const std::vector<FileOp> &ops, double now);
    // A poll that failed: counted until the next success.
    void poll_failed(const std::string &error);
    int failures() const
    {
        return failures_;
    }
    const std::string &poll_error() const
    {
        return poll_error_;
    }
    bool any_active() const;
    // Newest first.
    std::vector<const Tracked *> active() const;
    std::vector<const Tracked *> finished() const;
    const Tracked *find(const std::string &id) const;
    // Seconds left from the smoothed rate; negative when unknown.
    static double eta_seconds(const Tracked &tracked);
    bool polled() const
    {
        return polled_;
    }

  private:
    std::vector<Tracked> ops_; // oldest first
    int failures_ = 0;
    std::string poll_error_;
    bool polled_ = false;
    int reused_ = 0;
};

// ---- guards for a destination ----

enum class Verdict : std::uint8_t
{
    ok,
    warn,    // allowed, with a warning (free space)
    refused, // the confirm row is disabled, with the reason
};
struct DestCheck
{
    Verdict verdict = Verdict::ok;
    std::string reason;
};
// sources: full paths; directories: which of them are folders. free_bytes and
// need_bytes negative when unknown.
DestCheck check_destination(const std::vector<std::string> &sources,
                            const std::vector<bool> &directories, const std::string &destination,
                            bool move, bool writable, double free_bytes, double need_bytes);

// ---- diagnostics ----

// Lines for the title log (the console build hands them to sys::log with the
// [WINGS-FILES] tag) and the last few kept for tests and the Logs tab.
class Trace
{
  public:
    std::function<void(const std::string &)> sink;
    void add(std::string line);
    const std::deque<std::string> &lines() const
    {
        return lines_;
    }

  private:
    std::deque<std::string> lines_;
};

} // namespace orbit::files
