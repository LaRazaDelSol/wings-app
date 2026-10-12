// Wings TV app - The Downloads tab's after-download banner: what ShadowMount was asked, and
// the two controller choices. The state lives in AfterInstall (shadowmount_scan.*); this file
// only words it, draws it and takes the input while it has the focus.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/screens.hpp"

#include <algorithm>

namespace orbit
{

namespace
{
constexpr float kBannerHeight = 112.0f;
constexpr float kBannerGap = 18.0f;
} // namespace

std::string scan_banner_text(const ScanBanner &banner)
{
    switch (banner.phase)
    {
    case ScanPhase::deferred:
        return tr("Asked ShadowMount to find your new game. It waits while Wings is open. Close Wings "
                  "to finish.");
    case ScanPhase::scanning:
        return tr("ShadowMount is scanning now.");
    case ScanPhase::registered:
    {
        std::string names;
        for (const std::string &name : banner.registered)
            names += (names.empty() ? "" : ", ") + name;
        return tr("Registered: {title}", {{"title", names}});
    }
    case ScanPhase::unreachable:
        return tr("ShadowMount's API was not found. Close Wings and ShadowMount will find the game on "
                  "its next scan.");
    case ScanPhase::timed_out:
        return tr("ShadowMount has not listed the game yet. Close Wings and it will finish.");
    default:
        return {};
    }
}

std::string scan_banner_detail(const ScanBanner &banner)
{
    if (banner.phase == ScanPhase::deferred && banner.reason == "rest_mode")
        return tr("The console is in rest mode.");
    return {};
}

bool DownloadsScreen::banner_shown() const
{
    return ctx_.after_install != nullptr && ctx_.after_install->banner().visible();
}

float DownloadsScreen::banner_offset() const
{
    return banner_shown() ? kBannerHeight + kBannerGap : 0.0f;
}

void DownloadsScreen::banner_input(const InputFrame &input, Feedback &feedback, Intent &intent)
{
    const ScanBanner &banner = ctx_.after_install->banner();
    const int buttons = banner.offers_close() ? 2 : 1;
    banner_button_ = std::min(banner_button_, buttons - 1);
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int next = banner_button_ + (input.nav == Direction::right ? 1 : -1);
        if (next >= 0 && next < buttons)
        {
            banner_button_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    else if (input.nav == Direction::down)
    {
        if (!order_.empty())
        {
            banner_focus_ = false;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    else if (input.nav == Direction::up)
    {
        banner_focus_ = false;
        intent.kind = Intent::Kind::top_bar;
    }
    if (input.is_pressed(Action::confirm))
    {
        if (banner.offers_close() && banner_button_ == 0)
        {
            pending_ = Pending::close_wings;
            confirm_.open(tr("Close Wings?"),
                          tr("Close Wings and return to the home screen? ShadowMount will then register "
                             "your game."),
                          tr("Close Wings now"), tr("Not now"), false);
            feedback.play(hui::audio::Cue::modal_open);
        }
        else
        {
            ctx_.after_install->dismiss();
            banner_focus_ = false;
            feedback.play(hui::audio::Cue::select);
        }
    }
}

void DownloadsScreen::banner_draw(Frame &frame, bool focused) const
{
    if (!banner_shown())
        return;
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const ScanBanner &banner = ctx_.after_install->banner();
    const float width = 1920.0f - 2.0f * ui::kGutter;
    const ui::Rect r{ui::kGutter, layout::kListTop, width, kBannerHeight};
    ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.09f));

    // Buttons from the right edge; the words take the room that is left.
    const bool close = banner.offers_close();
    const char *first = close ? tr("Close Wings now") : tr("Dismiss");
    const char *second = tr("Not now");
    const float bw1 = ui::button_width(type, first, 20.0f, ui::ButtonKind::primary);
    const float bw2 = ui::button_width(type, second, 20.0f, ui::ButtonKind::secondary);
    float x = r.x + r.w - 28.0f;
    const float by = r.y + (r.h - ui::kButtonHeight) * 0.5f;
    if (close)
    {
        x -= bw2;
        ui::button(list, type, {x, by, bw2, ui::kButtonHeight}, second, ui::ButtonKind::secondary,
                   focused && banner_focus_ && banner_button_ == 1 ? 1.0f : 0.0f);
        x -= 16.0f;
    }
    x -= bw1;
    ui::button(list, type, {x, by, bw1, ui::kButtonHeight}, first,
               close ? ui::ButtonKind::primary : ui::ButtonKind::secondary,
               focused && banner_focus_ && banner_button_ == 0 ? 1.0f : 0.0f);

    const float text_w = x - r.x - 28.0f - 24.0f;
    const std::string detail = scan_banner_detail(banner);
    float baseline = ui::paragraph(list, type.regular, scan_banner_text(banner), r.x + 28.0f, r.y + 40.0f,
                                   22.0f, text_w, 30.0f, ui::color::text, 2);
    if (!detail.empty())
        ui::text_fit(list, type.regular, detail, r.x + 28.0f, baseline + 4.0f, 18.0f, text_w,
                     ui::color::text3);
}

} // namespace orbit
