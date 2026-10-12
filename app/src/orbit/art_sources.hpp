// Wings TV app - Where art comes from: the cover icon source and the 16:9 game-page art source.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two player choices (Settings > Wings > Art sources), both kept in WingsLocalPrefs:
//   * Cover icons: "Catalogue" (default: the poster link of each catalogue entry) or "Prosperopatches": the
//     title's PS5 icon from cdn.prosperopatches.com. The popularity data already carries the address of that
//     title's 16:9 picture (.../pic0.webp); the icon (.../icon0.webp) sits next to it, so no extra lookup is
//     made. A title Prosperopatches has no art for keeps its catalogue cover.
//   * 16:9 game-page art: "All sources" (default: Prosperopatches first, then IGDB, Steam) or "Prosperopatches
//     only" (the PS5's own background, nothing from IGDB or Steam; the page keeps the cover wash otherwise).
// Only the exact address shape Prosperopatches uses is ever derived or accepted.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

class PopularityIndex;

enum class IconSource : std::uint8_t
{
    catalogue,
    prospero
};
enum class HeroSource : std::uint8_t
{
    all,
    prospero
};

const char *icon_source_key(IconSource source); // "catalogue" / "prospero"
bool parse_icon_source(std::string_view text, IconSource *out);
const char *hero_source_key(HeroSource source); // "all" / "prospero"
bool parse_hero_source(std::string_view text, HeroSource *out);

// https://cdn.prosperopatches.com/titles/PPSA12345_<64 hex>/pic0.webp, exactly (no query, no other host).
bool is_prospero_pic0_url(std::string_view url);
// The icon0.webp next to such a pic0.webp; "" for any other address.
std::string prospero_icon_url(std::string_view pic0_url);
// Whether a hero address may be used under `source`.
bool hero_url_allowed(std::string_view url, HeroSource source);

// The 16:9 picture address for a game: the first of its title ids whose record has one the source allows. "" none.
std::string choose_hero(const PopularityIndex &index, const std::vector<std::string> &title_ids, HeroSource source);

// Decides when the Store grid fetches a game's 16:9 picture ahead of time (the focused tile). The picture goes to
// the disk cache only, so that opening the page finds it there. Rules: focus has to rest on the same game for
// `kDwellS` (scrolling past fetches nothing), a game is fetched once per time focus lands on it, and at most
// one fetch every `kMinGapS` seconds (a long run of resting on tiles cannot pull megabytes).
class HeroPrefetchGate
{
  public:
    static constexpr double kDwellS = 0.5;
    static constexpr double kMinGapS = 6.0;
    // Every frame with the focused game's id ("" when no game is focused) and the time in seconds.
    // True when its picture should be fetched now.
    bool update(const std::string &game_id, double now_s);
    // The grid or the setting changed under it: forget what was fetched, so a game can be fetched again.
    void reset();

  private:
    std::string game_;
    double since_ = 0.0;
    bool done_ = false;
    double last_fetch_ = -1.0e9;
};

} // namespace orbit
