// Wings TV app - Horizontal marquee offset for a value wider than its box (pure maths).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time and widths in, a left shift out, so the timing is a unit test. The text rests at its start,
// scrolls to its end at a steady speed, rests there, then jumps back and loops. A value that fits
// never moves. The caller clips to the value's rect and draws the whole text at x - offset.
#pragma once

#include <cmath>

namespace orbit
{

inline constexpr float kMarqueePauseS = 1.2f;
inline constexpr float kMarqueeSpeed = 60.0f; // px per second

inline float marquee_offset(double t, float text_w, float box_w, float pause = kMarqueePauseS,
                            float speed = kMarqueeSpeed)
{
    if (!(text_w > box_w) || box_w < 0.0f || speed <= 0.0f)
        return 0.0f;
    const float travel = text_w - box_w;
    const float scroll_s = travel / speed;
    const double period = 2.0 * pause + scroll_s;
    const double ph = std::fmod(t < 0.0 ? 0.0 : t, period);
    if (ph < pause)
        return 0.0f;
    if (ph < pause + scroll_s)
        return static_cast<float>(ph - pause) * speed;
    return travel;
}

} // namespace orbit
