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

// Two equally good pictures: for many games the data has Prosperopatches' picture (hero) AND a different IGDB
// screenshot (heroLow, small). Showing the small one first and then swapping to the other made the page flash
// one picture for half a second before another. Instead the page (and the Store's hero shot) shows ONE of them,
// whole and sharp, and the choice alternates: the picture a game showed last is not the one it shows next time.
// The IGDB variant lives under its own art key (art_key + kHeroSecondTail) so the two never share a texture.
constexpr const char *kHeroSecondTail = "~b";
// .../t_screenshot_big/<id>.jpg -> .../t_1080p/<id>.jpg (the same IGDB picture at full size); "" for any other
// address.
std::string igdb_full_url(std::string_view low_url);

struct HeroPick
{
    std::string full;      // the picture to show ("" none)
    std::string low;       // a small stand-in of THE SAME picture ("" none)
    bool second = false;   // the IGDB variant: its art key carries kHeroSecondTail
    bool has_both = false; // the game has the two different pictures, so the choice alternates
};
// use_second picks the IGDB picture when the game has both; otherwise it is Prosperopatches'. A game with only one
// kind of picture gets it, with its own small stand-in when the data has one (as before).
HeroPick pick_hero(const PopularityIndex &index, const std::vector<std::string> &title_ids, HeroSource source,
                   bool use_second);
// The art key a pick's pictures are cached under.
std::string hero_art_key(const std::string &art_key, bool second);

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
