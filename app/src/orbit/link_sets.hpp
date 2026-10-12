// Wings TV app - Multi-part archives: which links of a package form one set.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parts are separate links of the same package with no parent record. A set
// is identified by (release, host): each host is a full mirror and cuts the
// archive its own way, so parts never mix hosts. Sets are found by key, never
// by adjacency (hosts interleave in the catalogue). No name says "of M", so
// "complete" means "no visible hole" (docs/research/links-grouping.md section 2).

#pragma once

#include "orbit/link_meta.hpp"

#include <functional>
#include <string>
#include <vector>

namespace orbit
{

// One link of one package with what its name says.
struct ParsedLink
{
    int index = 0; // Pegasus linkIndex
    std::string name;
    LinkMeta meta;
};

// Parses every link of a package (PackageLink::host is the URL host).
std::vector<ParsedLink> parse_links(const std::vector<PackageLink> &links);

enum class PartStatus : std::uint8_t
{
    single,     // not a multipart set (one file)
    complete,   // numbers are exactly 1..N, each once
    restarts,   // numbering starts again at 1 (two archives under one label?)
    gaps,       // a run misses a number or does not start at 1
    unnumbered, // the 63-byte cut hid the numbers; numbered by position
};

struct PartSet
{
    std::string release; // release_key() of its links
    std::string host;    // LinkMeta::host
    std::vector<int> links;   // positions in the input vector, in download (catalogue index) order
    std::vector<int> numbers; // the part number of each link (by position when unnumbered)
    std::vector<std::vector<int>> runs; // numbers split where they restart
    PartStatus status = PartStatus::complete;

    int count() const
    {
        return static_cast<int>(links.size());
    }
    // Numbers missing inside the runs ("part 4 missing"); empty unless gaps.
    std::vector<int> missing() const;
    // "1-8", "1-10, 1-4" (en dash), "1, 2, 3, 5, 6" for a run with holes.
    std::string range_text() const;
};

struct PartGrouping
{
    std::vector<int> singles; // positions of links that are not parts, index order
    std::vector<PartSet> sets;
};

// group_parts() of the reference: links of ONE package, any order.
PartGrouping group_parts(const std::vector<ParsedLink> &links);

// "Download all parts": the linkIndexes of a set to queue, in download order,
// leaving out those `skip` says are already queued or done (may be empty).
std::vector<int> parts_to_queue(const PartSet &set, const std::vector<ParsedLink> &links,
                                const std::function<bool(int link_index)> &skip = {});

} // namespace orbit
