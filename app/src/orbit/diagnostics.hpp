// Wings TV app - The short human-readable diagnostics report.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the first console run nobody can read a debugger. main.cpp fills this
// plain struct from the art service, the heap and the Store and writes the text
// to /data/wings/diagnostics.txt (readable in Pegasus DL's file manager) and
// /download0/wings/diagnostics.txt (the title's own storage), and the key lines
// also go to the title log with the [WINGS-ART] tag. Nothing here touches the
// network or a file: it is a formatter, tested on the host.

#pragma once

#include "core/touch.hpp"
#include "orbit/thread.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace orbit
{

struct DiagnosticsInput
{
    // Identity
    std::string app_version;
    std::string title_id;
    // Time
    std::int64_t unix_time = 0; // 0 when the clock cannot be read
    double uptime_s = 0.0;
    // Pegasus DL
    std::string link; // "online", "waiting", "connecting"
    std::string endpoint;
    std::string pegasus_version;
    int catalogue_packages = -1; // -1: not loaded
    int poster_urls = 0;
    // Cover art
    bool art_present = false; // the module is in this build
    std::string art_phase;    // "ready", "off (DNS-over-HTTPS is off)", ...
    std::string art_detail;
    std::string dns_mode; // "off", "doh"
    bool doh_enabled = false;
    int ca_certs = 0;
    std::string ca_source;
    int rand_status = -1; // OpenSSL RAND_status(); -1 unknown
    std::string cache_dir;
    bool cache_dir_writable = false;
    bool disk_ok = false;
    std::uint64_t cache_entries = 0;
    std::uint64_t cache_bytes = 0;
    bool clock_suspect = false; // the service saw "certificate not yet valid / expired"
    int queued = 0;
    int running = 0;
    std::uint64_t requests = 0, disk_hits = 0, net_ok = 0, net_bytes = 0;
    std::uint64_t fail_transient = 0, fail_permanent = 0, refused_by_guard = 0;
    // Touchpad (what the pad layer saw; verifies the sample layout)
    bool touch_known = false; // the pad layer reports touch statistics
    hui::TouchStats touch;
    // Memory
    std::uint64_t heap_live = 0, heap_peak = 0;
    // Where this report is written
    std::vector<std::string> written_to;
    // Recent warnings and errors from the art service, oldest first.
    std::vector<std::string> recent_problems;
};

// "2026-10-11 14:03:07", UTC; "unknown" for t <= 0.
std::string format_utc(std::int64_t unix_time);
// True when the console date lies in a plausible window (certificates are
// checked against it, so a reset clock breaks every HTTPS cover).
bool clock_plausible(std::int64_t unix_time);
std::string format_diagnostics(const DiagnosticsInput &in);

// Keeps the last few problem lines (thread-safe: the art workers log).
class ProblemLog
{
  public:
    void add(const std::string &line);
    std::vector<std::string> snapshot() const;

  private:
    mutable Mutex mutex_;
    std::deque<std::string> lines_;
};

} // namespace orbit
