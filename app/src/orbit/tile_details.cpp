// Wings TV app - Store grid "Tile details".
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/tile_details.hpp"

#include <algorithm>

namespace orbit
{

const char *tile_details_key(TileDetails value)
{
    switch (value)
    {
    case TileDetails::always:
        return "always";
    case TileDetails::never:
        return "never";
    case TileDetails::on_focus:
        break;
    }
    return "focus";
}

bool parse_tile_details(std::string_view key, TileDetails *out)
{
    for (int i = 0; i < kTileDetailsCount; ++i)
    {
        const TileDetails value = static_cast<TileDetails>(i);
        if (key == tile_details_key(value))
        {
            *out = value;
            return true;
        }
    }
    return false;
}

const char *tile_details_label(TileDetails value)
{
    switch (value)
    {
    case TileDetails::always:
        return "Always";
    case TileDetails::never:
        return "Never";
    case TileDetails::on_focus:
        break;
    }
    return "On focus";
}

TileDetails next_tile_details(TileDetails value)
{
    return static_cast<TileDetails>((static_cast<int>(value) + 1) % kTileDetailsCount);
}

float tile_detail_alpha(TileDetails value, float focus)
{
    switch (value)
    {
    case TileDetails::always:
        return 1.0f;
    case TileDetails::never:
        return 0.0f;
    case TileDetails::on_focus:
        break;
    }
    return std::clamp(focus, 0.0f, 1.0f);
}

float tier_legend_alpha(TileDetails value, float focused_tile_focus)
{
    return tile_detail_alpha(value, focused_tile_focus);
}

} // namespace orbit
