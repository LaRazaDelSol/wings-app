// Wings TV app - Where art comes from (see art_sources.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/art_sources.hpp"

#include "orbit/popularity.hpp"

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

std::string choose_hero(const PopularityIndex &index, const std::vector<std::string> &title_ids, HeroSource source)
{
    for (const std::string &id : title_ids)
    {
        const PopRecord *record = index.find(id);
        if (record != nullptr && !record->hero.empty() && hero_url_allowed(record->hero, source))
            return record->hero;
    }
    return {};
}

std::string igdb_full_url(std::string_view low_url)
{
    constexpr std::string_view kPrefix = "https://images.igdb.com/igdb/image/upload/";
    constexpr std::string_view kSmall = "t_screenshot_big/";
    if (low_url.substr(0, kPrefix.size()) != kPrefix)
        return {};
    const std::string_view rest = low_url.substr(kPrefix.size());
    if (rest.substr(0, kSmall.size()) != kSmall || rest.size() == kSmall.size())
        return {};
    return std::string(kPrefix) + "t_1080p/" + std::string(rest.substr(kSmall.size()));
}

HeroPick pick_hero(const PopularityIndex &index, const std::vector<std::string> &title_ids, HeroSource source,
                   bool use_second)
{
    HeroPick pick;
    pick.full = choose_hero(index, title_ids, source);
    // The small IGDB picture is not used when only Prosperopatches art is wanted.
    if (source == HeroSource::all)
        pick.low = index.hero_low(title_ids);
    const std::string igdb = igdb_full_url(pick.low);
    pick.has_both = is_prospero_pic0_url(pick.full) && !igdb.empty();
    if (pick.has_both)
    {
        if (use_second)
        {
            pick.second = true;
            pick.full = igdb; // its small stand-in is the same picture: the swap is only sharpening
        }
        else
        {
            pick.low.clear(); // Prosperopatches' picture alone: no other picture shown first
        }
    }
    return pick;
}

std::string hero_art_key(const std::string &art_key, bool second)
{
    return second ? art_key + kHeroSecondTail : art_key;
}

bool HeroPrefetchGate::update(const std::string &game_id, double now_s)
{
    if (game_id.empty())
    {
        game_.clear();
        done_ = false;
        return false;
    }
    if (game_id != game_)
    {
        game_ = game_id;
        since_ = now_s;
        done_ = false;
        return false;
    }
    if (done_ || now_s - since_ < kDwellS || now_s - last_fetch_ < kMinGapS)
        return false;
    done_ = true;
    last_fetch_ = now_s;
    return true;
}

void HeroPrefetchGate::reset()
{
    game_.clear();
    done_ = false;
}

} // namespace orbit
