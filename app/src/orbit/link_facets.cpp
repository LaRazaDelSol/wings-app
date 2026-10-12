// Wings TV app - Filters and facets for a game page's mirrors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/link_facets.hpp"

#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace orbit
{
namespace
{

const LinkMeta &meta_of(const GamePage &page, int mirror)
{
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    return page.links[static_cast<std::size_t>(m.links.front())].meta;
}

const std::string &source_of(const GamePage &page, int mirror)
{
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    const GameLink &l = page.links[static_cast<std::size_t>(m.links.front())];
    return page.packages[static_cast<std::size_t>(l.package)].source_name;
}

void count(std::vector<FacetValue> *values, const std::string &value)
{
    for (FacetValue &v : *values)
    {
        if (v.value == value)
        {
            ++v.count;
            return;
        }
    }
    values->push_back(FacetValue{value, 1});
}

std::string region_value(const LinkMeta &m)
{
    return m.region.empty() ? "?" : m.region;
}

std::string format_value(const LinkMeta &m)
{
    return m.format == LinkFormat::none ? "?" : format_label(m.format);
}

bool host_hidden(const std::string &host, const LinkPrefs &p)
{
    return std::find(p.hidden_hosts.begin(), p.hidden_hosts.end(), host) != p.hidden_hosts.end();
}

// Sort by a fixed list of known values, "?" last.
void order_by(std::vector<FacetValue> *values, std::initializer_list<std::string_view> known)
{
    const auto rank = [&](const std::string &v)
    {
        if (v == "?")
            return 1000;
        int i = 0;
        for (const std::string_view k : known)
        {
            if (v == k)
                return i;
            ++i;
        }
        return 500;
    };
    std::stable_sort(values->begin(), values->end(),
                     [&](const FacetValue &a, const FacetValue &b) { return rank(a.value) < rank(b.value); });
}

std::vector<int> display_order(const GamePage &page)
{
    std::vector<int> out;
    for (const std::vector<int> &section : page.sections)
        for (const int r : section)
            for (const int m : page.releases[static_cast<std::size_t>(r)].mirrors)
                out.push_back(m);
    return out;
}

} // namespace

bool no_browser_tier(LinkTier tier)
{
    return tier == LinkTier::native_pkg || tier == LinkTier::debrid || tier == LinkTier::automated;
}

bool Facets::show_regions() const
{
    return regions.size() >= 2;
}

bool Facets::show_formats() const
{
    int known = 0;
    for (const FacetValue &v : formats)
        known += v.value != "?" ? 1 : 0;
    return known >= 2;
}

bool Facets::show_firmware() const
{
    return fw_too_new > 0 || (fw_unknown > 0 && fw_runs > 0);
}

bool Facets::show_no_browser() const
{
    return no_browser > 0 && no_browser < total;
}

bool Facets::show_single_file() const
{
    return single_file > 0 && single_file < total;
}

bool MirrorFilter::active() const
{
    return runs_on_my_fw || no_browser || single_file || !region.empty() || !format.empty() || !host.empty() ||
           !source.empty() || section >= 0;
}

Facets page_facets(const GamePage &page, const LinkPrefs &prefs)
{
    Facets f;
    for (const int mi : display_order(page))
    {
        const Mirror &m = page.mirrors[static_cast<std::size_t>(mi)];
        if (host_hidden(m.host, prefs))
            continue;
        const LinkMeta &meta = meta_of(page, mi);
        ++f.total;
        count(&f.regions, region_value(meta));
        count(&f.formats, format_value(meta));
        count(&f.hosts, m.host);
        count(&f.sources, source_of(page, mi));
        count(&f.sections, std::to_string(static_cast<int>(section_of(meta.kind))));
        if (!prefs.firmware.empty())
        {
            switch (fw_fit(meta.min_fw(), prefs.firmware))
            {
            case FwFit::runs:
                ++f.fw_runs;
                break;
            case FwFit::unknown:
                ++f.fw_unknown;
                break;
            case FwFit::too_new:
                ++f.fw_too_new;
                break;
            }
        }
        f.no_browser += no_browser_tier(m.tier) ? 1 : 0;
        f.single_file += m.multipart() ? 0 : 1;
    }
    order_by(&f.regions, {"USA", "EUR", "JPN", "ASIA", "HK", "KOR"});
    order_by(&f.formats, {"exFAT", "FPKG", "PKG", "Dump", "FFPFS"});
    order_by(&f.sections, {"0", "1", "2", "3"});
    std::stable_sort(f.hosts.begin(), f.hosts.end(),
                     [](const FacetValue &a, const FacetValue &b) { return a.count > b.count; });
    return f;
}

bool mirror_passes(const GamePage &page, int mirror, const MirrorFilter &filter, const LinkPrefs &prefs)
{
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    const LinkMeta &meta = meta_of(page, mirror);
    if (host_hidden(m.host, prefs))
        return false;
    if (filter.runs_on_my_fw && fw_fit(meta.min_fw(), prefs.firmware) == FwFit::too_new)
        return false;
    if (filter.no_browser && !no_browser_tier(m.tier))
        return false;
    if (filter.single_file && m.multipart())
        return false;
    if (!filter.region.empty() && region_value(meta) != filter.region)
        return false;
    if (!filter.format.empty() && format_value(meta) != filter.format)
        return false;
    if (!filter.host.empty() && m.host != filter.host)
        return false;
    if (!filter.source.empty() && source_of(page, mirror) != filter.source)
        return false;
    if (filter.section >= 0 && static_cast<int>(section_of(meta.kind)) != filter.section)
        return false;
    return true;
}

FilterResult filter_mirrors(const GamePage &page, const MirrorFilter &filter, const LinkPrefs &prefs)
{
    FilterResult out;
    int lowest = -1;
    for (const int mi : display_order(page))
    {
        const Mirror &m = page.mirrors[static_cast<std::size_t>(mi)];
        if (host_hidden(m.host, prefs))
        {
            ++out.hidden_hosts;
            continue;
        }
        if (mirror_passes(page, mi, filter, prefs))
        {
            out.mirrors.push_back(mi);
            continue;
        }
        const LinkMeta &meta = meta_of(page, mi);
        if (filter.runs_on_my_fw && fw_fit(meta.min_fw(), prefs.firmware) == FwFit::too_new)
        {
            // Counted only when the firmware is the reason it is hidden.
            MirrorFilter rest = filter;
            rest.runs_on_my_fw = false;
            if (mirror_passes(page, mi, rest, prefs))
            {
                ++out.hidden_too_new;
                const int need = fw_need(meta.min_fw());
                if (lowest < 0 || need < lowest)
                {
                    lowest = need;
                    out.lowest_needed = meta.min_fw();
                }
            }
        }
    }
    return out;
}

} // namespace orbit
