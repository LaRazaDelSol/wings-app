// Wings TV app - Game page: default filter seeded from the preferences, and the mirror sort.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_filters.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <cctype>

namespace orbit
{

namespace
{

bool has_value(const std::vector<FacetValue> &values, const std::string &value)
{
    for (const FacetValue &v : values)
        if (v.value == value)
            return true;
    return false;
}

std::string lower_ascii(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string drop_note(const char *what, const std::string &value)
{
    return tr("Your {what} ({value}) would hide every download, so it is not applied.",
              {{"what", what}, {"value", value}});
}

} // namespace

FilterSeed seed_filter(const GamePage &page, const Facets &facets, const LinkPrefs &prefs)
{
    FilterSeed seed;
    MirrorFilter &f = seed.filter;
    f.runs_on_my_fw = !prefs.firmware.empty() && facets.fw_too_new > 0;
    if (!prefs.region.empty() && facets.show_regions() && has_value(facets.regions, prefs.region))
        f.region = prefs.region;
    const std::string format = format_label(prefs.format);
    if (!format.empty() && facets.show_formats() && has_value(facets.formats, format))
        f.format = format;

    const auto empty = [&]() { return facets.total > 0 && filter_mirrors(page, f, prefs).mirrors.empty(); };
    if (f.active() && empty() && !f.format.empty())
    {
        seed.note = drop_note(tr("format"), f.format);
        f.format.clear();
        seed.dropped_format = true;
    }
    if (f.active() && empty() && !f.region.empty())
    {
        seed.note = drop_note(tr("region"), f.region);
        f.region.clear();
        seed.dropped_region = true;
    }
    if (f.active() && empty() && f.runs_on_my_fw)
    {
        seed.note = drop_note(tr("firmware"), prefs.firmware);
        f.runs_on_my_fw = false;
        seed.dropped_firmware = true;
    }
    return seed;
}

const char *mirror_sort_label(MirrorSort sort)
{
    switch (sort)
    {
    case MirrorSort::best:
        return tr("Best");
    case MirrorSort::newest:
        return tr("Newest version");
    case MirrorSort::fewest_parts:
        return tr("Fewest parts");
    case MirrorSort::host:
        return tr("Host A-Z");
    }
    return tr("Best");
}

MirrorSort cycle_mirror_sort(MirrorSort sort, int delta)
{
    const int n = kMirrorSortCount;
    return static_cast<MirrorSort>((((static_cast<int>(sort) + (delta < 0 ? -1 : 1)) % n) + n) % n);
}

void sort_mirrors(const GamePage &page, std::vector<int> *mirrors, MirrorSort sort)
{
    if (sort == MirrorSort::best || mirrors->size() < 2)
        return;
    const auto parts_of = [&](int m)
    {
        const Mirror &mirror = page.mirrors[static_cast<std::size_t>(m)];
        return mirror.multipart() ? static_cast<int>(mirror.links.size()) : 1;
    };
    const auto version_of = [&](int m)
    {
        const Mirror &mirror = page.mirrors[static_cast<std::size_t>(m)];
        const GameLink &link = page.links[static_cast<std::size_t>(mirror.links.front())];
        return version_parts(link.meta.version);
    };
    const auto before = [&](int a, int b)
    {
        switch (sort)
        {
        case MirrorSort::newest:
        {
            const std::vector<int> va = version_of(a);
            const std::vector<int> vb = version_of(b);
            if (va.empty() != vb.empty())
                return !va.empty();
            return va > vb;
        }
        case MirrorSort::fewest_parts:
            return parts_of(a) < parts_of(b);
        case MirrorSort::host:
            return lower_ascii(page.mirrors[static_cast<std::size_t>(a)].host) <
                   lower_ascii(page.mirrors[static_cast<std::size_t>(b)].host);
        case MirrorSort::best:
            break;
        }
        return false;
    };
    std::size_t i = 0;
    while (i < mirrors->size())
    {
        const int release = page.mirrors[static_cast<std::size_t>((*mirrors)[i])].release;
        std::size_t j = i;
        while (j < mirrors->size() && page.mirrors[static_cast<std::size_t>((*mirrors)[j])].release == release)
            ++j;
        std::stable_sort(mirrors->begin() + static_cast<std::ptrdiff_t>(i),
                         mirrors->begin() + static_cast<std::ptrdiff_t>(j), before);
        i = j;
    }
}

} // namespace orbit
