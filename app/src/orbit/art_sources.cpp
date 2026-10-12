// Wings TV app - Where art comes from (see art_sources.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/art_sources.hpp"

namespace orbit
{

const char *icon_source_key(IconSource source)
{
    return source == IconSource::prospero ? "prospero" : "catalogue";
}

bool parse_icon_source(std::string_view text, IconSource *out)
{
    if (text == "catalogue")
        *out = IconSource::catalogue;
    else if (text == "prospero")
        *out = IconSource::prospero;
    else
        return false;
    return true;
}

const char *hero_source_key(HeroSource source)
{
    return source == HeroSource::prospero ? "prospero" : "all";
}

bool parse_hero_source(std::string_view text, HeroSource *out)
{
    if (text == "all")
        *out = HeroSource::all;
    else if (text == "prospero")
        *out = HeroSource::prospero;
    else
        return false;
    return true;
}

bool is_prospero_pic0_url(std::string_view url)
{
    constexpr std::string_view head = "https://cdn.prosperopatches.com/titles/PPSA";
    constexpr std::string_view tail = "/pic0.webp";
    // head + 5 digits + "_" + 64 hex + tail
    if (url.size() != head.size() + 5 + 1 + 64 + tail.size() || url.substr(0, head.size()) != head ||
        url.substr(url.size() - tail.size()) != tail)
        return false;
    std::size_t at = head.size();
    for (int i = 0; i < 5; ++i, ++at)
        if (url[at] < '0' || url[at] > '9')
            return false;
    if (url[at++] != '_')
        return false;
    for (int i = 0; i < 64; ++i, ++at)
        if (!((url[at] >= '0' && url[at] <= '9') || (url[at] >= 'a' && url[at] <= 'f')))
            return false;
    return true;
}

std::string prospero_icon_url(std::string_view pic0_url)
{
    if (!is_prospero_pic0_url(pic0_url))
        return {};
    std::string out(pic0_url.substr(0, pic0_url.size() - std::string_view("pic0.webp").size()));
    out += "icon0.webp";
    return out;
}

bool hero_url_allowed(std::string_view url, HeroSource source)
{
    return source == HeroSource::all || is_prospero_pic0_url(url);
}

} // namespace orbit
