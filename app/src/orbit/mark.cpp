// Wings TV app - The Wings mark, rasterised once into a texture.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/mark.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::mark
{

namespace
{

// The "W" in its 512-unit design space: four thick strokes between five points.
constexpr float kPoints[5][2] = {{70.0f, 150.0f},
                                 {170.0f, 372.0f},
                                 {256.0f, 214.0f},
                                 {342.0f, 372.0f},
                                 {442.0f, 150.0f}};
constexpr float kHalfStroke = 26.0f;

float segment_distance(float px, float py, const float a[2], const float b[2])
{
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float length2 = dx * dx + dy * dy;
    float t = length2 > 0.0f ? ((px - a[0]) * dx + (py - a[1]) * dy) / length2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = a[0] + t * dx - px;
    const float cy = a[1] + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

// How much of a sample point the mark covers: 1 on the inner strokes,
// outer_alpha on the two outer ones, 0 off the mark.
float cover(float u, float v, float outer_alpha)
{
    float best = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        if (segment_distance(u, v, kPoints[i], kPoints[i + 1]) <= kHalfStroke)
        {
            const bool outer = i == 0 || i == 3;
            best = std::max(best, outer ? outer_alpha : 1.0f);
        }
    }
    return best;
}

} // namespace

image::Pixels render(int size, float outer_alpha)
{
    image::Pixels out;
    if (size <= 0)
        return out;
    out.width = size;
    out.height = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const float scale = 512.0f / static_cast<float>(size);
    constexpr int kSamples = 4;
    for (int py = 0; py < size; ++py)
    {
        for (int px = 0; px < size; ++px)
        {
            float coverage = 0.0f;
            for (int sy = 0; sy < kSamples; ++sy)
            {
                for (int sx = 0; sx < kSamples; ++sx)
                {
                    const float u = (static_cast<float>(px) + (sx + 0.5f) / kSamples) * scale;
                    const float v = (static_cast<float>(py) + (sy + 0.5f) / kSamples) * scale;
                    coverage += cover(u, v, outer_alpha);
                }
            }
            coverage /= kSamples * kSamples;
            std::uint8_t *pixel = out.rgba.data() + (static_cast<std::size_t>(py) * size + px) * 4;
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] =
                static_cast<std::uint8_t>(std::clamp(coverage * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

} // namespace orbit::mark
