// Wings TV app - Store grid "Tile details": when a tile shows its link tier pill and region chips.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Only the tier pill and the region chips follow this choice; the title, the
// installed state, the heart and the popularity score are always drawn.
// Kept in the Wings prefs file as "tile_details=always|focus|never".

#pragma once

#include <cstdint>
#include <string_view>

namespace orbit
{

enum class TileDetails : std::uint8_t
{
    always,
    on_focus, // the default: only the focused tile, fading in with its focus
    never,
};

constexpr int kTileDetailsCount = 3;

// The word kept in the prefs file.
const char *tile_details_key(TileDetails value);
// False (and *out unchanged) for an unknown word.
bool parse_tile_details(std::string_view key, TileDetails *out);
// The Settings row value (English, passed through tr by the caller).
const char *tile_details_label(TileDetails value);
// Always -> On focus -> Never -> Always.
TileDetails next_tile_details(TileDetails value);

// How opaque a tile's details are: 1 always, 0 never, the tile's focus (0..1) on focus.
float tile_detail_alpha(TileDetails value, float focus);
// The tier colours legend in the grid header: full when details are always
// shown, the focused tile's detail alpha on focus, hidden never.
float tier_legend_alpha(TileDetails value, float focused_tile_focus);

} // namespace orbit
