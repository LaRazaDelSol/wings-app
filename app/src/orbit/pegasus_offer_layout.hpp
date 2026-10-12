// Wings TV app - Where the first-run choices sit under "Waiting for Pegasus DL" (pure maths).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No fonts, no drawing: label widths in, rectangles out, so the bounds are a unit test.
// Virtual 1920 x 1080 px. The choices form one row, centred, with the source line below.
// The hint bar starts at y = 1000 and nothing of the block may reach it.
#pragma once

#include "gfx/draw_list.hpp"

namespace orbit
{

struct OfferLayout
{
    hui::gfx::Rect choice[3]; // empty (w = 0) for a choice that is not offered
    float source_y = 0.0f;    // baseline of the "Source: ..." line
    float scale = 1.0f;       // < 1 when the row had to be narrowed to fit
};

inline constexpr float kOfferScreenW = 1920.0f;
inline constexpr float kOfferSideMargin = 60.0f; // at least 40 px from each screen edge
inline constexpr float kOfferGap = 24.0f;
inline constexpr float kOfferButtonH = 54.0f;
inline constexpr float kOfferSourceDrop = 36.0f; // from the buttons' bottom to the source baseline
inline constexpr float kOfferHintTop = 1000.0f;

// label_w: the width each button wants (index 0 Enable or Retry, 1 Once, 2 Manual); first: the first one
// shown; a width of 0 leaves that button out (Retry and the gray choice show slots 0 and 2).
// top: y of the row. A row wider than the screen minus both margins is narrowed, never wrapped.
inline OfferLayout offer_layout(const float label_w[3], int first, float top)
{
    OfferLayout out;
    float total = 0.0f;
    int shown = 0;
    for (int i = first; i < 3; ++i)
    {
        if (label_w[i] <= 0.0f)
            continue;
        total += label_w[i] + (shown > 0 ? kOfferGap : 0.0f);
        ++shown;
    }
    const float avail = kOfferScreenW - 2.0f * kOfferSideMargin;
    if (total > avail)
    {
        // The gaps stay; the buttons give way (their text is fitted into what they get).
        const float gaps = kOfferGap * static_cast<float>(shown > 0 ? shown - 1 : 0);
        out.scale = (avail - gaps) / (total - gaps);
        total = avail;
    }
    float x = kOfferScreenW * 0.5f - total * 0.5f;
    for (int i = first; i < 3; ++i)
    {
        if (label_w[i] <= 0.0f)
            continue;
        const float w = label_w[i] * out.scale;
        out.choice[i] = {x, top, w, kOfferButtonH};
        x += w + kOfferGap;
    }
    out.source_y = top + kOfferButtonH + kOfferSourceDrop;
    return out;
}

} // namespace orbit
