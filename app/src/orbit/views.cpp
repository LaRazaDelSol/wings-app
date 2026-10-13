// Wings TV app - Drawing shared by the screens: covers, tiles, the waiting card.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Orbit Store's views.cpp (GPL-3.0-or-later).

#include "orbit/screens.hpp"
#include "orbit/spinner_arc.hpp"

#include <algorithm>
#include <cmath>

namespace orbit
{

namespace
{

// New art fades in over this long instead of popping.
constexpr float kArtFade = 0.25f;

float art_alpha(const Artwork &art)
{
    const double age = now_seconds() - art.ready_at;
    return std::clamp(static_cast<float>(age / kArtFade), 0.0f, 1.0f);
}

} // namespace

void draw_cover(Context &ctx, hui::gfx::DrawList &list, const std::string &art_key,
                const std::string &title, const ui::Rect &r, float radius, float opacity)
{
    const Artwork *art = art_key.empty() ? nullptr : ctx.art.get(art_key, ArtKind::cover);
    const float alpha = art != nullptr ? art_alpha(*art) : 0.0f;
    if (alpha < 1.0f)
    {
        list.push_opacity(opacity);
        ui::cover_placeholder(list, ctx.type, r, title, radius);
        list.pop_opacity();
    }
    if (art != nullptr)
        ui::image_cover(list, art->texture, art->width, art->height, r,
                        ui::Color::rgb(0xffffff, alpha * opacity), radius);
}

void draw_tile(Context &ctx, hui::gfx::DrawList &list, const std::string &art_key,
               const std::string &title, const ui::Rect &r, float focus, float opacity)
{
    // The ring first: its glow belongs behind the art, as a box-shadow is.
    ui::focus_ring(list, r, ui::kArtRadius, focus * opacity);
    draw_cover(ctx, list, art_key, title, r, ui::kArtRadius, opacity);
}

void draw_waiting(Context &ctx, hui::gfx::DrawList &list, float top)
{
    const ui::Type &type = ctx.type;
    const Snapshot &state = ctx.store.state();
    const float size = 150.0f;
    const float x = 960.0f - size * 0.5f;
    const float y = top + 40.0f;
    const bool connecting = state.link == Link::connecting;
    // A breathing glow behind the mark.
    const float breathe = 0.5f + 0.5f * std::sin(ctx.clock * 2.6f);
    list.glow({x + 30.0f, y + 30.0f, size - 60.0f, size - 60.0f}, size * 0.5f, 90.0f,
              ui::Color::rgb(0xffffff, 0.10f + 0.10f * breathe));
    if (ctx.loader_mark != 0)
        list.image(ctx.loader_mark, {x, y, size, size}, hui::gfx::kFullUv,
                   ui::Color::rgb(0xffffff, 0.85f));
    const std::string title =
        connecting ? std::string(tr("Connecting to Pegasus DL\xE2\x80\xA6"))
                   : tr("Waiting for Pegasus DL ({endpoint})", {{"endpoint", state.endpoint}});
    ui::text(list, type.light, title, 960.0f, y + size + 80.0f, 44.0f, ui::color::text,
             ui::Align::center);
    if (ctx.offer_showing)
    {
        // The choices below say how to get it; a raw socket error would only confuse here.
        if (ctx.offer_grace)
        {
            // Inside the grace: a small spinner and a plain line, no judgement yet.
            const char *looking = tr("Looking for Pegasus DL...");
            const float w = type.regular.measure(looking, 21.0f);
            const float left = 960.0f - (w + 26.0f) * 0.5f;
            draw_spinner_arc(list, left + 9.0f, y + size + 136.0f - 7.0f, 9.0f, ctx.clock, ui::color::text2);
            ui::text(list, type.regular, looking, left + 26.0f, y + size + 136.0f, 21.0f, ui::color::text3);
            return;
        }
        ui::text(list, type.regular,
                 tr("Pegasus DL is not answering yet. That is normal until it is started."), 960.0f,
                 y + size + 136.0f, 21.0f, ui::color::text3, ui::Align::center);
        return;
    }
    ui::text(list, type.regular,
             tr("Need it? Settings > Pegasus DL > Get Pegasus DL. Wings only downloads it when you ask."),
             960.0f, y + size + 136.0f, 21.0f, ui::color::text3, ui::Align::center);
    if (!state.link_error.empty())
        ui::text(list, type.regular, state.link_error, 960.0f, y + size + 172.0f, 21.0f,
                 ui::color::text3, ui::Align::center);
}

void draw_heading(Context &ctx, hui::gfx::DrawList &list, const char *title,
                  const std::string &summary)
{
    ui::text(list, ctx.type.light, title, ui::kGutter, layout::kTitleY, 56.0f, ui::color::text);
    if (!summary.empty())
        ui::text_fit(list, ctx.type.regular, summary, 1920.0f - ui::kGutter, layout::kTitleY - 4.0f,
                     24.0f, 900.0f, ui::color::text3, ui::Align::right);
}

void draw_busy(Context &ctx, hui::gfx::DrawList &list, const char *text)
{
    const ui::Type &type = ctx.type;
    // The words get the box minus the dots (130 left) and a margin (30 right): sized so they fit whole.
    const float width = std::max(520.0f, std::min(1840.0f, type.medium.measure(text, 26.0f) + 190.0f));
    const ui::Rect r{960.0f - width * 0.5f, 470.0f, width, 120.0f};
    ui::modal(list, r);
    // Three dots, breathing in turn.
    for (int i = 0; i < 3; ++i)
    {
        const float phase = 0.5f + 0.5f * std::sin(ctx.clock * 5.0f - static_cast<float>(i) * 0.9f);
        list.circle(r.x + 56.0f + static_cast<float>(i) * 22.0f, r.cy(), 7.0f,
                    ui::Color::rgb(0xffffff, 0.3f + 0.7f * phase));
    }
    ui::text_fit(list, type.medium, text, r.x + 130.0f, r.cy() + 9.0f, 26.0f, r.w - 160.0f,
                 ui::color::text);
}

const char *route_chip(LinkRoute route)
{
    switch (route)
    {
    case LinkRoute::native_pkg:
        return tr("PKG to PS5");
    case LinkRoute::debrid:
        return tr("Debrid");
    case LinkRoute::choose_debrid:
        return tr("Debrid");
    case LinkRoute::automated:
        return tr("Automatic");
    case LinkRoute::capture:
        return tr("Browser check");
    case LinkRoute::browser_only:
        return tr("Browser only");
    case LinkRoute::blocked:
        return tr("Unsupported");
    case LinkRoute::missing:
        return tr("No link");
    }
    return "";
}

ui::Color tier_color(LinkTier tier)
{
    switch (tier)
    {
    case LinkTier::native_pkg:
    case LinkTier::automated:
    case LinkTier::supported:
        return ui::color::tier_green;
    case LinkTier::debrid:
        return ui::color::tier_yellow;
    case LinkTier::unknown:
        return ui::color::text2;
    case LinkTier::unsupported:
        break;
    }
    return ui::color::tier_red;
}

float draw_tier_chip(const Context &ctx, hui::gfx::DrawList &list, float x, float cy,
                     LinkTier tier, float size)
{
    const ui::Color c = tier_color(tier);
    const bool neutral = tier == LinkTier::unknown;
    const ui::Color fill = neutral ? ui::Color::rgb(0xffffff, 0.08f) : c.with_alpha(0.17f);
    const ui::Color edge = neutral ? ui::Color::rgb(0xffffff, 0.22f) : c.with_alpha(0.7f);
    return ui::chip(list, ctx.type, x, cy, tier_label(tier), fill, c, size, edge);
}

float draw_tier_legend(const Context &ctx, hui::gfx::DrawList &list, float right_x, float baseline)
{
    const LinkTier items[] = {LinkTier::supported, LinkTier::debrid, LinkTier::unknown,
                              LinkTier::unsupported};
    const float size = 18.0f;
    // Measure first, then draw from the left so the group ends at right_x.
    float total = 0.0f;
    for (const LinkTier tier : items)
        total += 22.0f + ctx.type.regular.measure(tier_label(tier), size) + 26.0f;
    total -= 26.0f;
    float x = right_x - total;
    const float left = x;
    for (const LinkTier tier : items)
    {
        const ui::Color c = tier_color(tier);
        list.rounded_rect({x, baseline - 14.0f, 12.0f, 12.0f}, 6.0f, c);
        ui::text(list, ctx.type.regular, tier_label(tier), x + 22.0f, baseline, size,
                 ui::color::text2);
        x += 22.0f + ctx.type.regular.measure(tier_label(tier), size) + 26.0f;
    }
    return left;
}

int Picker::update(const InputFrame &input, int count, Feedback &feedback)
{
    if (!open || count <= 0)
        return -1;
    active = std::clamp(active, 0, count - 1);
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = active + (input.nav == Direction::down ? 1 : -1);
        if (next >= 0 && next < count)
        {
            active = next;
            feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    if (input.is_pressed(Action::confirm))
    {
        open = false;
        feedback.play(hui::audio::Cue::select);
        return active;
    }
    if (input.is_pressed(Action::back))
    {
        open = false;
        feedback.play(hui::audio::Cue::modal_close);
    }
    return -1;
}

void Picker::animate(float dt)
{
    in.target = open ? 1.0f : 0.0f;
    in.update(dt, 20.0f);
}

void refuse(Feedback &feedback, const InputFrame &input)
{
    if (!input.nav_repeat)
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.45f);
}

hui::ui::Theme keyboard_theme()
{
    hui::ui::Theme theme = hui::ui::themes()[0];
    for (const hui::ui::Theme &candidate : hui::ui::themes())
    {
        if (std::string_view(candidate.id) == "acrylic")
            theme = candidate;
    }
    theme.page = ui::color::night1;
    theme.surface = ui::color::night2.with_alpha(0.82f);
    theme.surface_high = ui::Color::rgb(0xffffff, 0.1f);
    theme.text = ui::color::text;
    theme.text_muted = ui::color::text2;
    theme.primary = ui::color::action;
    theme.on_primary = ui::color::night1;
    theme.secondary = ui::Color::rgb(0xffffff, 0.1f);
    theme.accent = ui::color::action;
    theme.focus = ui::color::text;
    theme.radius = 12.0f;
    theme.radius_card = ui::kPanelRadius;
    // Solid keys: glass keys would pick up the colours of the covers behind.
    theme.style = hui::ui::SurfaceStyle::flat;
    theme.surface_high = ui::color::night3;
    theme.secondary = ui::color::night3;
    return theme;
}

} // namespace orbit
