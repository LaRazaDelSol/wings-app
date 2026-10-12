// Wings TV app - The storefront's look: tokens and drawing helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same design language as the browser storefront (docs/design.md): black
// layers, art that fills the screen, one white focus ring with a soft white
// glow, light large titles, pill buttons. Sizes are virtual 1920 x 1080 px.

#pragma once

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::ui
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

namespace color
{
inline const Color night0 = Color::rgb(0x000000);
inline const Color night1 = Color::rgb(0x080808);
inline const Color night2 = Color::rgb(0x151515);
inline const Color night3 = Color::rgb(0x242424);
inline const Color glow = Color::rgb(0x181818);
inline const Color text = Color::rgb(0xffffff);
inline const Color text2 = Color::rgb(0xd0d0d0);
inline const Color text3 = Color::rgb(0xa1a1a1);
inline const Color action = Color::rgb(0xffffff);
inline const Color action_hi = Color::rgb(0xe0e0e0);
inline const Color ok = Color::rgb(0xffffff);
inline const Color bad = Color::rgb(0xffffff);
inline const Color line = Color::rgb(0xffffff, 0.16f);
// Link support tiers, the colours Pegasus DL's own page uses for them
// (--ok, the debrid badge, --bad). Always paired with a text label.
inline const Color tier_green = Color::rgb(0x5ce0bc);
inline const Color tier_yellow = Color::rgb(0xf4c95d);
inline const Color tier_red = Color::rgb(0xff6b9d);
// Fields are tonal surfaces, not outlines (the browser's --field tokens).
inline const Color field = Color::rgb(0xffffff, 0.075f);
inline const Color field_open = Color::rgb(0xffffff, 0.145f);
inline const Color field_edge = Color::rgb(0xffffff, 0.06f);
// Over game art they are dark glass, so text stays readable on bright covers.
inline const Color glass = Color::rgb(0x080808, 0.62f);
inline const Color glass_on = Color::rgb(0x202020, 0.82f);
// An open list: a frosted panel over the screen.
inline const Color popover = Color::rgb(0x141414, 0.9f);
} // namespace color

constexpr float kGutter = 100.0f;     // left and right page margin
constexpr float kArtRadius = 14.0f;   // covers and art
constexpr float kPanelRadius = 22.0f; // dialogs

// The four faces the app draws with (all Inter, SIL Open Font License).
struct Type
{
    hui::ui::FontRef light;    // Inter Display Light: large titles
    hui::ui::FontRef regular;  // body
    hui::ui::FontRef medium;   // tabs, buttons
    hui::ui::FontRef semibold; // headings, labels
};

// One line of text; x is the left edge, centre or right edge. Returns its width.
float text(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
           float baseline, float size, Color color, Align align = Align::left);
// The same, shortened with an ellipsis to fit max_width.
float text_fit(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
               float baseline, float size, float max_width, Color color, Align align = Align::left);
// The largest size from `size` down to `smallest` at which value fits max_width.
float fit_size(const hui::ui::FontRef &font, std::string_view value, float size, float smallest,
               float max_width);
// Wrapped text from its first baseline; returns the baseline after the last line.
float paragraph(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
                float baseline, float size, float width, float line_height, Color color,
                int max_lines, Align align = Align::left);

// The focus ring: white, 3 px, outside the shape, with a soft white glow.
// amount (0..1) fades it in and out. Draw it before the shape it surrounds,
// so the glow sits behind it like a box-shadow.
void focus_ring(DrawList &list, const Rect &r, float radius, float amount);

enum class ButtonKind : std::uint8_t
{
    primary,   // white action with dark text
    light,     // white storefront action
    secondary, // translucent white
    danger,
};
// The browser's .primary: 0.75 rem padding around a 1 rem label.
constexpr float kButtonHeight = 54.0f;
float button_width(const Type &type, std::string_view label, float size = 20.0f,
                   ButtonKind kind = ButtonKind::primary);
// A pill button. focus (0..1) draws the ring and grows a primary button.
void button(DrawList &list, const Type &type, const Rect &r, std::string_view label,
            ButtonKind kind, float focus, bool enabled = true, float size = 20.0f);

// A small rounded label ("PS5", a genre, a status). Returns its width.
float chip(DrawList &list, const Type &type, float x, float cy, std::string_view label, Color fill,
           Color ink, float size = 20.0f, Color border = Color{0, 0, 0, 0});

// The browser's .platform badge ("PS5"): outlined in white, 6 px corners.
// Returns its width.
float platform_badge(DrawList &list, const Type &type, float x, float cy, std::string_view label);

// Draws a texture to fill r, cropping the overflow (CSS object-fit: cover);
// anchor_x/y place the crop as object-position does (0.5 centres it).
void image_cover(DrawList &list, std::uint32_t texture, int width, int height, const Rect &r,
                 Color tint, float radius = 0.0f, float anchor_x = 0.5f, float anchor_y = 0.5f);
// What a cover shows before (or instead of) its art: a dark plate with the title.
void cover_placeholder(DrawList &list, const Type &type, const Rect &r, std::string_view title,
                       float radius);

void progress_bar(DrawList &list, const Rect &r, float fraction, Color fill);

// The browser's .modal: a dark panel with a hairline edge and a deep shadow.
void modal(DrawList &list, const Rect &r);

// A field's surface: the fill, with a hairline of light along its top edge.
void surface(DrawList &list, const Rect &r, float radius, Color fill);

// The browser's Select. A .field label above the trigger, when given.
constexpr float kFieldHeight = 58.0f; // .select-trigger: 2.9 rem
void field_label(DrawList &list, const Type &type, const Rect &field, std::string_view label);
enum class FieldLook : std::uint8_t
{
    box,   // forms and dialogs
    pill,  // filter rows, beside the search pill
    glass, // over game art
};
// .select-trigger: the chosen value in a tonal field, with a chevron.
void select_field(DrawList &list, const Type &type, const Rect &r, std::string_view value,
                  float focus, bool open = false, bool enabled = true,
                  FieldLook look = FieldLook::box);
// .select-menu under (or, near the bottom, over) its trigger: the chosen
// option has a check, the controller's option is lit white. in fades it.
// With the frame's glass texture (set frame.glass) the panel is frosted.
void select_menu(DrawList &list, const Type &type, const Rect &trigger,
                 const std::vector<std::string> &options, int selected, int active, float in,
                 std::uint32_t glass_texture = 0);

// Line-art icons drawn from shapes.
void icon_search(DrawList &list, float cx, float cy, float size, Color ink);
void icon_settings(DrawList &list, float cx, float cy, float size, Color ink);
void icon_check(DrawList &list, float cx, float cy, float size, Color ink, float thickness = 3.0f);
void icon_download(DrawList &list, float cx, float cy, float size, Color ink);
void icon_chevron(DrawList &list, float cx, float cy, float size, Color ink, bool up = false);
void icon_back(DrawList &list, float cx, float cy, float size, Color ink);
void icon_heart(DrawList &list, float cx, float cy, float size, Color ink);
// The same heart, filled (a favourite).
void icon_heart_filled(DrawList &list, float cx, float cy, float size, Color fill);
void icon_layers(DrawList &list, float cx, float cy, float size, Color ink);

// A hint row in the bottom-right corner, drawn as the browser's footer.
void hints(DrawList &list, const Type &type, const std::vector<hui::ui::Hint> &items);

// A value that eases toward a target (focus fades, scrolling, crossfades).
struct Ease
{
    float value = 0.0f;
    float target = 0.0f;
    void snap(float v)
    {
        value = target = v;
    }
    void update(float dt, float rate = 14.0f);
};

} // namespace orbit::ui
