// Wings TV app - Multi-part archives: which links of a package form one set.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// group_parts() of docs/research/links-grouping.md section 1.7, same rules.

#include "orbit/link_sets.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace orbit
{

std::vector<ParsedLink> parse_links(const std::vector<PackageLink> &links)
{
    std::vector<ParsedLink> out;
    out.reserve(links.size());
    for (const PackageLink &link : links)
    {
        ParsedLink p;
        p.index = link.index;
        p.name = link.name;
        p.meta = parse_link(link.name, link.host);
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<int> PartSet::missing() const
{
    std::vector<int> out;
    if (status != PartStatus::gaps)
        return out;
    for (const std::vector<int> &run : runs)
    {
        int expect = 1;
        for (const int n : run)
        {
            for (; expect < n; ++expect)
                out.push_back(expect);
            expect = n + 1;
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string PartSet::range_text() const
{
    std::string out;
    for (const std::vector<int> &run : runs)
    {
        if (run.empty())
            continue;
        if (!out.empty())
            out += ", ";
        bool consecutive = true;
        for (std::size_t k = 1; k < run.size(); ++k)
            consecutive = consecutive && run[k] == run[k - 1] + 1;
        if (run.size() == 1)
        {
            out += std::to_string(run.front());
        }
        else if (consecutive)
        {
            out += std::to_string(run.front()) + "\xE2\x80\x93" + std::to_string(run.back());
        }
        else
        {
            for (std::size_t k = 0; k < run.size(); ++k)
                out += (k ? ", " : "") + std::to_string(run[k]);
        }
    }
    return out;
}

PartGrouping group_parts(const std::vector<ParsedLink> &links)
{
    PartGrouping out;
    std::vector<int> order(links.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b)
                     {
                         const ParsedLink &x = links[static_cast<std::size_t>(a)];
                         const ParsedLink &y = links[static_cast<std::size_t>(b)];
                         return x.index != y.index ? x.index < y.index : x.name < y.name;
                     });
    std::unordered_map<std::string, int> name_count;
    for (const ParsedLink &l : links)
        ++name_count[l.name];

    struct Building
    {
        PartSet set;
        bool trusted = true;
    };
    std::vector<Building> sets;
    std::unordered_map<std::string, std::size_t> by_key;
    for (const int at : order)
    {
        const ParsedLink &l = links[static_cast<std::size_t>(at)];
        const LinkMeta &f = l.meta;
        const bool is_part = f.part >= 0 || f.part_unknown || (f.truncated && name_count[l.name] > 1);
        if (!is_part)
        {
            out.singles.push_back(at);
            continue;
        }
        const std::string release = release_key(f);
        // The reference keys on (release, host) where a missing host differs from an empty one.
        std::string key = release + '\x1e' + f.host + (f.host_from == HostSource::none ? "\x1d" : "");
        auto it = by_key.find(key);
        if (it == by_key.end())
        {
            it = by_key.emplace(std::move(key), sets.size()).first;
            sets.emplace_back();
            sets.back().set.release = release;
            sets.back().set.host = f.host;
        }
        Building &b = sets[it->second];
        b.set.links.push_back(at);
        b.set.numbers.push_back(f.part);
        if (f.truncated || f.part < 0)
            b.trusted = false; // a 63-byte cut can turn 'part 12' into 'part 1'
    }
    for (Building &b : sets)
    {
        PartSet &s = b.set;
        if (!b.trusted)
            for (std::size_t k = 0; k < s.numbers.size(); ++k)
                s.numbers[k] = static_cast<int>(k) + 1; // position in index order
        std::vector<int> cur;
        for (const int n : s.numbers)
        {
            if (!cur.empty() && n <= cur.back())
            {
                s.runs.push_back(std::move(cur));
                cur.clear();
            }
            cur.push_back(n);
        }
        s.runs.push_back(std::move(cur));
        bool gaps = false;
        for (const std::vector<int> &run : s.runs)
        {
            if (run.front() != 1)
                gaps = true;
            for (std::size_t k = 1; k < run.size(); ++k)
                if (run[k] != run.front() + static_cast<int>(k))
                    gaps = true;
        }
        s.status = !b.trusted          ? PartStatus::unnumbered
                   : gaps              ? PartStatus::gaps
                   : s.runs.size() > 1 ? PartStatus::restarts
                                       : PartStatus::complete;
        out.sets.push_back(std::move(s));
    }
    return out;
}

std::vector<int> parts_to_queue(const PartSet &set, const std::vector<ParsedLink> &links,
                                const std::function<bool(int link_index)> &skip)
{
    std::vector<int> out;
    for (const int at : set.links)
    {
        const int index = links[static_cast<std::size_t>(at)].index;
        if (skip && skip(index))
            continue;
        out.push_back(index);
    }
    return out;
}

} // namespace orbit
