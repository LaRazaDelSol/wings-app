// Wings TV app - The data the screens show: Wings' own types, no JSON.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from Orbit Store's model.hpp (Orbit Store, GPL-3.0-or-later). The
// UI only ever sees these types. Whatever supplies them (the fake backend in
// the PC preview, the Pegasus DL adapter on the console) lives behind
// orbit::Backend.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit
{

// What Pegasus DL says about itself (GET /api/health) and how Wings sees it.
struct PegasusInfo
{
    std::string version; // "1.11.0"
    int port = 6970;
    bool download_only = false;
    bool native_pkg_handoff = true;
    std::string adapter; // which adapter answered ("fake", "probe", "pegasus-v1")
    bool supported = true; // false: a Pegasus major version Wings has no adapter for
    std::string compat;    // "tested", "untested version", "unsupported", for the About rows
    std::vector<std::string> notes; // features Wings switched off, "feature: reason"
    // Every capability the probe looked at: (label, available). Empty for the fake.
    std::vector<std::pair<std::string, bool>> features;
};

// The settings the UI shows (GET /api/settings); tokens are never returned.
// One debrid account as Pegasus reports it (the token itself never comes back).
struct DebridView
{
    bool configured = false; // a token is stored
    bool valid = false;      // Pegasus checked it and the service accepted it
    bool premium = false;
    std::string account_type;
    std::int64_t hosters = 0;
    std::int64_t domains = 0;
    std::int64_t last_checked_at = 0; // unix seconds, 0 never
    std::string error;
    bool ready() const
    {
        return configured && valid && domains > 0;
    }
};

struct SettingsView
{
    std::string download_root;
    int connections = 0;
    bool real_debrid_ready = false; // configured, valid, and lists hosters
    bool torbox_ready = false;
    bool real_debrid_configured = false;
    bool torbox_configured = false;
    bool dns_helper_running = false;
    DebridView real_debrid;
    DebridView torbox;
    bool dns_present = false; // the reply described the DNS helper
    bool dns_available = false;
    bool dns_managed = false;
    std::string dns_error;
    std::string dns_primary; // the DNS address Pegasus recommends
    bool download_only = false;
    bool queue_persists = false;
    bool range_resume = false;
    // The download providers Pegasus resolves by itself (names; read-only for Wings).
    std::vector<std::string> providers;
};

// What POST /api/settings may carry; only the flagged fields are sent.
struct SettingsPatch
{
    bool has_root = false;
    std::string root;
    bool has_connections = false;
    int connections = 0;
    bool has_real_debrid = false;
    std::string real_debrid_token; // "" clears
    bool has_torbox = false;
    std::string torbox_token;
};

// A mounted storage (GET /api/mounts, /api/files/locations).
struct StorageMount
{
    std::string path;
    std::string name;
    std::string type;
    bool writable = true;
    double total_bytes = 0.0;
    double free_bytes = 0.0;
};

// An entry of a folder listing (browse or file manager).
struct FileItem
{
    std::string name;
    std::string path; // empty in browse listings: join with the folder
    bool directory = false;
    bool symlink = false;
    bool writable = true;
    double size_bytes = 0.0;
    std::int64_t modified = 0;
};

struct FolderListing
{
    std::string path;
    bool writable = true;
    std::vector<FileItem> entries;
};

// A file-manager background operation (copy, move, delete, extract, pkg install).
struct FileOp
{
    std::string id;
    std::string type;
    // queued starting running copying installing waiting_password (as Pegasus
    // spells it) accepted completed failed cancelled
    std::string status;
    std::string source;
    std::string destination;
    std::string current;
    std::string error;
    std::int64_t total_items = 0;
    std::int64_t done_items = 0;
    double total_bytes = 0.0;
    double done_bytes = 0.0;
    bool cancelable = true;
    bool cancel_requested = false;
    // accepted is final too: it is where a pkg-install ends (Pegasus fm.js).
    bool active() const
    {
        return status != "completed" && status != "failed" && status != "cancelled" &&
               status != "accepted" && status != "gone";
    }
};

// APR Emu on one installed title.
struct AprView
{
    bool present = false;
    std::string state;        // update, current, unavailable, unsupported, missing, unknown
    std::string version_text; // "1.4.2 release", or empty
    std::string latest;
    bool update_available = false;
    bool can_update = false;
    bool can_reinstall = false;
    bool remote_available = true;
    bool remount_required = false;
    std::string reason;
    std::string manifest_error;
};

// One APR Emu build the player may reinstall.
struct AprVersionItem
{
    std::string id;
    std::string label;
};

// A background job that Pegasus runs for an installed title: APR Emu update, game delete.
struct AdminJob
{
    std::string job_id;
    std::string title_id;
    std::string status; // starting running ... completed failed
    int progress = 0;   // 0..100
    std::string detail;
    std::string error;
    std::int64_t files_deleted = 0;
    bool done() const
    {
        return status == "completed" || status == "failed";
    }
};

// One catalogue entry from a source the user added to Pegasus. The list keeps
// only what the Store grid and search need; links come with PackageDetail.
struct Package
{
    std::string id; // Pegasus packageId, as text
    std::string title;
    std::string title_id; // PPSA12345 / CUSA12345, often empty
    std::string source_name;
    std::string source_id;
    std::string version;
    std::string poster_url; // https CDN link; the art module decides what to do with it
    std::string description; // plain text, shortened
    double size_bytes = 0.0;
    int link_count = 0;
    bool has_pkg = false;       // one of its links is a .pkg: handed to the PS5's Downloads
    bool has_automated = false; // one of its links is a provider Pegasus resolves itself
    bool has_supported = false; // one of its links is a supported browser provider (captured)
};

// What activating a download link does (wings::logic::decideLinkAction).
enum class LinkRoute : std::uint8_t
{
    native_pkg,   // .pkg: handed to the PS5's own Downloads
    debrid,       // exactly one ready debrid service takes it
    choose_debrid, // several do: ask which
    automated,    // Pegasus resolves it by itself (may open the PS5 browser for a captcha)
    capture,      // opened in the PS5 browser; Pegasus captures the download URL
    browser_only, // opened in the PS5 browser, nothing captured
    blocked,      // a provider Wings knows it cannot use
    missing,      // no usable URL
};

struct PackageLink
{
    int index = 0;
    std::string name;
    std::string host; // "mediafire.com"; empty when unknown
    LinkRoute route = LinkRoute::capture;
    bool needs_browser = false; // the PS5 browser opens (a check to complete there)
    std::string provider;              // "MediaFire", empty when not a known provider
    std::vector<std::string> services; // "Real-Debrid", "TorBox": the debrid services that take it
    std::string url; // the catalogue URL; only to show each URL once in a merged game (game_page)
};

// How well a link is supported, best first (the order the package page sorts by).
// Derived from the route Pegasus' own rules chose (wings::logic::decideLinkAction):
// debrid is only reported when a service is configured, valid and lists the host.
enum class LinkTier : std::uint8_t
{
    native_pkg, // green: a .pkg handed to the PS5's Downloads
    automated,  // green: a provider Pegasus resolves by itself
    supported,  // green: a supported provider opened in the PS5 browser and captured
    debrid,     // yellow: a debrid service the player has set up takes it
    unknown,    // neutral: not a known provider, manual capture
    unsupported, // red: not supported (or no usable address)
};

struct PackageDetail
{
    Package package;
    std::vector<PackageLink> links;
};

// Small description of a Job's archive choice (waitingChoice).
struct ArchiveChoice
{
    int index = 0;
    std::string name;
};

// One entry of Pegasus' download queue.
struct Job
{
    std::string id;
    std::string name;
    std::string title_id;
    std::string source_name;
    // queued starting downloading merging extracting finalizing waitingPassword
    // waitingChoice pausing cancelling paused completed failed cancelled
    std::string status;
    std::string phase;  // download merge extract finish, or empty
    std::string detail; // Pegasus' own progress text
    std::string error;
    std::string entry; // the archive member being extracted
    double received = 0.0;
    double total = 0.0;
    double speed = 0.0;        // bytes per second
    double eta_seconds = -1.0; // negative: unknown
    double progress = -1.0;    // 0..1 when Pegasus reports one; negative: derive from bytes
    int attempts = 0;
    int max_attempts = 0;
    std::vector<ArchiveChoice> choices; // when status is waitingChoice
    // A .pkg Wings downloaded to disk and installs itself (pkg_install.hpp), on a completed job:
    // "installing", "installed", "install_failed", or empty. install_text is the player text.
    std::string install_state;
    std::string install_text;
};

// A game installed on the console (GET /appdb).
struct InstalledGame
{
    std::string title_id;
    std::string title;
    std::string version;
    std::string path;
    double size_bytes = 0.0;
    bool apr_emu = false;
    std::string storage; // "Folder", "Image", or empty
    std::string firmware; // "FW 9.60", or empty
    AprView apr;
};

// A package source added in Pegasus.
struct Source
{
    std::string id;
    std::string name;
    std::string kind; // "url", "file", "default"
    std::string url;
    bool enabled = true;
    bool managed = false;
    bool deletable = true;
    bool is_new = false;
    int package_count = 0;
    std::int64_t last_refresh_at = 0; // unix seconds, 0 when never
    std::string status;               // "ok", "refreshing", "error", ...
};

// One of the sources Pegasus offers to add (GET /api/default-sources).
struct DefaultSource
{
    std::string id;
    std::string name;
    std::string description;
    bool default_enabled = false; // pre-selected in the onboarding list
    bool configured = false;      // already added
    bool enabled = false;
    bool is_new = false;
    int package_count = 0;
};

struct DefaultSources
{
    bool available = false;
    std::string status;
    std::string error;
    std::int64_t revision = 0;
    bool onboarding_required = false;
    bool has_new = false;
    std::vector<DefaultSource> sources;
};

struct LogLine
{
    std::int64_t seq = 0;
    std::string level; // info, warn, error
    std::string component;
    std::string message;
};

// What a call that can change the queue reports back.
struct Dispatch
{
    bool has_jobs = false; // the reply carried the queue
    std::vector<Job> jobs;
    bool queued = false; // something was added to Pegasus' own queue
    std::string toast;   // short confirmation for the player
    bool native = false; // handed to the PS5's own Downloads
    std::string native_title;
    std::string native_message;
};

struct DebridFile
{
    int index = 0;
    std::string name;
    double size_bytes = 0.0;
};

// A browser capture session (a captcha in the PS5 browser, or an automated provider).
struct CaptureView
{
    bool active = false;
    std::string session_id;
    // priming watching queueing completed failed cancelled
    std::string state;
    std::string message; // Pegasus' own wording, or ours
    bool automated = false;
    bool requires_browser = true;
    std::string title; // what is being downloaded
    std::string host;
    Dispatch result; // when completed
    bool terminal() const
    {
        return state == "completed" || state == "failed" || state == "cancelled";
    }
};

// The outcome of "download this link".
struct LinkResult
{
    enum class Kind : std::uint8_t
    {
        dispatched,    // queued or handed to the PS5; see dispatch
        capture,       // a browser capture session started; see Snapshot::capture
        select_files,  // the debrid service wants the player to pick files
        browser_opened, // opened in the PS5 browser, nothing is captured
        blocked,
    } kind = Kind::dispatched;
    Dispatch dispatch;
    std::string selection_id; // select_files
    std::string service;      // debrid service id, select_files
    std::vector<DebridFile> files;
    CaptureView capture; // capture: the session that just started
    std::string message;
};

// A direct link checked with POST /api/resolve/probe.
struct Probe
{
    std::string url;
    std::string final_url;
    std::string file_name;
    std::string content_type;
    double length = -1.0;
    std::string rejection; // empty when the link can be queued
};

// ---- job helpers ----
bool job_active(const Job &job);   // still moving or waiting its turn
bool job_finished(const Job &job); // completed, failed or cancelled
bool job_running(const Job &job);  // actually transferring or working
const char *status_label(std::string_view status);
// The chip for a completed .pkg Wings installs itself (Job::install_state); nullptr when there is none.
const char *install_label(std::string_view install_state);
// The fraction to draw (0..1), or negative when nothing is known.
double job_fraction(const Job &job);

const char *route_label(LinkRoute route);

// ---- link tiers ----
LinkTier link_tier(const PackageLink &link);
// "PKG to PS5", "Supported", "Debrid", "Unknown", "Not supported" (English keys).
const char *tier_label(LinkTier tier);
bool tier_is_green(LinkTier tier);
// Stable: the original order is kept inside a tier.
void sort_links_by_tier(std::vector<PackageLink> &links);
// The best tier any link reaches; LinkTier::unsupported for an empty list.
LinkTier best_tier(const std::vector<PackageLink> &links);

// A package's best tier for the grid, as one byte (Backend::package_tiers): the
// tier in bits 0..2 and, in bit 3, whether at least one link goes through a
// debrid service the player has set up.
constexpr std::uint8_t pack_tier(LinkTier tier, bool via_debrid)
{
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(tier) | (via_debrid ? 8u : 0u));
}
constexpr LinkTier packed_tier(std::uint8_t packed)
{
    return static_cast<LinkTier>(packed & 7u);
}
constexpr bool packed_via_debrid(std::uint8_t packed)
{
    return (packed & 8u) != 0;
}

// ---- formatting ----
std::string format_bytes(double bytes); // "12.4 GB", "512.0 MB"
std::string format_speed(double bytes_per_second);
std::string format_eta(double seconds); // "3 min", "1 h 05 min", ""
std::string format_age(double unix_time, double now);
bool contains_folded(std::string_view haystack, std::string_view needle);

} // namespace orbit
