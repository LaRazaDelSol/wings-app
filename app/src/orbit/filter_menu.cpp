// Wings TV app - Filter menus: named groups, an "N filters on" summary and a result count.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/filter_menu.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>

namespace orbit
{

const char *filter_group_name(FilterGroup group)
{
    switch (group)
    {
    case FilterGroup::show:
        return tr("Show");
    case FilterGroup::region_fw:
        return tr("Region & firmware");
    case FilterGroup::format_host:
        return tr("Format & host");
    case FilterGroup::source:
        return tr("Source");
    case FilterGroup::sort:
        return tr("Sort");
    case FilterGroup::none:
        break;
    }
    return "";
}

int filter_group_accent(FilterGroup group)
{
    switch (group)
    {
    case FilterGroup::show:
        return theme::accent_filter_show;
    case FilterGroup::region_fw:
        return theme::accent_filter_region_fw;
    case FilterGroup::format_host:
        return theme::accent_filter_format_host;
    case FilterGroup::source:
        return theme::accent_filter_source;
    case FilterGroup::sort:
        return theme::accent_filter_sort;
    case FilterGroup::none:
        break;
    }
    return theme::accent_none;
}

FilterGroup link_filter_group(LinkFilterRow row)
{
    switch (row)
    {
    case LinkFilterRow::runs_on_my_fw:
    case LinkFilterRow::region:
        return FilterGroup::region_fw;
    case LinkFilterRow::no_browser:
    case LinkFilterRow::single_file:
    case LinkFilterRow::kind:
        return FilterGroup::show;
    case LinkFilterRow::format:
    case LinkFilterRow::host:
        return FilterGroup::format_host;
    case LinkFilterRow::source:
        return FilterGroup::source;
    case LinkFilterRow::sort:
        return FilterGroup::sort;
    case LinkFilterRow::reset:
        break;
    }
    return FilterGroup::none;
}

SortGroup sort_group(SortMode mode)
{
    switch (mode)
    {
    case SortMode::az:
    case SortMode::za:
        return SortGroup::order;
    case SortMode::popular:
    case SortMode::rated:
    case SortMode::reviewed:
    case SortMode::my_downloads:
        return SortGroup::popularity;
    case SortMode::installed_first:
    case SortMode::not_installed_first:
    case SortMode::favourites_first:
        return SortGroup::library;
    case SortMode::tier_first:
    case SortMode::source:
        break;
    }
    return SortGroup::link;
}

const char *sort_group_name(SortGroup group)
{
    switch (group)
    {
    case SortGroup::order:
        return tr("Order");
    case SortGroup::popularity:
        return tr("Popularity");
    case SortGroup::library:
        return tr("Library state");
    case SortGroup::link:
        break;
    }
    return tr("Link quality & source");
}

int sort_group_accent(SortGroup group)
{
    switch (group)
    {
    case SortGroup::order:
        return theme::accent_sort_order;
    case SortGroup::popularity:
        return theme::accent_sort_popularity;
    case SortGroup::library:
        return theme::accent_sort_library;
    case SortGroup::link:
        break;
    }
    return theme::accent_sort_link;
}

std::vector<SortMode> sort_menu_order(const std::vector<SortMode> &available)
{
    // Within a group: the order a person expects (the listing here is the menu's order).
    static const SortMode kMenu[] = {
        SortMode::az,           SortMode::za,
        SortMode::popular,      SortMode::rated,
        SortMode::reviewed,     SortMode::my_downloads,
        SortMode::installed_first, SortMode::not_installed_first, SortMode::favourites_first,
        SortMode::tier_first,   SortMode::source,
    };
    std::vector<SortMode> out;
    for (const SortMode mode : kMenu)
        if (std::find(available.begin(), available.end(), mode) != available.end())
            out.push_back(mode);
    return out;
}

int grid_filters_on(const CatalogueState &s)
{
    return (s.installed != InstalledFilter::any ? 1 : 0) + (s.favourites_only ? 1 : 0) +
           (s.tier != TierFilter::any ? 1 : 0) + (s.has_links_only ? 1 : 0) + (s.source.empty() ? 0 : 1) +
           (s.region.empty() ? 0 : 1) + (s.runs_on_my_fw ? 1 : 0) + (s.no_browser ? 1 : 0) +
           (s.single_file_only ? 1 : 0);
}

int link_filters_on(const MirrorFilter &f)
{
    return (f.runs_on_my_fw ? 1 : 0) + (f.no_browser ? 1 : 0) + (f.single_file ? 1 : 0) +
           (f.region.empty() ? 0 : 1) + (f.format.empty() ? 0 : 1) + (f.host.empty() ? 0 : 1) +
           (f.source.empty() ? 0 : 1) + (f.section >= 0 ? 1 : 0);
}

bool link_row_changed(LinkFilterRow row, const MirrorFilter &f, MirrorSort sort)
{
    switch (row)
    {
    case LinkFilterRow::runs_on_my_fw:
        return f.runs_on_my_fw;
    case LinkFilterRow::no_browser:
        return f.no_browser;
    case LinkFilterRow::region:
        return !f.region.empty();
    case LinkFilterRow::single_file:
        return f.single_file;
    case LinkFilterRow::format:
        return !f.format.empty();
    case LinkFilterRow::kind:
        return f.section >= 0;
    case LinkFilterRow::host:
        return !f.host.empty();
    case LinkFilterRow::source:
        return !f.source.empty();
    case LinkFilterRow::sort:
        return sort != MirrorSort::best;
    case LinkFilterRow::reset:
        break;
    }
    return false;
}

std::string filters_on_text(int count)
{
    if (count <= 0)
        return tr("No filters on");
    return trn(count, "{count} filter on", "{count} filters on", {{"count", std::to_string(count)}});
}

std::string result_count_text(int shown, int total, bool downloads)
{
    const std::string s = i18n::count(shown);
    const std::string t = i18n::count(total);
    return downloads ? tr("{shown} of {total} downloads", {{"shown", s}, {"total", t}})
                     : tr("{shown} of {total} games", {{"shown", s}, {"total", t}});
}

int first_focus_index(const std::vector<bool> &changed)
{
    for (std::size_t i = 0; i < changed.size(); ++i)
        if (changed[i])
            return static_cast<int>(i);
    return 0;
}

} // namespace orbit
