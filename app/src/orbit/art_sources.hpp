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

namespace orbit
{

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

} // namespace orbit
