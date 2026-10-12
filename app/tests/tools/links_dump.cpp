// Wings TV app - Dumps the link parser, part sets and game grouping over a
// recorded catalogue as TSV, in the format of tools/linkref/dump_golden.py, so
// the C++ port can be diffed line by line against the Python reference.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Build (host): clang++ -std=c++20 -O2 -Isrc tests/tools/links_dump.cpp
//   src/orbit/link_meta.cpp src/orbit/link_sets.cpp src/orbit/game_group.cpp
//   src/orbit/json.cpp src/orbit/i18n.cpp -o links_dump
// Run: links_dump library.json out_dir

#include "orbit/game_group.hpp"
#include "orbit/json.hpp"
#include "orbit/link_meta.hpp"
#include "orbit/link_sets.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace orbit;

namespace
{
// Python's urlsplit(url).hostname, lower case, without "www.".
std::string url_host(const std::string &url)
{
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return {};
    std::size_t b = scheme + 3;
    std::size_t e = url.find_first_of("/?#", b);
    if (e == std::string::npos)
        e = url.size();
    std::string host = url.substr(b, e - b);
    const std::size_t at = host.rfind('@');
    if (at != std::string::npos)
        host = host.substr(at + 1);
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);
    for (char &c : host)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (host.rfind("www.", 0) == 0)
        host = host.substr(4);
    return host;
}

const char *kind_text(LinkKind k)
{
    switch (k)
    {
    case LinkKind::base:
        return "base";
    case LinkKind::game_dlc:
        return "game+dlc";
    case LinkKind::dlc:
        return "dlc";
    case LinkKind::update:
        return "update";
    case LinkKind::fix:
        return "fix";
    }
    return "";
}

const char *status_text(PartStatus s)
{
    switch (s)
    {
    case PartStatus::single:
        return "single";
    case PartStatus::complete:
        return "complete";
    case PartStatus::restarts:
        return "restarts";
    case PartStatus::gaps:
        return "gaps";
    case PartStatus::unnumbered:
        return "unnumbered";
    }
    return "";
}

std::string b(bool v)
{
    return v ? "1" : "0";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    json::Value doc;
    if (!json::parse(ss.str(), &doc))
        return 3;
    const std::string out = argv[2];
    std::ofstream fl(out + "/links.tsv", std::ios::binary);
    std::ofstream fs(out + "/sets.tsv", std::ios::binary);
    std::ofstream ft(out + "/titles.tsv", std::ios::binary);
    std::vector<Package> packages;
    for (const json::Value &p : doc["packages"].items())
    {
        const std::string id = std::to_string(p["id"].as_int());
        std::vector<ParsedLink> links;
        for (const json::Value &l : p["downloadLinks"].items())
        {
            ParsedLink pl;
            pl.index = static_cast<int>(l["index"].as_int());
            pl.name = l.text("name");
            pl.meta = parse_link(pl.name, url_host(l.text("url")));
            const LinkMeta &f = pl.meta;
            std::string tags;
            for (const std::string &t : f.tags)
                tags += (tags.empty() ? "" : ",") + t;
            const char *from = f.host_from == HostSource::name  ? "name"
                               : f.host_from == HostSource::url ? "url"
                                                                : "";
            fl << id << '\t' << pl.index << '\t' << f.region << '\t' << f.lang_from << '\t'
               << f.lang_to << '\t' << f.fw << '\t' << b(f.fw_plus) << '\t' << b(f.backport) << '\t'
               << f.backport_target << '\t' << kind_text(f.kind) << '\t' << b(f.kind_inferred) << '\t'
               << b(f.all_dlc) << '\t' << format_label(f.format) << '\t'
               << (f.standard_build ? "standard" : "") << '\t' << f.version << '\t'
               << (f.release >= 0 ? std::to_string(f.release) : "") << '\t'
               << (f.part >= 0 ? std::to_string(f.part) : "") << '\t' << b(f.part_unknown) << '\t'
               << b(f.truncated) << '\t' << tags << '\t' << f.packager << '\t' << f.host << '\t' << from
               << '\t' << f.size_text << '\t' << f.title_id << '\t' << f.label << '\t' << f.residue
               << '\n';
            links.push_back(std::move(pl));
        }
        const PartGrouping g = group_parts(links);
        for (const PartSet &s : g.sets)
        {
            fs << id << '\t' << s.host << '\t';
            for (std::size_t k = 0; k < s.links.size(); ++k)
                fs << (k ? "," : "") << links[static_cast<std::size_t>(s.links[k])].index;
            fs << '\t';
            for (std::size_t k = 0; k < s.numbers.size(); ++k)
                fs << (k ? "," : "") << s.numbers[k];
            fs << '\t' << status_text(s.status) << '\n';
        }
        ft << id << '\t' << norm_title(p.text("title")) << '\t' << loose_title(p.text("title")) << '\n';
        if (!links.empty())
        {
            Package pk;
            pk.id = id;
            pk.title = p.text("title");
            pk.title_id = p.text("titleId");
            pk.source_name = p.text("sourceName");
            pk.link_count = static_cast<int>(links.size());
            packages.push_back(pk);
        }
    }
    const GameIndex index = group_games(packages);
    std::vector<std::string> lines;
    for (const GameGroup &g : index.games)
    {
        std::vector<long> ids;
        for (const int p : g.packages)
            ids.push_back(std::stol(packages[static_cast<std::size_t>(p)].id));
        std::sort(ids.begin(), ids.end());
        std::string line;
        for (std::size_t k = 0; k < ids.size(); ++k)
            line += (k ? "," : "") + std::to_string(ids[k]);
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    std::ofstream fg(out + "/games.tsv", std::ios::binary);
    for (const std::string &l : lines)
        fg << l << '\n';
    std::printf("games %zu\n", index.games.size());
    return 0;
}
