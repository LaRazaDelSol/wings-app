// Wings TV app - Colours and geometry of the game page header (the hero band).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Colour comes from theme.hpp: the hero sits under a veil in the Store
// place hue (so every game page is in one family, whatever the art), the info cards are neutral
// glass, and semantic colours appear only on values (game_header.cpp). Text colours come from
// ui::color so they stay readable on any art (the veils keep the art dark under the words).

#pragma once

#include "orbit/theme.hpp"
#include "orbit/ui.hpp"

namespace orbit::ui::game
{

// Veils over the hero art and over the cover-built fallback (layer 4: content under a fixed veil).
inline const Color indigo_deep = Color::rgb(0x05060f);   // darkest, the band's far end
inline const Color indigo = Color::rgb(0x0a0c1c);        // fallback band, top (near-black, hue-neutral)
inline const Color blue_wash = theme::tab_color(theme::place_store, 0.55f); // the Store hue veil over art
inline const Color veil_top = Color::rgb(0x03061a, 0.78f);  // under the app's top bar
inline const Color veil_left = Color::rgb(0x03061a, 0.72f); // behind the title and cards
inline const Color page_end = Color::rgb(0x080808);         // night1: where the band fades to

// Info cards: neutral glass tiles with a label and a value.
inline const Color card_fill = Color::rgb(0xffffff, 0.08f);
inline const Color card_edge = Color::rgb(0xffffff, 0.14f);
inline const Color card_label = ui::color::text2;
inline const Color card_value = Color::rgb(0xffffff);

// Rating bar: the "selected / active" accent.
inline const Color bar_track = Color::rgb(0xffffff, 0.14f);
inline const Color bar_fill = theme::semantic::info;

// Round and pill buttons beside Install: neutral glass.
inline const Color button_fill = Color::rgb(0xffffff, 0.12f);
inline const Color button_fill_focus = Color::rgb(0xffffff, 0.22f);

// Geometry (virtual 1920 x 1080 px).
constexpr float kBandHeight = 640.0f;     // hero art height; the list top in header mode may be lower
constexpr float kIconSize = 380.0f;       // the cover at the top left
constexpr float kIconTop = 124.0f;
constexpr float kTextGap = 52.0f;         // icon to title
constexpr float kCardHeight = 88.0f;
constexpr float kCardGap = 12.0f;
constexpr float kCardMinWidth = 188.0f;
constexpr int kCardsPerRow = 6;
constexpr float kButtonHeight = 64.0f;
constexpr float kListGap = 36.0f;         // header's last row to the list's top

} // namespace orbit::ui::game
