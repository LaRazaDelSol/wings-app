// Wings TV app - Simple shape icons: see game_icons.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_icons.hpp"

#include <algorithm>

namespace orbit::ui
{

namespace
{
constexpr float kStroke = 2.0f;

void outline(DrawList &list, const Rect &r, float radius, Color ink)
{
    list.bordered_rect(r, radius, Color{0.0f, 0.0f, 0.0f, 0.0f}, kStroke, ink);
}
} // namespace

void icon_source(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float w = size * 0.84f;
    const float h = size * 0.36f;
    const float x = cx - w * 0.5f;
    for (int i = 0; i < 2; ++i)
    {
        const float y = cy - size * 0.44f + static_cast<float>(i) * (h + size * 0.08f);
        outline(list, {x, y, w, h}, size * 0.1f, ink);
        list.circle(x + w - size * 0.16f, y + h * 0.5f, size * 0.04f, ink);
    }
}

void icon_disk(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float w = size * 0.88f;
    const float h = size * 0.5f;
    outline(list, {cx - w * 0.5f, cy - h * 0.5f, w, h}, size * 0.12f, ink);
    list.circle(cx + w * 0.5f - size * 0.18f, cy, size * 0.045f, ink);
    list.line(cx - w * 0.5f + size * 0.14f, cy, cx - size * 0.04f, cy, kStroke, ink);
}

void icon_version(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float w = size * 0.88f;
    const float h = size * 0.52f;
    outline(list, {cx - w * 0.5f, cy - h * 0.5f, w, h}, size * 0.12f, ink);
    list.circle(cx - w * 0.5f + size * 0.22f, cy, size * 0.06f, ink);
    list.line(cx - size * 0.02f, cy, cx + w * 0.5f - size * 0.14f, cy, kStroke, ink);
}

void icon_chip(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float body = size * 0.56f;
    outline(list, {cx - body * 0.5f, cy - body * 0.5f, body, body}, size * 0.08f, ink);
    const float reach = size * 0.5f;
    for (int i = -1; i <= 1; ++i)
    {
        const float o = static_cast<float>(i) * body * 0.3f;
        list.line(cx + o, cy - body * 0.5f, cx + o, cy - reach, kStroke, ink);
        list.line(cx + o, cy + body * 0.5f, cx + o, cy + reach, kStroke, ink);
        list.line(cx - body * 0.5f, cy + o, cx - reach, cy + o, kStroke, ink);
        list.line(cx + body * 0.5f, cy + o, cx + reach, cy + o, kStroke, ink);
    }
}

void icon_globe(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float r = size * 0.44f;
    list.ring(cx, cy, r, kStroke, ink);
    list.line(cx - r, cy, cx + r, cy, kStroke, ink);
    list.line(cx, cy - r, cx, cy + r, kStroke, ink);
    list.ring(cx, cy, r * 0.5f, kStroke, ink);
}

void icon_files(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float w = size * 0.5f;
    const float h = size * 0.64f;
    outline(list, {cx - w * 0.5f - size * 0.12f, cy - h * 0.5f - size * 0.08f, w, h}, size * 0.08f, ink);
    outline(list, {cx - w * 0.5f + size * 0.12f, cy - h * 0.5f + size * 0.08f, w, h}, size * 0.08f, ink);
}

void icon_link(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float w = size * 0.56f;
    const float h = size * 0.32f;
    outline(list, {cx - w + size * 0.1f, cy - h * 0.5f, w, h}, h * 0.5f, ink);
    outline(list, {cx - size * 0.1f, cy - h * 0.5f, w, h}, h * 0.5f, ink);
}

float rating_stars(DrawList &list, float x, float cy, float radius, float value, Color fill, Color empty)
{
    const float pitch = radius * 2.3f;
    for (int i = 0; i < 5; ++i)
    {
        const float cx = x + radius + static_cast<float>(i) * pitch;
        list.star(cx, cy, radius, empty);
        const float part = std::clamp(value - static_cast<float>(i), 0.0f, 1.0f);
        if (part > 0.0f)
        {
            list.push_clip({cx - radius, cy - radius, radius * 2.0f * part, radius * 2.0f});
            list.star(cx, cy, radius, fill);
            list.pop_clip();
        }
    }
    return pitch * 4.0f + radius * 2.0f;
}

} // namespace orbit::ui
