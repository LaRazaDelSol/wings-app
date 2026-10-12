// Wings TV app - Formatting and status wording.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// format_bytes and format_age are from Orbit Store's model.cpp (GPL-3.0-or-later).

#include "orbit/model.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <cstdio>

namespace orbit
{

bool job_active(const Job &job)
{
    return !job_finished(job);
}

bool job_finished(const Job &job)
{
    return job.status == "completed" || job.status == "failed" || job.status == "cancelled";
}

bool job_running(const Job &job)
{
    return job.status == "downloading" || job.status == "merging" || job.status == "extracting" ||
           job.status == "finalizing" || job.status == "starting";
}

double job_fraction(const Job &job)
{
    if (job.progress >= 0.0)
        return job.progress > 1.0 ? 1.0 : job.progress;
    if (job.total > 0.0)
    {
        const double f = job.received / job.total;
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    }
    return -1.0;
}

const char *route_label(LinkRoute route)
{
    switch (route)
    {
    case LinkRoute::native_pkg:
        return tr("To PS5 Downloads");
    case LinkRoute::debrid:
        return tr("Debrid");
    case LinkRoute::choose_debrid:
        return tr("Debrid");
    case LinkRoute::automated:
        return tr("Automatic");
    case LinkRoute::capture:
        return tr("Browser capture");
    case LinkRoute::browser_only:
        return tr("Browser only");
    case LinkRoute::blocked:
        return tr("Not supported");
    case LinkRoute::missing:
        return tr("No link");
    }
    return "";
}

LinkTier link_tier(const PackageLink &link)
{
    switch (link.route)
    {
    case LinkRoute::native_pkg:
        return LinkTier::native_pkg;
    case LinkRoute::automated:
        return LinkTier::automated;
    case LinkRoute::debrid:
    case LinkRoute::choose_debrid:
        return LinkTier::debrid;
    case LinkRoute::capture:
        // A known provider in Pegasus' direct-browser table is "Supported";
        // anything else is a manual capture.
        return link.provider.empty() ? LinkTier::unknown : LinkTier::supported;
    case LinkRoute::browser_only:
    case LinkRoute::blocked:
    case LinkRoute::missing:
        break;
    }
    return LinkTier::unsupported;
}

const char *tier_label(LinkTier tier)
{
    switch (tier)
    {
    case LinkTier::native_pkg:
        return tr("PKG to PS5");
    case LinkTier::automated:
    case LinkTier::supported:
        return tr("Supported");
    case LinkTier::debrid:
        return tr("Debrid");
    case LinkTier::unknown:
        return tr("Unknown");
    case LinkTier::unsupported:
        break;
    }
    return tr("Not supported");
}

bool tier_is_green(LinkTier tier)
{
    return tier == LinkTier::native_pkg || tier == LinkTier::automated ||
           tier == LinkTier::supported;
}

void sort_links_by_tier(std::vector<PackageLink> &links)
{
    std::stable_sort(links.begin(), links.end(),
                     [](const PackageLink &a, const PackageLink &b)
                     { return link_tier(a) < link_tier(b); });
}

LinkTier best_tier(const std::vector<PackageLink> &links)
{
    LinkTier best = LinkTier::unsupported;
    for (const PackageLink &link : links)
        best = std::min(best, link_tier(link));
    return best;
}

const char *status_label(std::string_view status)
{
    if (status == "queued")
        return tr("Queued");
    if (status == "starting")
        return tr("Starting");
    if (status == "downloading")
        return tr("Downloading");
    if (status == "merging")
        return tr("Merging");
    if (status == "extracting")
        return tr("Extracting");
    if (status == "finalizing")
        return tr("Finalizing");
    if (status == "waitingPassword")
        return tr("Needs a password");
    if (status == "waitingChoice")
        return tr("Needs a choice");
    if (status == "pausing")
        return tr("Pausing");
    if (status == "cancelling")
        return tr("Cancelling");
    if (status == "paused")
        return tr("Paused");
    if (status == "completed")
        return tr("Completed");
    if (status == "failed")
        return tr("Failed");
    if (status == "cancelled")
        return tr("Cancelled");
    return tr("Unknown");
}

std::string format_bytes(double bytes)
{
    // 49.4 GB in English, 49,4 GB in German.
    const bool giga = bytes >= 1e9;
    return i18n::decimal(giga ? bytes / 1e9 : bytes / 1e6) + " " + std::string(i18n::unit(giga));
}

std::string format_speed(double bytes_per_second)
{
    if (bytes_per_second <= 0.0)
        return {};
    return format_bytes(bytes_per_second) + "/s";
}

std::string format_eta(double seconds)
{
    if (seconds < 0.0)
        return {};
    const long total = static_cast<long>(seconds);
    char text[32];
    if (total < 60)
        std::snprintf(text, sizeof(text), "%ld s", total);
    else if (total < 3600)
        std::snprintf(text, sizeof(text), "%ld min", total / 60);
    else
        std::snprintf(text, sizeof(text), "%ld h %02ld min", total / 3600, (total % 3600) / 60);
    return text;
}

std::string format_age(double unix_time, double now)
{
    if (unix_time <= 0.0)
        return tr("never");
    const double age = std::max(0.0, now - unix_time);
    if (age < 60.0)
        return tr("just now");
    if (age < 3600.0)
        return trn(static_cast<long>(age / 60.0), "{count} min ago", "{count} min ago");
    if (age < 86400.0)
        return trn(static_cast<long>(age / 3600.0), "{count} h ago", "{count} h ago");
    return trn(static_cast<long>(age / 86400.0), "{count} day ago", "{count} days ago");
}

bool contains_folded(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        std::size_t j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

} // namespace orbit
