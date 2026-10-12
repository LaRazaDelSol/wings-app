// Wings TV app - The storefront's look: tokens and drawing helpers.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/ui.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::ui
{

float text(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
           float baseline, float size, Color color, Align align)
{
    return hui::ui::text(list, font, value, x, baseline, size, color, align);
}

float text_fit(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
               float baseline, float size, float max_width, Color color, Align align)
{
    if (font.measure(value, size) <= max_width)
        return orbit::ui::text(list, font, value, x, baseline, size, color, align);
    const std::string fitted = font.font->fit(value, size, max_width);
    return orbit::ui::text(list, font, fitted, x, baseline, size, color, align);
}

float fit_size(const hui::ui::FontRef &font, std::string_view value, float size, float smallest,
               float max_width)
{
    while (size > smallest && font.measure(value, size) > max_width)
        size -= 2.0f;
    return size;
}

float paragraph(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
                float baseline, float size, float width, float line_height, Color color,
                int max_lines, Align align)
{
    return hui::ui::paragraph(list, font, value, x, baseline, size, width, line_height, color,
                              max_lines, align);
}

void focus_ring(DrawList &list, const Rect &r, float radius, float amount)
{
    if (amount <= 0.01f)
        return;
    // The browser's --ring: 3 px of white hugging the shape, then the glow.
    list.glow(r.inset(-4.0f), radius + 4.0f, 13.0f, Color::rgb(0xffffff, 0.33f * amount));
    list.bordered_rect(r.inset(-3.0f), radius + 3.0f, Color{0, 0, 0, 0}, 3.0f,
                       Color::rgb(0xffffff, amount));
}

float button_width(const Type &type, std::string_view label, float size, ButtonKind kind)
{
    // .primary: 2 rem of padding each side and an 11 rem minimum; other
    // buttons 1.4 rem of padding.
    if (kind == ButtonKind::primary)
        return std::max(220.0f, type.medium.measure(label, size) + 2.0f * 40.0f);
    return type.medium.measure(label, size) + 2.0f * 28.0f;
}

void button(DrawList &list, const Type &type, const Rect &r, std::string_view label,
            ButtonKind kind, float focus, bool enabled, float size)
{
    // A focused primary button grows a little, as on the web; the ring follows it.
    const float scale = kind == ButtonKind::primary ? 1.0f + 0.04f * focus : 1.0f;
    list.push_transform(scale, r.cx(), r.cy(), 0.0f, 0.0f);
    Color fill;
    Color ink = color::text;
    switch (kind)
    {
    case ButtonKind::primary:
        fill = color::action;
        ink = color::night1;
        break;
    case ButtonKind::light:
        fill = color::text;
        ink = color::night0;
        break;
    case ButtonKind::danger:
        fill = Color::rgb(0xffffff, 0.16f);
        ink = Color::rgb(0xffffff);
        break;
    default:
        fill = Color::rgb(0xffffff, 0.1f);
        break;
    }
    if (!enabled)
    {
        fill = kind == ButtonKind::primary ? color::action.with_alpha(0.4f)
                                           : Color::rgb(0xffffff, 0.04f);
        ink = color::text.with_alpha(0.4f);
    }
    focus_ring(list, r, r.h * 0.5f, focus); // the glow goes behind the fill
    list.rounded_rect(r, r.h * 0.5f, fill);
    text_fit(list, type.medium, label, r.cx(), r.cy() + size * 0.36f, size, r.w - 32.0f, ink,
             Align::center);
    list.pop_transform();
}

float chip(DrawList &list, const Type &type, float x, float cy, std::string_view label, Color fill,
           Color ink, float size, Color border)
{
    const float width = type.semibold.measure(label, size) + 2.0f * 14.0f;
    const float height = size + 16.0f;
    const Rect r{x, cy - height * 0.5f, width, height};
    if (border.a > 0.0f)
        list.bordered_rect(r, 8.0f, fill, 1.5f, border);
    else
        list.rounded_rect(r, 8.0f, fill);
    orbit::ui::text(list, type.semibold, label, r.cx(), cy + size * 0.36f, size, ink,
                    Align::center);
    return width;
}

float platform_badge(DrawList &list, const Type &type, float x, float cy, std::string_view label)
{
    constexpr float kSize = 14.4f; // 0.72 rem, weight 600
    const float width = type.semibold.measure(label, kSize) + 2.0f * 9.0f + 3.0f;
    list.bordered_rect({x, cy - 12.0f, width, 24.0f}, 6.0f, Color{0, 0, 0, 0}, 1.5f, color::text);
    orbit::ui::text(list, type.semibold, label, x + width * 0.5f, cy + kSize * 0.36f, kSize,
                    color::text, Align::center);
    return width;
}

void image_cover(DrawList &list, std::uint32_t texture, int width, int height, const Rect &r,
                 Color tint, float radius, float anchor_x, float anchor_y)
{
    if (texture == 0 || width <= 0 || height <= 0 || r.w <= 0.0f || r.h <= 0.0f)
        return;
    const float image_aspect = static_cast<float>(width) / static_cast<float>(height);
    const float box_aspect = r.w / r.h;
    Rect uv = hui::gfx::kFullUv;
    if (image_aspect > box_aspect)
    {
        const float visible = box_aspect / image_aspect;
        uv.x = (1.0f - visible) * anchor_x;
        uv.w = visible;
    }
    else
    {
        const float visible = image_aspect / box_aspect;
        uv.y = (1.0f - visible) * anchor_y;
        uv.h = visible;
    }
    list.image(texture, r, uv, tint, radius);
}

void cover_placeholder(DrawList &list, const Type &type, const Rect &r, std::string_view title,
                       float radius)
{
    list.gradient_rect(r, radius, color::night3, color::night2);
    // The title's initial, large and quiet, until the art arrives.
    std::string initial;
    for (const char c : title)
    {
        if (c != ' ')
        {
            initial.push_back(c);
            break;
        }
    }
    orbit::ui::text(list, type.light, initial, r.cx(), r.cy() + r.h * 0.17f, r.h * 0.46f,
                    Color::rgb(0xffffff, 0.16f), Align::center);
}

void progress_bar(DrawList &list, const Rect &r, float fraction, Color fill)
{
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    list.rounded_rect(r, r.h * 0.5f, Color::rgb(0xffffff, 0.14f));
    if (fraction > 0.0f)
        list.rounded_rect({r.x, r.y, std::max(r.h, r.w * fraction), r.h}, r.h * 0.5f, fill);
}

void modal(DrawList &list, const Rect &r)
{
    list.shadow({r.x, r.y + 30.0f, r.w, r.h}, kPanelRadius, 90.0f, Color::rgb(0x000000, 0.6f));
    list.gradient_rect(r, kPanelRadius, Color::rgb(0x242424), Color::rgb(0x151515));
    list.bordered_rect(r, kPanelRadius, Color{0, 0, 0, 0}, 1.0f, color::line);
}

void field_label(DrawList &list, const Type &type, const Rect &field, std::string_view label)
{
    orbit::ui::text(list, type.regular, label, field.x, field.y - 12.0f, 16.0f, color::text2);
}

void surface(DrawList &list, const Rect &r, float radius, Color fill)
{
    list.rounded_rect(r, radius, fill);
    const float inset = std::min(radius, r.w * 0.5f);
    list.rounded_rect({r.x + inset, r.y, r.w - 2.0f * inset, 1.5f}, 0.75f, color::field_edge);
}

void select_field(DrawList &list, const Type &type, const Rect &r, std::string_view value,
                  float focus, bool open, bool enabled, FieldLook look)
{
    const float radius = look == FieldLook::pill    ? r.h * 0.5f
                         : look == FieldLook::glass ? 20.0f
                                                    : 14.0f;
    const float pad = look == FieldLook::pill ? 25.0f : 20.0f;
    focus_ring(list, r, radius, focus);
    if (look == FieldLook::glass)
        surface(list, r, radius, open ? color::glass_on : color::glass);
    else
        surface(list, r, radius, open ? color::field_open : color::field);
    const Color ink = enabled ? color::text : color::text3;
    text_fit(list, type.regular, value, r.x + pad, r.cy() + 6.5f, 18.0f, r.w - pad - 50.0f, ink);
    icon_chevron(list, r.x + r.w - 30.0f, r.cy(), 20.0f, color::text2, open);
}

void select_menu(DrawList &list, const Type &type, const Rect &trigger,
                 const std::vector<std::string> &options, int selected, int active, float in,
                 std::uint32_t glass_texture)
{
    if (in <= 0.01f || options.empty())
        return;
    constexpr float kRow = 55.0f;
    constexpr float kPad = 7.0f;
    constexpr float kRadius = 18.0f;
    constexpr int kMaxRows = 8;
    float width = trigger.w;
    for (const std::string &option : options)
        width = std::max(width, type.regular.measure(option, 18.0f) + 90.0f);
    width = std::min(width, 1920.0f - trigger.x - kGutter);
    const int rows = std::min(static_cast<int>(options.size()), kMaxRows);
    const float height = static_cast<float>(rows) * kRow + 2.0f * kPad;
    const bool below = trigger.y + trigger.h + 8.0f + height < 1000.0f;
    const Rect menu{trigger.x, below ? trigger.y + trigger.h + 8.0f : trigger.y - 8.0f - height,
                    width, height};
    // Long lists scroll to keep the controller's option in view.
    const int first = std::clamp(active - kMaxRows + 2, 0,
                                 std::max(0, static_cast<int>(options.size()) - kMaxRows));
    list.push_opacity(in);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (below ? -8.0f : 8.0f) * (1.0f - in));
    list.shadow({menu.x, menu.y + 22.0f, menu.w, menu.h}, kRadius, 60.0f,
                Color::rgb(0x000000, 0.55f));
    if (glass_texture != 0)
    {
        list.glass(glass_texture, menu, kRadius, Color::rgb(0xffffff));
        list.rounded_rect(menu, kRadius, color::popover);
    }
    else
    {
        list.rounded_rect(menu, kRadius, color::popover.with_alpha(0.97f));
    }
    list.bordered_rect(menu, kRadius, Color{0, 0, 0, 0}, 1.0f, Color::rgb(0xffffff, 0.08f));
    for (int r = 0; r < rows; ++r)
    {
        const int i = first + r;
        const Rect row{menu.x + kPad, menu.y + kPad + static_cast<float>(r) * kRow,
                       menu.w - 2.0f * kPad, kRow};
        const bool is_active = i == active;
        const bool is_selected = i == selected;
        // The controller's option is the cursor: solid white, like a focused tile.
        if (is_active)
            list.rounded_rect(row, 13.0f, Color::rgb(0xf4f4f4));
        const Color ink = is_active     ? Color::rgb(0x080808)
                          : is_selected ? color::text
                                        : color::text2;
        text_fit(list, is_selected ? type.medium : type.regular,
                 options[static_cast<std::size_t>(i)], row.x + 18.0f, row.cy() + 6.5f, 18.0f,
                 row.w - 70.0f, ink);
        if (is_selected)
            icon_check(list, row.x + row.w - 26.0f, row.cy(), 18.0f,
                       is_active ? color::night1 : color::text, 2.4f);
    }
    list.pop_transform();
    list.pop_opacity();
}

void icon_chevron(DrawList &list, float cx, float cy, float size, Color ink, bool up)
{
    const float w = size * 0.32f;
    const float h = size * 0.17f * (up ? -1.0f : 1.0f);
    const float t = std::max(1.8f, size * 0.09f);
    list.line(cx - w, cy - h, cx, cy + h, t, ink);
    list.line(cx, cy + h, cx + w, cy - h, t, ink);
}

void icon_back(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float t = std::max(2.0f, size * 0.09f);
    list.line(cx + size * 0.12f, cy - size * 0.3f, cx - size * 0.18f, cy, t, ink);
    list.line(cx - size * 0.18f, cy, cx + size * 0.12f, cy + size * 0.3f, t, ink);
}

void icon_heart(DrawList &list, float cx, float cy, float size, Color ink)
{
    // Sample a symmetrical heart outline in the same stroke as the other icons.
    float last_x = cx, last_y = cy - size * 5.0f / 32.0f;
    for (int i = 1; i <= 64; ++i)
    {
        const float t = static_cast<float>(i) * 6.2831853f / 64.0f;
        const float sn = std::sin(t);
        const float x = cx + size * 0.5f * sn * sn * sn;
        const float y = cy - size / 32.0f *
                                 (13.0f * std::cos(t) - 5.0f * std::cos(2.0f * t) -
                                  2.0f * std::cos(3.0f * t) - std::cos(4.0f * t));
        list.line(last_x, last_y, x, y, 2.0f, ink);
        last_x = x;
        last_y = y;
    }
}

void icon_heart_filled(DrawList &list, float cx, float cy, float size, Color fill)
{
    constexpr int kPoints = 48;
    float xy[kPoints * 2];
    for (int i = 0; i < kPoints; ++i)
    {
        const float t = static_cast<float>(i) * 6.2831853f / kPoints;
        const float sn = std::sin(t);
        xy[i * 2] = cx + size * 0.5f * sn * sn * sn;
        xy[i * 2 + 1] = cy - size / 32.0f *
                                 (13.0f * std::cos(t) - 5.0f * std::cos(2.0f * t) -
                                  2.0f * std::cos(3.0f * t) - std::cos(4.0f * t));
    }
    list.polygon(xy, kPoints, fill);
}

void icon_layers(DrawList &list, float cx, float cy, float size, Color ink)
{
    // Three stacked diamonds (the browser's "sources" icon).
    const float w = size * 0.4f;
    const float h = size * 0.2f;
    const float t = std::max(1.8f, size * 0.08f);
    list.line(cx - w, cy - h * 0.6f, cx, cy - h * 1.6f, t, ink);
    list.line(cx, cy - h * 1.6f, cx + w, cy - h * 0.6f, t, ink);
    list.line(cx + w, cy - h * 0.6f, cx, cy + h * 0.4f, t, ink);
    list.line(cx, cy + h * 0.4f, cx - w, cy - h * 0.6f, t, ink);
    list.line(cx - w, cy + h * 0.2f, cx, cy + h * 1.2f, t, ink);
    list.line(cx, cy + h * 1.2f, cx + w, cy + h * 0.2f, t, ink);
    list.line(cx - w, cy + h * 1.0f, cx, cy + h * 2.0f, t, ink);
    list.line(cx, cy + h * 2.0f, cx + w, cy + h * 1.0f, t, ink);
}

void icon_search(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float radius = size * 0.32f;
    const float thickness = std::max(2.0f, size * 0.09f);
    const float ox = cx - size * 0.08f;
    const float oy = cy - size * 0.08f;
    list.ring(ox, oy, radius, thickness, ink);
    const float d = radius * 0.72f;
    list.line(ox + d, oy + d, cx + size * 0.38f, cy + size * 0.38f, thickness, ink);
}

void icon_settings(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float thickness = std::max(2.0f, size * 0.09f);
    const float radius = size * 0.27f;
    // Eight teeth around a ring, and a hub.
    for (int i = 0; i < 8; ++i)
    {
        const float angle = static_cast<float>(i) * 0.785398f;
        const float tx = cx + std::sin(angle) * (radius + size * 0.08f);
        const float ty = cy - std::cos(angle) * (radius + size * 0.08f);
        list.rotated_rect({tx - size * 0.07f, ty - size * 0.08f, size * 0.14f, size * 0.16f},
                          size * 0.02f, angle, ink);
    }
    list.ring(cx, cy, radius + thickness * 0.5f, thickness * 1.3f, ink);
    list.ring(cx, cy, size * 0.11f, thickness, ink);
}

void icon_check(DrawList &list, float cx, float cy, float size, Color ink, float thickness)
{
    list.line(cx - size * 0.32f, cy + size * 0.02f, cx - size * 0.08f, cy + size * 0.26f, thickness,
              ink);
    list.line(cx - size * 0.08f, cy + size * 0.26f, cx + size * 0.34f, cy - size * 0.24f, thickness,
              ink);
}

void icon_download(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float thickness = std::max(2.0f, size * 0.09f);
    list.line(cx, cy - size * 0.36f, cx, cy + size * 0.12f, thickness, ink);
    list.line(cx - size * 0.22f, cy - size * 0.08f, cx, cy + size * 0.14f, thickness, ink);
    list.line(cx + size * 0.22f, cy - size * 0.08f, cx, cy + size * 0.14f, thickness, ink);
    list.line(cx - size * 0.34f, cy + size * 0.36f, cx + size * 0.34f, cy + size * 0.36f, thickness,
              ink);
}

namespace
{

constexpr float kHintY = 1038.0f;
constexpr float kHintGlyph = 30.0f;

const char *shoulder_name(hui::ui::Button button)
{
    switch (button)
    {
    case hui::ui::Button::l1:
        return "L1";
    case hui::ui::Button::r1:
        return "R1";
    case hui::ui::Button::l2:
        return "L2";
    case hui::ui::Button::r2:
        return "R2";
    case hui::ui::Button::options:
        return tr("Options");
    default:
        return nullptr;
    }
}

float glyph_width(const Type &type, hui::ui::Button button)
{
    if (const char *name = shoulder_name(button))
        return type.semibold.measure(name, 13.0f) + 18.0f;
    return kHintGlyph;
}

// The browser's hint glyph: the face button's symbol in a 1.5 px outlined
// circle, in the label's colour; shoulder buttons as an outlined pill.
float glyph(DrawList &list, const Type &type, hui::ui::Button button, float x, Color ink)
{
    const float width = glyph_width(type, button);
    const float cx = x + width * 0.5f;
    const float cy = kHintY;
    if (const char *name = shoulder_name(button))
    {
        list.bordered_rect({x, cy - 11.0f, width, 22.0f}, 6.0f, Color{0, 0, 0, 0}, 1.5f, ink);
        orbit::ui::text(list, type.semibold, name, cx, cy + 4.7f, 13.0f, ink, Align::center);
        return width;
    }
    list.ring(cx, cy, kHintGlyph * 0.5f - 0.75f, 1.5f, ink);
    const float s = 6.0f;
    switch (button)
    {
    case hui::ui::Button::cross:
        list.line(cx - s, cy - s, cx + s, cy + s, 1.6f, ink);
        list.line(cx - s, cy + s, cx + s, cy - s, 1.6f, ink);
        break;
    case hui::ui::Button::circle:
        list.ring(cx, cy, 7.0f, 1.6f, ink);
        break;
    case hui::ui::Button::square:
        list.bordered_rect({cx - 6.0f, cy - 6.0f, 12.0f, 12.0f}, 1.0f, Color{0, 0, 0, 0}, 1.6f,
                           ink);
        break;
    case hui::ui::Button::triangle:
        list.line(cx, cy - 7.0f, cx - 7.0f, cy + 5.0f, 1.6f, ink);
        list.line(cx - 7.0f, cy + 5.0f, cx + 7.0f, cy + 5.0f, 1.6f, ink);
        list.line(cx + 7.0f, cy + 5.0f, cx, cy - 7.0f, 1.6f, ink);
        break;
    case hui::ui::Button::dpad:
        // A plus of four arms.
        list.rounded_rect({cx - 2.0f, cy - 8.0f, 4.0f, 16.0f}, 1.0f, ink);
        list.rounded_rect({cx - 8.0f, cy - 2.0f, 16.0f, 4.0f}, 1.0f, ink);
        break;
    case hui::ui::Button::left_stick:
    case hui::ui::Button::right_stick:
        list.circle(cx, cy, 5.0f, ink);
        break;
    default:
        break;
    }
    return width;
}

} // namespace

void hints(DrawList &list, const Type &type, const std::vector<hui::ui::Hint> &items)
{
    // Right-aligned at the gutter, 1.6 rem apart, 0.85 rem labels.
    constexpr float kLabel = 17.0f;
    float x = 1920.0f - kGutter;
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        const float label = type.regular.measure(it->label, kLabel);
        float width = glyph_width(type, it->button) + 10.0f + label;
        if (it->second != hui::ui::Button::none)
            width += glyph_width(type, it->second) + 6.0f;
        float at = x - width;
        at += glyph(list, type, it->button, at, color::text);
        if (it->second != hui::ui::Button::none)
            at += 6.0f + glyph(list, type, it->second, at + 6.0f, color::text);
        orbit::ui::text(list, type.regular, it->label, at + 10.0f, kHintY + 6.0f, kLabel,
                        color::text);
        x -= width + 32.0f;
    }
}

void Ease::update(float dt, float rate)
{
    const float k = 1.0f - std::exp(-rate * dt);
    value += (target - value) * k;
    if (std::fabs(target - value) < 0.0005f)
        value = target;
}

} // namespace orbit::ui
