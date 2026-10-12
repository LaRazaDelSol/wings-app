// Wings TV app - The short human-readable diagnostics report.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/diagnostics.hpp"

#include <cstdio>
#include <functional>

namespace orbit
{

namespace
{

constexpr std::int64_t kEarliestPlausible = 1735689600; // 2025-01-01
constexpr std::int64_t kLatestPlausible = 4102444800;   // 2100-01-01

std::string mib(std::uint64_t bytes)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f MiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

const char *yes(bool value)
{
    return value ? "yes" : "NO";
}

// The Touchpad block: min/max prove the offsets and the pad range on the console.
void touchpad_block(const DiagnosticsInput &in, const std::function<void(const std::string &)> &line)
{
    const hui::TouchStats &t = in.touch;
    line("Touchpad");
    if (!in.touch_known)
    {
        line("  not reported by this build");
        line("");
        return;
    }
    if (!t.seen())
    {
        line("  no touch seen yet: put a finger on the touchpad, then wait for the next rewrite");
        if (t.bad != 0)
            line("  samples with an impossible touch count: " + std::to_string(t.bad) +
                 "   <- the touch offsets are probably wrong");
        line("");
        return;
    }
    line("  samples with a finger: " + std::to_string(t.samples) + ", contacts " +
         std::to_string(t.contacts) + ", most fingers at once " + std::to_string(t.max_fingers));
    line("  x range seen:    " + std::to_string(t.min_x) + " .. " + std::to_string(t.max_x) +
         "   (expected within 0 .. " + std::to_string(hui::kTouchWidth - 1) + ")");
    line("  y range seen:    " + std::to_string(t.min_y) + " .. " + std::to_string(t.max_y) +
         "   (expected within 0 .. " + std::to_string(hui::kTouchHeight - 1) + ")");
    std::string ids;
    for (int id = 0; id < 64; ++id)
        if ((t.id_mask >> id) & 1u)
            ids += (ids.empty() ? "" : " ") + std::to_string(id);
    line("  ids seen:        " + std::to_string(t.distinct_ids()) + " (" + ids + "), last " +
         std::to_string(t.last_id) + ", changes " + std::to_string(t.id_changes));
    line("  gestures:        " + std::to_string(t.taps) + " taps, " + std::to_string(t.swipes_left) +
         " swipes left, " + std::to_string(t.swipes_right) + " right, " +
         std::to_string(t.scroll_frames) + " scroll frames, " + std::to_string(t.clicks_on_touch) +
         " clicks with a finger down");
    const bool in_range = t.max_x < 2 * hui::kTouchWidth && t.max_y < 2 * hui::kTouchHeight;
    line(std::string("  layout looks right: ") + yes(in_range && t.bad == 0) +
         (in_range && t.bad == 0 ? "" : "   <- values outside the expected pad range: check the offsets"));
    if (t.bad != 0)
        line("  samples with an impossible touch count: " + std::to_string(t.bad));
    line("");
}

} // namespace

std::string format_utc(std::int64_t t)
{
    if (t <= 0)
        return "unknown";
    // Howard Hinnant's civil_from_days; no libc time functions (the console
    // build replaces some of them).
    std::int64_t days = t / 86400;
    const std::int64_t rem = t % 86400;
    days += 719468;
    const std::int64_t era = days / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        ++y;
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", static_cast<int>(y),
                  static_cast<int>(m), static_cast<int>(d), static_cast<int>(rem / 3600),
                  static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60));
    return buffer;
}

bool clock_plausible(std::int64_t unix_time)
{
    return unix_time >= kEarliestPlausible && unix_time < kLatestPlausible;
}

std::string format_diagnostics(const DiagnosticsInput &in)
{
    std::string out;
    const auto line = [&](const std::string &text) { out += text + "\n"; };
    line("Wings diagnostics");
    line("=================");
    line("Written by the app so a first console run can be understood without a debugger.");
    line("Safe to share: no account data, tokens or download links. Cover image addresses");
    line("(public CDN links) can appear in the problem list below.");
    line("");
    line("App");
    line("  version:         " + (in.app_version.empty() ? std::string("unknown") : in.app_version));
    if (!in.app_build.empty())
        line("  build:           " + in.app_build);
    line("  title id:        " + (in.title_id.empty() ? std::string("unknown") : in.title_id));
    line("  uptime:          " + std::to_string(static_cast<long long>(in.uptime_s)) + " s");
    line("");
    line("Console clock");
    line("  now (UTC):       " + format_utc(in.unix_time));
    const bool plausible = clock_plausible(in.unix_time);
    line(std::string("  looks right:     ") + yes(plausible) +
         (plausible ? "" : "   <- a wrong date makes every HTTPS certificate look expired"));
    line("");
    line("Pegasus DL");
    line("  state:           " + in.link);
    line("  address:         " + in.endpoint);
    line("  version:         " + (in.pegasus_version.empty() ? std::string("-") : in.pegasus_version));
    line("  catalogue:       " +
         (in.catalogue_packages < 0
              ? std::string("not loaded")
              : std::to_string(in.catalogue_packages) + " packages, " +
                    std::to_string(in.poster_urls) + " with an https cover URL"));
    line("");
    line("Cover art");
    if (!in.art_present)
    {
        line("  this build has no HTTPS cover art module");
    }
    else
    {
        line("  state:           " + in.art_phase);
        line("  message:         " + in.art_detail);
        line("  DNS mode:        " + in.dns_mode +
             (in.doh_enabled ? "  (DNS-over-HTTPS is ON)"
                             : "  (DNS-over-HTTPS is OFF: nothing is downloaded; turned off in Settings)"));
        line("  CA bundle:       " + std::to_string(in.ca_certs) + " certificates" +
             (in.ca_source.empty() ? std::string() : " from " + in.ca_source));
        line("  RAND_status:     " +
             (in.rand_status < 0 ? std::string("unknown") : std::to_string(in.rand_status)) +
             (in.rand_status == 1 ? "  (ok)"
                                  : (in.rand_status < 0 ? "" : "   <- must be 1 for HTTPS")));
        line("  cache folder:    " + in.cache_dir);
        line(std::string("  folder writable: ") + yes(in.cache_dir_writable));
        line(std::string("  disk cache on:   ") + yes(in.disk_ok) +
             (in.disk_ok ? "" : "   <- covers are kept in memory only"));
        line("  cached covers:   " + std::to_string(in.cache_entries) + " (" + mib(in.cache_bytes) +
             ")");
        line(std::string("  clock suspect:   ") +
             (in.clock_suspect ? "YES: certificates were rejected as expired or not yet valid"
                               : "no"));
        line("  workers:         " + std::to_string(in.running) + " running, " +
             std::to_string(in.queued) + " queued");
        line("  requests:        " + std::to_string(in.requests) + " (disk hits " +
             std::to_string(in.disk_hits) + ", downloaded " + std::to_string(in.net_ok) + " = " +
             mib(in.net_bytes) + ")");
        line("  failures:        " + std::to_string(in.fail_transient) + " temporary, " +
             std::to_string(in.fail_permanent) + " permanent, " +
             std::to_string(in.refused_by_guard) + " refused as too large");
        if (!in.recent_problems.empty())
        {
            line("  recent problems:");
            for (const std::string &problem : in.recent_problems)
                line("    " + problem);
        }
    }
    line("");
    touchpad_block(in, line);
    line("Memory");
    line("  app heap live:   " + mib(in.heap_live) + "   peak " + mib(in.heap_peak) +
         "   (limit 128 MiB)");
    line("");
    line("This file was written to");
    if (in.written_to.empty())
        line("  (nowhere yet)");
    for (const std::string &path : in.written_to)
        line("  " + path);
    line("The title log is download0/hui/dev/app.log in the title storage; lines tagged "
         "[WINGS-ART] come from the art module.");
    return out;
}

void ProblemLog::add(const std::string &text)
{
    Lock lock(mutex_);
    lines_.push_back(text.size() > 220 ? text.substr(0, 220) : text);
    while (lines_.size() > 12)
        lines_.pop_front();
}

std::vector<std::string> ProblemLog::snapshot() const
{
    Lock lock(mutex_);
    return std::vector<std::string>(lines_.begin(), lines_.end());
}

} // namespace orbit
