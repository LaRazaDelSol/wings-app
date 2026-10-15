// Wings TV app - "Best": which mirror to download, and why.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// rank_key() and best() of docs/research/links-grouping.md section 4.5. One
// deliberate difference: a candidate with no version sorts after those with
// one (the reference's empty tuple sorted first).

#include "orbit/ranking.hpp"

#include "orbit/catalogue_view.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <array>

namespace orbit
{
namespace
{

struct Key
{
    std::array<int, 9> head{}; // flagged, too new, tier, fit, region, format, parts class, parts, size unknown
    int version_unknown = 0;
    std::vector<int> version; // compared descending
    int source_order = 0;
    int index = 0;

    bool operator<(const Key &o) const
    {
        if (head != o.head)
            return head < o.head;
        if (version_unknown != o.version_unknown)
            return version_unknown < o.version_unknown;
        if (version != o.version)
            return o.version < version;
        if (source_order != o.source_order)
            return source_order < o.source_order;
        return index < o.index;
    }
};

int status_class(PartStatus s)
{
    switch (s)
    {
    case PartStatus::single:
        return 0;
    case PartStatus::complete:
        return 1;
    case PartStatus::restarts:
    case PartStatus::unnumbered:
        return 2;
    case PartStatus::gaps:
        break;
    }
    return 3;
}

// Tier rank doubled so "prefer debrid off" can sit between automated and browser.
int tier_key(LinkTier tier, bool prefer_debrid)
{
    if (tier == LinkTier::debrid && !prefer_debrid)
        return 5;
    return tier_rank(tier) * 2;
}

Key key_of(const Candidate &c, const LinkPrefs &p)
{
    Key k;
    const FwFit fit = fw_fit(c.fw, p.firmware);
    k.head[0] = c.flagged ? 1 : 0;
    k.head[1] = fit == FwFit::too_new ? 1 : 0;
    k.head[2] = tier_key(c.tier, p.prefer_debrid);
    k.head[3] = static_cast<int>(fit);
    k.head[4] = p.region.empty() || c.region == p.region ? 0 : (c.region.empty() ? 1 : 2);
    k.head[5] = p.format == LinkFormat::none || c.format == p.format ? 0 : 1;
    const int cls = status_class(c.status);
    k.head[6] = p.prefer_single_file ? cls : (cls >= 2 ? 1 : 0);
    k.head[7] = p.prefer_single_file ? c.parts : 0;
    k.head[8] = c.size_known ? 0 : 1;
    k.version = version_parts(c.version);
    k.version_unknown = k.version.empty() ? 1 : 0;
    k.source_order = c.source_order;
    k.index = c.index;
    return k;
}

const char *tier_why(LinkTier tier)
{
    switch (tier)
    {
    case LinkTier::native_pkg:
        return tr("goes straight to the PS5 downloads");
    case LinkTier::debrid:
        return tr("debrid: fast, no captcha");
    case LinkTier::automated:
        return tr("Pegasus fetches it by itself");
    case LinkTier::supported:
        return tr("needs the PS5 browser");
    case LinkTier::unknown:
        return tr("unknown host");
    case LinkTier::unsupported:
        break;
    }
    return tr("not supported");
}

bool hidden(const std::string &host, const LinkPrefs &p)
{
    return std::find(p.hidden_hosts.begin(), p.hidden_hosts.end(), host) != p.hidden_hosts.end();
}

} // namespace

int tier_rank(LinkTier tier)
{
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

const char *best_tier_label(LinkTier tier)
{
    switch (tier)
    {
    case LinkTier::native_pkg:
        return tr("PKG to PS5");
    case LinkTier::debrid:
        return tr("Debrid");
    case LinkTier::automated:
        return tr("Automatic");
    case LinkTier::supported:
        return tr("Browser");
    case LinkTier::unknown:
        return tr("Unknown host");
    case LinkTier::unsupported:
        break;
    }
    return tr("Not supported");
}

FwFit fw_fit(std::string_view link_fw, std::string_view console_fw)
{
    if (console_fw.empty() || link_fw.empty())
        return console_fw.empty() ? FwFit::runs : FwFit::unknown;
    const int need = fw_need(link_fw);
    const int have = fw_console(console_fw);
    if (need < 0 || have < 0)
        return FwFit::unknown;
    return need <= have ? FwFit::runs : FwFit::too_new;
}

std::string Ranking::summary(const char *separator) const
{
    std::string out;
    for (const std::string &c : chips)
        out += (out.empty() ? "" : separator) + c;
    return out;
}

std::string Ranking::text() const
{
    if (best < 0)
        return {};
    return tr("Best: {chips}", {{"chips", summary()}}) + " \xE2\x80\x94 " + why;
}

Ranking rank_links(const std::vector<Candidate> &candidates, const LinkPrefs &prefs)
{
    Ranking r;
    std::vector<Key> keys;
    keys.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        keys.push_back(key_of(candidates[i], prefs));
        if (hidden(candidates[i].host, prefs))
            continue;
        r.order.push_back(static_cast<int>(i));
        if (fw_fit(candidates[i].fw, prefs.firmware) == FwFit::too_new)
            ++r.too_new;
        if (candidates[i].flagged)
            ++r.flagged;
    }
    std::stable_sort(r.order.begin(), r.order.end(),
                     [&](int a, int b) { return keys[static_cast<std::size_t>(a)] < keys[static_cast<std::size_t>(b)]; });
    if (r.order.empty())
        return r;
    r.best = r.order.front();
    const Candidate &b = candidates[static_cast<std::size_t>(r.best)];
    r.chips.emplace_back(best_tier_label(b.tier));
    if (!b.region.empty())
        r.chips.push_back(b.region);
    if (!b.fw.empty())
        r.chips.push_back((b.backport ? "BP " : "") + b.fw);
    if (b.format != LinkFormat::none)
        r.chips.emplace_back(format_label(b.format));
    r.chips.push_back(b.parts == 0 ? std::string(tr("1 file")) : trn(b.parts, "{count} part", "{count} parts"));
    std::vector<std::string> why;
    why.emplace_back(tier_why(b.tier));
    if (b.flagged)
        why.emplace_back(tr("failed for you before (no other link)"));
    if (!prefs.firmware.empty())
    {
        switch (fw_fit(b.fw, prefs.firmware))
        {
        case FwFit::runs:
            why.push_back(tr("runs on your {fw}", {{"fw", prefs.firmware}}));
            break;
        case FwFit::unknown:
            why.emplace_back(tr("firmware not stated"));
            break;
        case FwFit::too_new:
            why.push_back(tr("needs {need}, newer than your {fw} (no compatible link)",
                             {{"need", b.fw}, {"fw", prefs.firmware}}));
            break;
        }
    }
    if (!prefs.region.empty() && b.region == prefs.region)
        why.emplace_back(tr("your region"));
    if (b.parts == 0)
        why.emplace_back(tr("single file"));
    if (!b.version.empty())
    {
        const std::vector<int> bv = version_parts(b.version);
        bool newest = true;
        for (const int i : r.order)
            newest = newest && !(bv < version_parts(candidates[static_cast<std::size_t>(i)].version));
        why.push_back(newest ? std::string(tr("newest version")) : "v" + b.version);
    }
    for (const std::string &w : why)
        r.why += (r.why.empty() ? "" : ", ") + w;
    return r;
}

Candidate candidate_of(const GamePage &page, int mirror)
{
    Candidate c;
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    const GameLink &first = page.links[static_cast<std::size_t>(m.links.front())];
    const GamePackage &pkg = page.packages[static_cast<std::size_t>(first.package)];
    c.tier = m.tier;
    c.fw = first.meta.min_fw();
    c.backport = !first.meta.backport_target.empty();
    c.region = first.meta.region;
    c.format = first.meta.format;
    c.parts = m.parts();
    c.status = m.status;
    c.size_known = pkg.size_bytes > 0.0;
    c.version = first.meta.version;
    if (c.version.empty())
    {
        std::string_view v = pkg.version;
        if (!v.empty() && (v.front() == 'v' || v.front() == 'V'))
            v.remove_prefix(1);
        // Only a numeric version ("01.027"), not "Source-Fixe".
        if (!v.empty() && v.front() >= '0' && v.front() <= '9')
            c.version = std::string(v);
    }
    c.source_order = pkg.source_order;
    c.index = first.index;
    c.host = m.host;
    c.flagged = m.flagged_link(page) >= 0;
    return c;
}

void rank_page(GamePage *page, const LinkPrefs &prefs)
{
    // Mirrors inside each release; remember each release's best key.
    std::vector<Key> release_best(page->releases.size());
    std::vector<bool> has_best(page->releases.size(), false);
    for (std::size_t ri = 0; ri < page->releases.size(); ++ri)
    {
        Release &rel = page->releases[ri];
        std::vector<Candidate> cands;
        for (const int m : rel.mirrors)
            cands.push_back(candidate_of(*page, m));
        const Ranking r = rank_links(cands, prefs);
        std::vector<int> sorted;
        for (const int i : r.order)
            sorted.push_back(rel.mirrors[static_cast<std::size_t>(i)]);
        for (std::size_t i = 0; i < rel.mirrors.size(); ++i)
            if (std::find(r.order.begin(), r.order.end(), static_cast<int>(i)) == r.order.end())
                sorted.push_back(rel.mirrors[i]); // hidden hosts last
        if (r.best >= 0)
        {
            release_best[ri] = key_of(cands[static_cast<std::size_t>(r.best)], prefs);
            has_best[ri] = true;
        }
        rel.mirrors = std::move(sorted);
    }
    // Releases inside each section by their best mirror (the key ends with
    // version, source order and index).
    for (std::vector<int> &section : page->sections)
    {
        std::stable_sort(section.begin(), section.end(),
                         [&](int a, int b) -> bool
                         {
                             const auto ua = static_cast<std::size_t>(a);
                             const auto ub = static_cast<std::size_t>(b);
                             if (has_best[ua] != has_best[ub])
                                 return has_best[ua];
                             return has_best[ua] && release_best[ua] < release_best[ub];
                         });
    }
    // The page's Best: over every mirror of the Base section.
    std::vector<int> mirrors;
    for (const int ri : page->sections[static_cast<int>(Section::base)])
        for (const int m : page->releases[static_cast<std::size_t>(ri)].mirrors)
            mirrors.push_back(m);
    std::vector<Candidate> cands;
    for (const int m : mirrors)
        cands.push_back(candidate_of(*page, m));
    const Ranking r = rank_links(cands, prefs);
    page->best = r.best >= 0 ? mirrors[static_cast<std::size_t>(r.best)] : -1;
    page->best_chips = r.chips;
    page->best_why = r.why;
}

std::uint8_t game_tier(const GameGroup &game, const std::vector<Package> &packages,
                       const std::vector<std::uint8_t> &package_tiers, const LinkPrefs &prefs)
{
    LinkTier best = LinkTier::unsupported;
    bool via = false;
    for (const int p : game.packages)
    {
        if (p < 0 || p >= static_cast<int>(packages.size()))
            continue;
        const std::uint8_t packed = tier_of(packages, package_tiers, static_cast<std::size_t>(p));
        const LinkTier t = packed_tier(packed);
        via = via || packed_via_debrid(packed);
        if (tier_key(t, prefs.prefer_debrid) < tier_key(best, prefs.prefer_debrid))
            best = t;
        // The packed tier is the enum minimum (debrid after browser); the bit says a debrid link exists.
        if (packed_via_debrid(packed) &&
            tier_key(LinkTier::debrid, prefs.prefer_debrid) < tier_key(best, prefs.prefer_debrid))
            best = LinkTier::debrid;
    }
    return pack_tier(best, via);
}

} // namespace orbit
