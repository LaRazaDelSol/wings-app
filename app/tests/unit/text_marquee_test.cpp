// Wings TV app - The marquee offset for values wider than their box.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/text_marquee.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

TEST(Marquee, AValueThatFitsNeverMoves)
{
    for (double t = 0.0; t < 20.0; t += 0.7)
    {
        EXPECT_EQ(marquee_offset(t, 200.0f, 300.0f), 0.0f);
        EXPECT_EQ(marquee_offset(t, 300.0f, 300.0f), 0.0f);
    }
}

TEST(Marquee, RestsScrollsRestsAndLoops)
{
    // 360 px of text in 300 px: 60 px of travel, 1 s of scrolling at 60 px/s.
    const float text = 360.0f, box = 300.0f;
    EXPECT_EQ(marquee_offset(0.0, text, box), 0.0f);
    EXPECT_EQ(marquee_offset(1.1, text, box), 0.0f);          // still resting at the start
    EXPECT_NEAR(marquee_offset(1.2 + 0.5, text, box), 30.0f, 0.01f);
    EXPECT_NEAR(marquee_offset(1.2 + 1.0, text, box), 60.0f, 0.01f);
    EXPECT_NEAR(marquee_offset(1.2 + 1.0 + 1.1, text, box), 60.0f, 0.01f); // resting at the end
    // One period is 1.2 + 1.0 + 1.2 s; then it starts over.
    EXPECT_EQ(marquee_offset(3.4 + 0.5, text, box), 0.0f);
    EXPECT_NEAR(marquee_offset(3.4 + 1.2 + 0.5, text, box), 30.0f, 0.01f);
}

TEST(Marquee, NeverLeavesTheTextAndScrollsAtTheSteadySpeed)
{
    const float text = 900.0f, box = 250.0f;
    float last = 0.0f;
    for (double t = 0.0; t < 40.0; t += 0.05)
    {
        const float o = marquee_offset(t, text, box);
        EXPECT_GE(o, 0.0f);
        EXPECT_LE(o, text - box + 0.001f);
        if (o > last)
            EXPECT_LE(o - last, kMarqueeSpeed * 0.05f + 0.01f);
        last = o;
    }
}

} // namespace
} // namespace orbit
