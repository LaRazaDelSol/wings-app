// Wings TV app - The first-run choices' layout: bounds, spacing, no overlap, contrast. Pure maths.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_offer_layout.hpp"
#include "orbit/ui.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace orbit
{
namespace
{

using hui::gfx::Rect;

bool overlap(const Rect &a, const Rect &b)
{
    return a.w > 0 && b.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The block's fixed rows in draw_waiting / PegasusOffer::draw: the last card line, the status line.
constexpr float kLastCardLineY = 370.0f + 150.0f + 178.0f; // draw_waiting top 330 + 40, size 150
constexpr float kStatusLineY = 790.0f;

void expect_inside(const OfferLayout &l, int first)
{
    for (int i = first; i < 3; ++i)
    {
        EXPECT_GE(l.choice[i].x, 40.0f);
        EXPECT_LE(l.choice[i].x + l.choice[i].w, 1920.0f - 40.0f);
        EXPECT_GT(l.choice[i].y, kStatusLineY);
        EXPECT_LT(l.choice[i].y + l.choice[i].h, kOfferHintTop);
    }
    EXPECT_LT(l.source_y + 8.0f, kOfferHintTop); // descender of the source line
    EXPECT_GT(l.source_y - 19.0f, l.choice[first].y + l.choice[first].h);
}

TEST(PegasusOfferLayout, ThreeChoicesFormOneCentredRowInsideTheMargins)
{
    const float w[3] = {440.0f, 400.0f, 470.0f}; // roughly 20 px labels with pill padding
    const OfferLayout l = offer_layout(w, 0, 820.0f);
    expect_inside(l, 0);
    EXPECT_FLOAT_EQ(l.scale, 1.0f);
    EXPECT_FALSE(overlap(l.choice[0], l.choice[1]));
    EXPECT_FALSE(overlap(l.choice[1], l.choice[2]));
    EXPECT_FLOAT_EQ(l.choice[1].x - (l.choice[0].x + l.choice[0].w), kOfferGap);
    const float left = l.choice[0].x;
    const float right = 1920.0f - (l.choice[2].x + l.choice[2].w);
    EXPECT_NEAR(left, right, 0.01f);
    EXPECT_FLOAT_EQ(l.choice[0].y, l.choice[2].y);
}

TEST(PegasusOfferLayout, AnOversizedRowIsNarrowedNotPushedOffScreen)
{
    const float w[3] = {900.0f, 800.0f, 900.0f};
    const OfferLayout l = offer_layout(w, 0, 820.0f);
    EXPECT_LT(l.scale, 1.0f);
    expect_inside(l, 0);
    EXPECT_FALSE(overlap(l.choice[0], l.choice[1]));
    EXPECT_FALSE(overlap(l.choice[1], l.choice[2]));
    EXPECT_GE(l.choice[0].x, kOfferSideMargin - 0.01f);
}

TEST(PegasusOfferLayout, ManualOnlyShowsTheSingleButtonCentred)
{
    const float w[3] = {0.0f, 0.0f, 470.0f};
    const OfferLayout l = offer_layout(w, 2, 820.0f);
    expect_inside(l, 2);
    EXPECT_EQ(l.choice[0].w, 0.0f);
    EXPECT_EQ(l.choice[1].w, 0.0f);
    EXPECT_NEAR(l.choice[2].cx(), 960.0f, 0.01f);
}

TEST(PegasusOfferLayout, TheBlockClearsTheWaitingCardAndTheHintBar)
{
    const float w[3] = {440.0f, 400.0f, 470.0f};
    const OfferLayout l = offer_layout(w, 0, kStatusLineY + 30.0f);
    EXPECT_GT(kStatusLineY - 23.0f, kLastCardLineY + 8.0f);
    EXPECT_GT(l.choice[0].y, kStatusLineY + 6.0f);
    EXPECT_LT(l.source_y, kOfferHintTop - 40.0f);
}

float luminance(const ui::Color &c)
{
    const auto lin = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

TEST(PegasusOfferLayout, TheGrayChoiceTextKeepsContrastOverTheBackground)
{
    // Worst case: the lightest base (night2) plus the 5 percent white pill under the label.
    const ui::Color white = ui::Color::rgb(0xffffff);
    const ui::Color pill = hui::gfx::mix(ui::color::night2, white, 0.05f);
    const float ratio = (luminance(ui::color::text3) + 0.05f) / (luminance(pill) + 0.05f);
    EXPECT_GE(ratio, 4.5f);
    const float failed = (luminance(ui::color::tier_red) + 0.05f) / (luminance(pill) + 0.05f);
    EXPECT_GE(failed, 4.5f); // the failure line
}

} // namespace
} // namespace orbit
