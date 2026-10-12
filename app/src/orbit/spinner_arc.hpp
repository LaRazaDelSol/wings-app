// Wings TV app - A small spinner drawn from shapes: a faint ring and a turning arc.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/draw_list.hpp"

namespace orbit
{

// Centre (cx, cy), outer radius r, time in seconds (about 0.9 turns per second).
inline void draw_spinner_arc(hui::gfx::DrawList &list, float cx, float cy, float r, double clock,
                             hui::gfx::Color color)
{
    const float thick = r * 0.28f;
    list.ring(cx, cy, r, thick, color.with_alpha(0.2f));
    list.arc(cx, cy, r, thick, static_cast<float>(clock * 5.6), 1.7f, color);
}

} // namespace orbit
