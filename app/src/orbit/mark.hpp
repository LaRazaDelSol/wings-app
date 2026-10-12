// Wings TV app - The Wings mark, rasterised once into a texture.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// An original placeholder: a bold "W" whose outer strokes read as wings. It
// is drawn on the CPU (anti-aliased by 4 x 4 supersampling) with real
// transparency, so it works over any background.

#pragma once

#include "gfx/draw_list.hpp"
#include "orbit/image.hpp"

namespace orbit::mark
{

// size x size white pixels on transparent; outer_alpha dims only the two
// outer strokes (the waiting screen draws a faint mark with a glow).
image::Pixels render(int size, float outer_alpha = 1.0f);

} // namespace orbit::mark
