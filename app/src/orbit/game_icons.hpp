// Wings TV app - Simple shape icons for the game page's facts panel and rating row.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Line art from rounded rectangles, rings and lines, in the same weight as the icons in ui.hpp
// (which are left as they are). size is the icon's box; cx, cy its centre.

#pragma once

#include "orbit/ui.hpp"

namespace orbit::ui
{

void icon_source(DrawList &list, float cx, float cy, float size, Color ink);  // a server: stacked bars
void icon_disk(DrawList &list, float cx, float cy, float size, Color ink);    // size: a drive
void icon_version(DrawList &list, float cx, float cy, float size, Color ink); // a tag
void icon_chip(DrawList &list, float cx, float cy, float size, Color ink);    // firmware: a chip with pins
void icon_globe(DrawList &list, float cx, float cy, float size, Color ink);   // region
void icon_files(DrawList &list, float cx, float cy, float size, Color ink);   // parts: two sheets
void icon_link(DrawList &list, float cx, float cy, float size, Color ink);    // two chained links
// Five stars, `value` (0..5) filled from the left; returns the width. radius is one star's.
float rating_stars(DrawList &list, float x, float cy, float radius, float value, Color fill, Color empty);

} // namespace orbit::ui
