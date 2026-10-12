// Wings TV app - Export the catalogue's title list for the popularity index builder.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes [{"titleId","title"}] (deduplicated by titleId, titles cleaned as the index builder's
// clean_title does: trademark signs, [..] and (..) tags, @packager, version numbers). Pure
// formatting here; the file is written with the checked save helpers.
#pragma once

#include "orbit/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

constexpr std::size_t kTitleExportMaxBytes = 2u << 20; // a hand-off file, not a dump

struct TitleExport
{
    std::string json;    // "[]\n" when nothing qualified
    int count = 0;       // titles written
    bool capped = false; // more titles existed than max_bytes allows
};

// Mirrors popularity/normalize.py clean_title (case kept).
std::string clean_export_title(std::string_view title);

// Only PPSA/CUSA ids, first seen per id wins, sorted by id.
TitleExport build_title_export(const std::vector<Package> &packages,
                               std::size_t max_bytes = kTitleExportMaxBytes);

// "<dir of each path>/titles.json", without duplicates, /data/wings first.
std::vector<std::string> title_export_paths(const std::vector<std::string> &sibling_paths);

// Atomic write to every path; returns how many were written.
int write_title_export(const std::vector<std::string> &paths, const std::string &json);

} // namespace orbit
