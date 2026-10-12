// Wings TV app - A game's links, folded: sections, releases, mirrors, part sets.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_page.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <unordered_map>

namespace orbit
{
namespace
{

constexpr const char *kDot = " \xC2\xB7 "; // " · "

// How much a link's name says: the copy of a URL that says most is shown.
int field_count(const LinkMeta &m)
{
    return (m.region.empty() ? 0 : 1) + (m.fw.empty() ? 0 : 1) + (m.backport ? 1 : 0) +
           (m.backport_target.empty() ? 0 : 1) + (m.kind_inferred ? 0 : 1) +
           (m.format == LinkFormat::none ? 0 : 1) + (m.version.empty() ? 0 : 1) +
           (m.packager.empty() ? 0 : 1) + static_cast<int>(m.tags.size()) + (m.label.empty() ? 0 : 1) +
           (m.release >= 0 ? 1 : 0) + (m.has_part() ? 1 : 0);
}

int tier_badness(LinkTier tier)
{
    // The research order (debrid right after native .pkg); only used to pick a set's worst link.
    switch (tier)
    {
    case LinkTier::native_pkg:
        return 0;
    case LinkTier::debrid:
        return 1;
    case LinkTier::automated:
        return 2;
    case LinkTier::supported:
        return 3;
    case LinkTier::unknown:
        return 4;
    case LinkTier::unsupported:
        break;
    }
    return 5;
}

} // namespace

const char *section_label(Section section)
{
    switch (section)
    {
    case Section::base:
        return tr("Game");
    case Section::update:
        return tr("Updates");
    case Section::dlc:
        return tr("DLC");
    case Section::other:
        break;
    }
    return tr("Other");
}

Section section_of(LinkKind kind)
{
    switch (kind)
    {
    case LinkKind::base:
    case LinkKind::game_dlc:
        return Section::base;
    case LinkKind::update:
        return Section::update;
    case LinkKind::dlc:
        return Section::dlc;
    case LinkKind::fix:
        break;
    }
    return Section::other;
}

std::string Mirror::parts_text() const
{
    if (!multipart())
        return tr("1 file");
    const long n = static_cast<long>(links.size());
    std::string range;
    for (const std::vector<int> &run : runs)
    {
        if (!range.empty())
            range += ", ";
        range += run.size() == 1 ? std::to_string(run.front())
                                 : std::to_string(run.front()) + "\xE2\x80\x93" + std::to_string(run.back());
    }
    switch (status)
    {
    case PartStatus::complete:
        return trn(n, "{count} part", "{count} parts") + " (" + range + ")";
    case PartStatus::restarts:
    case PartStatus::gaps:
        return trn(n, "{count} link", "{count} links") + " (" + tr("parts {range}", {{"range", range}}) + ")";
    case PartStatus::unnumbered:
        return trn(n, "{count} part", "{count} parts") + " (" + tr("numbers cut off") + ")";
    case PartStatus::single:
        break;
    }
    return tr("1 file");
}

std::string Mirror::warning() const
{
    switch (status)
    {
    case PartStatus::restarts:
        return tr("numbering restarts: these may be two different archives");
    case PartStatus::gaps:
    {
        std::string list;
        for (const int m : missing)
            list += (list.empty() ? "" : ", ") + std::to_string(m);
        if (missing.size() == 1)
            return tr("part {list} missing", {{"list", list}});
        if (!missing.empty())
            return tr("parts {list} missing", {{"list", list}});
        return tr("numbering does not start at 1");
    }
    case PartStatus::unnumbered:
        return tr("numbers cut off in the catalogue");
    case PartStatus::single:
    case PartStatus::complete:
        break;
    }
    return {};
}

std::string Release::header(const GamePage &page) const
{
    std::vector<std::string> parts;
    if (!meta.region.empty())
        parts.push_back(meta.lang_to.empty() ? meta.region : meta.region + " > " + meta.lang_to);
    if (!meta.backport_target.empty())
        parts.push_back((meta.fw.empty() ? std::string() : meta.fw + " ") + "BP " + meta.backport_target);
    else if (meta.backport)
        parts.push_back((meta.fw.empty() ? std::string() : meta.fw + " ") + tr("BackPort"));
    else if (!meta.fw.empty())
        parts.push_back(meta.fw + (meta.fw_plus ? "+" : ""));
    std::string kind = kind_label(meta.kind);
    if (meta.kind == LinkKind::game_dlc && meta.all_dlc)
        kind = tr("Game + all DLC");
    parts.push_back(kind);
    if (meta.format != LinkFormat::none)
        parts.emplace_back(format_label(meta.format));
    if (meta.standard_build)
        parts.emplace_back(tr("Standard"));
    if (!meta.version.empty())
        parts.push_back("v" + meta.version);
    if (meta.release >= 0)
        parts.push_back(tr("Release {n}", {{"n", std::to_string(meta.release)}}));
    if (package >= 0 && package < static_cast<int>(page.packages.size()))
        parts.push_back(page.packages[static_cast<std::size_t>(package)].source_name);
    std::string out;
    for (const std::string &p : parts)
        out += (out.empty() ? "" : kDot) + p;
    return out;
}

std::string Release::subheader() const
{
    std::vector<std::string> parts;
    if (!meta.packager.empty())
        parts.push_back("@" + meta.packager);
    for (const std::string &t : meta.tags)
        parts.push_back(t);
    if (!meta.label.empty())
        parts.push_back(meta.label);
    std::string out;
    for (const std::string &p : parts)
        out += (out.empty() ? "" : kDot) + p;
    return out;
}

std::vector<LinkRef> GamePage::queue_refs(int mirror, const std::function<bool(const LinkRef &)> &skip) const
{
    std::vector<LinkRef> out;
    if (mirror < 0 || mirror >= static_cast<int>(mirrors.size()))
        return out;
    for (const int l : mirrors[static_cast<std::size_t>(mirror)].links)
    {
        const GameLink &link = links[static_cast<std::size_t>(l)];
        LinkRef ref{packages[static_cast<std::size_t>(link.package)].id, link.index};
        if (skip && skip(ref))
            continue;
        out.push_back(std::move(ref));
    }
    return out;
}

std::vector<std::string> GamePage::regions() const
{
    std::vector<std::string> out;
    for (const GameLink &l : links)
        if (!l.duplicate && !l.meta.region.empty() &&
            std::find(out.begin(), out.end(), l.meta.region) == out.end())
            out.push_back(l.meta.region);
    return out;
}

GamePage build_game_page(const std::vector<PackageDetail> &details, const std::vector<std::string> &source_order)
{
    GamePage page;
    // Source order: the user's, then the others A to Z.
    std::vector<std::string> sources = source_order;
    std::vector<std::string> others;
    for (const PackageDetail &d : details)
        if (std::find(sources.begin(), sources.end(), d.package.source_name) == sources.end() &&
            std::find(others.begin(), others.end(), d.package.source_name) == others.end())
            others.push_back(d.package.source_name);
    std::sort(others.begin(), others.end());
    sources.insert(sources.end(), others.begin(), others.end());

    // Pass 1: parse and group the parts of every package.
    struct PackageWork
    {
        std::vector<ParsedLink> parsed;
        PartGrouping grouping;
        int first_link = 0; // index of its first GameLink
    };
    std::vector<PackageWork> work(details.size());
    std::vector<bool> in_set;
    for (std::size_t pi = 0; pi < details.size(); ++pi)
    {
        const PackageDetail &d = details[pi];
        GamePackage gp;
        gp.id = d.package.id;
        gp.title = d.package.title;
        gp.source_name = d.package.source_name;
        gp.version = d.package.version;
        gp.size_bytes = d.package.size_bytes;
        gp.source_order = static_cast<int>(std::find(sources.begin(), sources.end(), gp.source_name) - sources.begin());
        page.packages.push_back(std::move(gp));
        PackageWork &w = work[pi];
        w.parsed = parse_links(d.links);
        w.grouping = group_parts(w.parsed);
        w.first_link = static_cast<int>(page.links.size());
        for (std::size_t k = 0; k < d.links.size(); ++k)
        {
            GameLink l;
            l.package = static_cast<int>(pi);
            l.index = d.links[k].index;
            l.name = d.links[k].name;
            l.url = d.links[k].url;
            l.meta = w.parsed[k].meta;
            l.tier = link_tier(d.links[k]);
            l.needs_browser = d.links[k].needs_browser;
            l.services = d.links[k].services;
            page.links.push_back(std::move(l));
            in_set.push_back(false);
        }
        for (const PartSet &s : w.grouping.sets)
            for (const int at : s.links)
                in_set[static_cast<std::size_t>(w.first_link + at)] = true;
    }

    // Pass 2: each URL once. Set parts are never hidden (a set needs all of
    // them); among single links the one whose name says most is kept.
    std::unordered_map<std::string, std::vector<int>> by_url;
    for (std::size_t i = 0; i < page.links.size(); ++i)
        if (!page.links[i].url.empty())
            by_url[page.links[i].url].push_back(static_cast<int>(i));
    for (auto &entry : by_url)
    {
        std::vector<int> &ids = entry.second;
        if (ids.size() < 2)
            continue;
        int keep = -1;
        for (const int i : ids)
            if (in_set[static_cast<std::size_t>(i)])
            {
                keep = i;
                break;
            }
        if (keep < 0)
        {
            keep = ids.front();
            for (const int i : ids)
                if (field_count(page.links[static_cast<std::size_t>(i)].meta) >
                    field_count(page.links[static_cast<std::size_t>(keep)].meta))
                    keep = i;
        }
        GameLink &kept = page.links[static_cast<std::size_t>(keep)];
        for (const int i : ids)
        {
            if (i == keep)
                continue;
            GameLink &other = page.links[static_cast<std::size_t>(i)];
            kept.copies.push_back(LinkRef{page.packages[static_cast<std::size_t>(other.package)].id, other.index});
            if (!in_set[static_cast<std::size_t>(i)])
                other.duplicate = true;
        }
    }

    // Pass 3: releases and mirrors, in catalogue order.
    for (std::size_t pi = 0; pi < details.size(); ++pi)
    {
        const PackageWork &w = work[pi];
        std::unordered_map<std::string, int> release_of;
        // position -> the set it starts, or -1
        std::vector<int> set_first(w.parsed.size(), -1);
        std::vector<bool> is_part(w.parsed.size(), false);
        for (std::size_t s = 0; s < w.grouping.sets.size(); ++s)
        {
            set_first[static_cast<std::size_t>(w.grouping.sets[s].links.front())] = static_cast<int>(s);
            for (const int at : w.grouping.sets[s].links)
                is_part[static_cast<std::size_t>(at)] = true;
        }
        std::vector<int> order(w.parsed.size());
        for (std::size_t k = 0; k < order.size(); ++k)
            order[k] = static_cast<int>(k);
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b)
                         { return w.parsed[static_cast<std::size_t>(a)].index < w.parsed[static_cast<std::size_t>(b)].index; });
        for (const int at : order)
        {
            const std::size_t pos = static_cast<std::size_t>(at);
            const int link_id = w.first_link + at;
            if (is_part[pos] && set_first[pos] < 0)
                continue; // a later part of a set already placed
            if (page.links[static_cast<std::size_t>(link_id)].duplicate)
                continue;
            Mirror m;
            const LinkMeta &meta = page.links[static_cast<std::size_t>(link_id)].meta;
            m.host = meta.host;
            if (set_first[pos] >= 0)
            {
                const PartSet &s = w.grouping.sets[static_cast<std::size_t>(set_first[pos])];
                for (const int p : s.links)
                    m.links.push_back(w.first_link + p);
                m.status = s.status;
                m.numbers = s.numbers;
                m.runs = s.runs;
                m.missing = s.missing();
            }
            else
            {
                m.links.push_back(link_id);
            }
            int worst = -1;
            for (const int l : m.links)
            {
                const GameLink &gl = page.links[static_cast<std::size_t>(l)];
                if (tier_badness(gl.tier) > worst)
                {
                    worst = tier_badness(gl.tier);
                    m.tier = gl.tier;
                }
                m.needs_browser = m.needs_browser || gl.needs_browser;
            }
            const std::string key = release_key(meta);
            auto it = release_of.find(key);
            if (it == release_of.end())
            {
                Release r;
                r.package = static_cast<int>(pi);
                r.key = key;
                r.meta = meta;
                r.section = section_of(meta.kind);
                it = release_of.emplace(key, static_cast<int>(page.releases.size())).first;
                page.sections[static_cast<int>(r.section)].push_back(static_cast<int>(page.releases.size()));
                page.releases.push_back(std::move(r));
            }
            m.release = it->second;
            page.releases[static_cast<std::size_t>(it->second)].mirrors.push_back(static_cast<int>(page.mirrors.size()));
            page.mirrors.push_back(std::move(m));
        }
    }
    for (const GameLink &l : page.links)
    {
        if (l.duplicate)
            ++page.duplicate_links;
        else
            ++page.unique_links;
    }
    return page;
}

} // namespace orbit
