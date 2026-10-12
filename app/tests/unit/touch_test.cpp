// Wings TV app - Touchpad: the sample parser, gestures and key mapping.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic scePad buffers go through the header-only parser, then through
// InputTracker / TouchGestures, so the console only has to confirm the layout.

#include "core/input.hpp"
#include "core/touch.hpp"
#include "orbit/diagnostics.hpp"
#include "platform/ps5/pad_layout.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace hui;

namespace
{

// A 120-byte scePad buffer built field by field at the researched offsets.
struct RawBuffer
{
    unsigned char bytes[120] = {};
    RawBuffer()
    {
        put32(0x4c, 1); // connected
    }
    void put16(std::size_t off, std::uint16_t v)
    {
        std::memcpy(bytes + off, &v, 2);
    }
    void put32(std::size_t off, std::uint32_t v)
    {
        std::memcpy(bytes + off, &v, 4);
    }
    RawBuffer &touches(int n)
    {
        bytes[0x34] = static_cast<unsigned char>(n);
        return *this;
    }
    RawBuffer &finger(int i, int x, int y, int id)
    {
        const std::size_t base = 0x3c + static_cast<std::size_t>(i) * 8;
        put16(base, static_cast<std::uint16_t>(x));
        put16(base + 2, static_cast<std::uint16_t>(y));
        bytes[base + 4] = static_cast<unsigned char>(id);
        return *this;
    }
    RawBuffer &buttons(std::uint32_t b)
    {
        put32(0, b);
        return *this;
    }
    PadSample parse() const
    {
        PadSample s;
        EXPECT_TRUE(ps5::parse_buffer(bytes, sizeof(bytes), s));
        return s;
    }
};

PadSample one(int x, int y, int id = 1, std::uint32_t buttons = 0)
{
    return RawBuffer().touches(1).finger(0, x, y, id).buttons(buttons).parse();
}

PadSample none(std::uint32_t buttons = 0)
{
    return RawBuffer().buttons(buttons).parse();
}

PadSample two(int x0, int y0, int x1, int y1)
{
    return RawBuffer().touches(2).finger(0, x0, y0, 1).finger(1, x1, y1, 2).parse();
}

// Drives a tracker with a clock.
struct Rig
{
    InputTracker tracker;
    std::uint64_t now = 1000000;
    InputFrame step(const PadSample &s, std::uint64_t advance_us = 16000)
    {
        now += advance_us;
        return tracker.update(std::span<const PadSample>(&s, 1), now);
    }
    InputFrame idle(std::uint64_t advance_us)
    {
        now += advance_us;
        return tracker.update({}, now);
    }
};

struct Drag
{
    float scroll = 0.0f;
    std::uint32_t pressed = 0;
    InputFrame last;
};

// Moves a finger from a to b in equal steps.
Drag drag(Rig &r, int x0, int y0, int x1, int y1, int steps, std::uint64_t step_us,
          std::uint32_t buttons = 0)
{
    Drag d;
    for (int i = 0; i <= steps; ++i)
    {
        const int x = x0 + (x1 - x0) * i / steps;
        const int y = y0 + (y1 - y0) * i / steps;
        d.last = r.step(one(x, y, 1, buttons), step_us);
        d.scroll += d.last.touch_scroll;
        d.pressed |= d.last.pressed;
    }
    return d;
}

} // namespace

TEST(TouchLayout, OffsetsMatchTheResearch)
{
    EXPECT_EQ(sizeof(ps5::RawPadSample), 120u);
    EXPECT_EQ(offsetof(ps5::RawPadSample, touch_num), 0x34u);
    EXPECT_EQ(offsetof(ps5::RawPadSample, touch), 0x3cu);
    EXPECT_EQ(offsetof(ps5::RawPadSample, connected), 0x4cu);
}

TEST(TouchParse, OneFingerInTheMiddle)
{
    const PadSample s = one(960, 470, 7);
    EXPECT_EQ(s.touch_count, 1);
    EXPECT_TRUE(s.touch[0].down);
    EXPECT_EQ(s.touch[0].x, 960);
    EXPECT_EQ(s.touch[0].y, 470);
    EXPECT_EQ(s.touch[0].id, 7);
    EXPECT_FALSE(s.touch[1].down);
    EXPECT_FALSE(s.touch_bad);
    EXPECT_TRUE(s.connected);

    Rig rig;
    const InputFrame f = rig.step(s);
    EXPECT_TRUE(f.touch_active);
    EXPECT_TRUE(f.touch_began);
    EXPECT_EQ(f.touch_count, 1);
    EXPECT_NEAR(f.touch_x, 0.5f, 0.01f);
    EXPECT_NEAR(f.touch_y, 0.5f, 0.01f);
}

TEST(TouchParse, NoFingerMeansNoTouch)
{
    // Stale coordinates behind touchNum=0 must be ignored.
    RawBuffer raw;
    raw.touches(0).finger(0, 500, 500, 9);
    const PadSample s = raw.parse();
    EXPECT_EQ(s.touch_count, 0);
    EXPECT_FALSE(s.touch[0].down);
    Rig rig;
    const InputFrame f = rig.step(s);
    EXPECT_FALSE(f.touch_active);
    EXPECT_EQ(f.touch_count, 0);
}

TEST(TouchParse, TwoFingers)
{
    const PadSample s = two(100, 200, 1800, 900);
    EXPECT_EQ(s.touch_count, 2);
    EXPECT_TRUE(s.touch[1].down);
    EXPECT_EQ(s.touch[1].x, 1800);
    EXPECT_EQ(s.touch[1].id, 2);
    Rig rig;
    EXPECT_EQ(rig.step(s).touch_count, 2);
}

TEST(TouchParse, GarbageCountIsRejected)
{
    for (int n : {3, 7, 255})
    {
        const PadSample s = RawBuffer().touches(n).finger(0, 10, 10, 1).parse();
        EXPECT_EQ(s.touch_count, 0) << n;
        EXPECT_TRUE(s.touch_bad) << n;
        EXPECT_FALSE(s.touch[0].down) << n;
    }
    Rig rig;
    rig.step(RawBuffer().touches(200).parse());
    EXPECT_EQ(rig.tracker.touch_stats().bad, 1u);
    EXPECT_FALSE(rig.tracker.touch_stats().seen());
}

TEST(TouchParse, ShortOrMissingBufferIsRefused)
{
    unsigned char buffer[119] = {};
    PadSample s;
    s.buttons = 0x1234;
    EXPECT_FALSE(ps5::parse_buffer(buffer, sizeof(buffer), s));
    EXPECT_FALSE(ps5::parse_buffer(nullptr, 120, s));
    EXPECT_FALSE(ps5::parse_buffer(buffer, 0, s));
    EXPECT_EQ(s.buttons, 0x1234u); // untouched
}

TEST(TouchParse, RandomBytesNeverExceedTwoFingers)
{
    std::uint32_t seed = 12345;
    for (int round = 0; round < 500; ++round)
    {
        unsigned char buffer[120];
        for (unsigned char &b : buffer)
        {
            seed = seed * 1664525u + 1013904223u;
            b = static_cast<unsigned char>(seed >> 24);
        }
        PadSample s;
        ASSERT_TRUE(ps5::parse_buffer(buffer, sizeof(buffer), s));
        EXPECT_LE(s.touch_count, 2);
    }
}

TEST(TouchParse, ParsesTheRestOfTheSampleToo)
{
    RawBuffer raw;
    raw.buttons(pad_bits::kCross | pad_bits::kTouchpad);
    raw.bytes[4] = 10;
    raw.bytes[9] = 200;
    const PadSample s = raw.parse();
    EXPECT_EQ(s.buttons, pad_bits::kCross | pad_bits::kTouchpad);
    EXPECT_EQ(s.left_x, 10);
    EXPECT_EQ(s.r2, 200);
}

TEST(TouchParse, OutOfRangeCoordinatesAreClamped)
{
    Rig rig;
    const InputFrame f = rig.step(one(65535, 65535));
    EXPECT_FLOAT_EQ(f.touch_x, 1.0f);
    EXPECT_FLOAT_EQ(f.touch_y, 1.0f);
    const InputFrame g = rig.step(one(0, 0));
    EXPECT_FLOAT_EQ(g.touch_x, 0.0f);
    EXPECT_FLOAT_EQ(g.touch_y, 0.0f);
}

TEST(TouchParse, AChangedIdIsANewContact)
{
    Rig rig;
    rig.step(one(500, 500, 4));
    const InputFrame same = rig.step(one(510, 500, 4));
    EXPECT_FALSE(same.touch_began);
    const InputFrame swapped = rig.step(one(520, 500, 5));
    EXPECT_TRUE(swapped.touch_began);
    EXPECT_TRUE(swapped.touch_ended);
    EXPECT_EQ(rig.tracker.touch_stats().contacts, 2u);
    EXPECT_EQ(rig.tracker.touch_stats().id_changes, 1u);
}

TEST(TouchGesture, VerticalDragScrolls)
{
    Rig rig;
    const Drag d = drag(rig, 900, 300, 920, 700, 20, 16000);
    EXPECT_GT(d.scroll, 0.2f);
    EXPECT_LT(d.scroll, 400.0f / kTouchHeight + 0.001f);
    EXPECT_EQ(d.last.touch_swipe, TouchSwipe::none);
    EXPECT_EQ(d.pressed & (action_bit(Action::page_prev) | action_bit(Action::page_next)), 0u);
    const InputFrame lift = rig.step(none());
    EXPECT_TRUE(lift.touch_ended);
    EXPECT_FALSE(lift.touch_tap);
    EXPECT_EQ(lift.touch_swipe, TouchSwipe::none);
    // Up is negative.
    Rig up;
    EXPECT_LT(drag(up, 900, 700, 900, 300, 20, 16000).scroll, -0.2f);
}

TEST(TouchGesture, SmallJitterDoesNotScroll)
{
    Rig rig;
    const Drag d = drag(rig, 900, 400, 910, 415, 5, 16000);
    EXPECT_EQ(d.scroll, 0.0f);
}

TEST(TouchGesture, HorizontalSwipeLeftIsPreviousTab)
{
    Rig rig;
    drag(rig, 1500, 450, 500, 470, 10, 30000);
    const InputFrame lift = rig.step(none());
    EXPECT_EQ(lift.touch_swipe, TouchSwipe::left);
    EXPECT_TRUE(lift.is_pressed(Action::page_prev));
    EXPECT_FALSE(lift.is_pressed(Action::page_next));
    EXPECT_FALSE(lift.is_held(Action::page_prev)); // never sticks
    EXPECT_EQ(rig.tracker.touch_stats().swipes_left, 1u);
    EXPECT_FALSE(rig.step(none()).is_pressed(Action::page_prev));
}

TEST(TouchGesture, HorizontalSwipeRightIsNextTab)
{
    Rig rig;
    drag(rig, 300, 450, 1400, 430, 10, 30000);
    const InputFrame lift = rig.step(none());
    EXPECT_EQ(lift.touch_swipe, TouchSwipe::right);
    EXPECT_TRUE(lift.is_pressed(Action::page_next)); // finger right = tab on the right = R1
}

TEST(TouchGesture, SwipeThresholds)
{
    { // too short (30% of the width)
        Rig rig;
        drag(rig, 600, 450, 1175, 450, 10, 30000);
        EXPECT_EQ(rig.step(none()).touch_swipe, TouchSwipe::none);
    }
    { // too slow
        Rig rig;
        drag(rig, 1700, 450, 300, 450, 10, 100000);
        EXPECT_EQ(rig.step(none()).touch_swipe, TouchSwipe::none);
    }
    { // too diagonal (dx/dy < 1.5)
        Rig rig;
        drag(rig, 1500, 100, 700, 700, 10, 30000);
        EXPECT_EQ(rig.step(none()).touch_swipe, TouchSwipe::none);
    }
    { // a long, quick, flat swipe passes
        Rig rig;
        drag(rig, 1700, 450, 900, 450, 8, 40000);
        EXPECT_EQ(rig.step(none()).touch_swipe, TouchSwipe::left);
    }
}

TEST(TouchGesture, ShortStillTouchIsATap)
{
    Rig rig;
    rig.step(one(800, 400));
    rig.step(one(803, 402));
    const InputFrame lift = rig.step(none());
    EXPECT_TRUE(lift.touch_tap);
    EXPECT_EQ(rig.tracker.touch_stats().taps, 1u);
}

TEST(TouchGesture, LongOrMovedTouchIsNotATap)
{
    {
        Rig rig;
        rig.step(one(800, 400));
        rig.idle(400000);
        EXPECT_FALSE(rig.step(none()).touch_tap);
    }
    {
        Rig rig;
        rig.step(one(800, 400));
        rig.step(one(880, 400));
        EXPECT_FALSE(rig.step(none()).touch_tap);
    }
}

TEST(TouchGesture, TwoFingersAreIgnored)
{
    Rig rig;
    float scroll = 0.0f;
    std::uint32_t pressed = 0;
    bool tap = false;
    for (int i = 0; i <= 10; ++i)
    {
        const InputFrame f = rig.step(two(1700 - i * 100, 300, 1750 - i * 100, 320), 30000);
        scroll += f.touch_scroll;
        pressed |= f.pressed;
        tap = tap || f.touch_tap;
        EXPECT_EQ(f.touch_count, 2);
    }
    const InputFrame lift = rig.step(none());
    EXPECT_EQ(scroll, 0.0f);
    EXPECT_EQ(pressed, 0u);
    EXPECT_FALSE(tap || lift.touch_tap);
    EXPECT_EQ(lift.touch_swipe, TouchSwipe::none);
}

TEST(TouchGesture, SecondFingerMidDragCancelsTheGesture)
{
    Rig rig;
    drag(rig, 1700, 450, 1000, 450, 5, 30000);
    rig.step(two(1000, 450, 300, 450), 30000);
    rig.step(one(300, 450, 1), 30000); // back to one finger, same contact
    const InputFrame lift = rig.step(none());
    EXPECT_EQ(lift.touch_swipe, TouchSwipe::none);
    EXPECT_FALSE(lift.touch_tap);
}

TEST(TouchGesture, ClickLatchSuppressesEverything)
{
    // A swipe that includes a physical click.
    Rig rig;
    rig.step(one(1700, 450));
    rig.step(one(1500, 450), 30000);
    const InputFrame click = rig.step(one(1400, 450, 1, pad_bits::kTouchpad), 30000);
    EXPECT_TRUE(click.is_pressed(Action::touch));
    EXPECT_TRUE(click.touch_click);
    EXPECT_TRUE(click.touch_active);
    for (int i = 0; i < 6; ++i)
    {
        const InputFrame f = rig.step(one(1200 - i * 150, 450, 1, pad_bits::kTouchpad), 30000);
        EXPECT_EQ(f.touch_scroll, 0.0f);
        EXPECT_TRUE(f.touch_click);
    }
    rig.step(one(300, 450), 30000); // click released, finger still down
    const InputFrame lift = rig.step(none());
    EXPECT_EQ(lift.touch_swipe, TouchSwipe::none);
    EXPECT_FALSE(lift.touch_tap);
    EXPECT_FALSE(lift.is_pressed(Action::page_next));
}

TEST(TouchGesture, ClickWithAStillFingerIsNotATap)
{
    Rig rig;
    rig.step(one(800, 400));
    rig.step(one(800, 400, 1, pad_bits::kTouchpad), 40000);
    rig.step(one(800, 400), 40000);
    const InputFrame lift = rig.step(none());
    EXPECT_FALSE(lift.touch_tap);
}

TEST(TouchGesture, FingerLandingJustAfterAClickReleaseIsGuarded)
{
    Rig rig;
    rig.step(none(pad_bits::kTouchpad));
    rig.step(none());
    // Lands 16 ms after the release, inside the 100 ms guard.
    const Drag guarded = drag(rig, 900, 200, 900, 700, 10, 16000);
    EXPECT_EQ(guarded.scroll, 0.0f);
    rig.step(none());
    // Later contacts are fine again.
    rig.idle(300000);
    EXPECT_GT(drag(rig, 900, 200, 900, 700, 10, 16000).scroll, 0.2f);
}

TEST(TouchGesture, AbsoluteModeHasNoScrollOrSwipe)
{
    Rig rig;
    rig.tracker.set_touch_mode(TouchMode::absolute);
    const Drag d = drag(rig, 1700, 200, 1600, 800, 10, 30000);
    EXPECT_EQ(d.scroll, 0.0f);
    EXPECT_TRUE(d.last.touch_active);
    EXPECT_GT(d.last.touch_y, 0.8f);
    Rig sw;
    sw.tracker.set_touch_mode(TouchMode::absolute);
    drag(sw, 1800, 450, 300, 450, 10, 30000);
    EXPECT_EQ(sw.step(none()).touch_swipe, TouchSwipe::none);
}

TEST(TouchGesture, LosingTheControllerDropsTheContact)
{
    Rig rig;
    rig.step(one(500, 500));
    PadSample gone;
    gone.connected = false;
    const InputFrame f = rig.step(gone);
    EXPECT_FALSE(f.touch_active);
    EXPECT_FALSE(f.touch_tap);
    EXPECT_EQ(f.touch_swipe, TouchSwipe::none);
}

TEST(TouchGesture, ActiveFlagPersistsOverFramesWithoutSamples)
{
    Rig rig;
    rig.step(one(500, 600));
    const InputFrame f = rig.idle(16000);
    EXPECT_TRUE(f.touch_active);
    EXPECT_NEAR(f.touch_y, 600.0f / 941.0f, 0.001f);
    EXPECT_FALSE(f.touch_began);
    EXPECT_GE(f.touch_down_ms, 16u);
}

TEST(TouchGesture, ShortTapBetweenFramesStillRegisters)
{
    // Down, up in one batch of samples.
    Rig rig;
    const PadSample batch[3] = {one(800, 400), one(801, 400), none()};
    rig.now += 16000;
    const InputFrame f = rig.tracker.update(batch, rig.now);
    EXPECT_TRUE(f.touch_began);
    EXPECT_TRUE(f.touch_ended);
    EXPECT_TRUE(f.touch_tap);
    EXPECT_FALSE(f.touch_active);
}

TEST(TouchGesture, PhysicalClickStillGivesActionTouch)
{
    Rig rig;
    const InputFrame f = rig.step(none(pad_bits::kTouchpad));
    EXPECT_TRUE(f.is_pressed(Action::touch));
    EXPECT_TRUE(f.touch_click);
    EXPECT_FALSE(f.touch_active);
}

TEST(TouchStatsTest, TrackRangeAndIds)
{
    Rig rig;
    rig.step(one(100, 50, 3));
    rig.step(one(1800, 900, 3));
    rig.step(none());
    rig.step(one(960, 470, 4));
    const TouchStats &st = rig.tracker.touch_stats();
    EXPECT_EQ(st.min_x, 100);
    EXPECT_EQ(st.max_x, 1800);
    EXPECT_EQ(st.min_y, 50);
    EXPECT_EQ(st.max_y, 900);
    EXPECT_EQ(st.distinct_ids(), 2);
    EXPECT_EQ(st.contacts, 2u);
    EXPECT_EQ(st.last_id, 4);
    EXPECT_EQ(st.samples, 3u);
}

TEST(TouchCell, CornersAndCentre)
{
    EXPECT_EQ(touch_to_cell(0.0f, 0.0f, 4, 10, 0.0f), (TouchCell{0, 0}));
    EXPECT_EQ(touch_to_cell(1.0f, 1.0f, 4, 10, 0.0f), (TouchCell{3, 9}));
    EXPECT_EQ(touch_to_cell(0.5f, 0.5f, 4, 10, 0.0f), (TouchCell{2, 5}));
    EXPECT_EQ(touch_to_cell(0.5f, 0.1f, 4, 10, 0.0f).row, 0);
}

TEST(TouchCell, MarginsClampToTheOuterKeys)
{
    EXPECT_EQ(touch_to_cell(0.03f, 0.03f, 3, 8, 0.0f), (TouchCell{0, 0}));
    EXPECT_EQ(touch_to_cell(0.97f, 0.97f, 3, 8, 0.0f), (TouchCell{2, 7}));
    EXPECT_EQ(touch_to_cell(kTouchMargin + 0.001f, 0.5f, 1, 10, 0.0f).col, 0);
}

TEST(TouchCell, HysteresisKeepsTheKeyNearABoundary)
{
    // 10 columns over the inner 92%: the boundary between col 4 and 5 is x = 0.5.
    const TouchCell at4 = touch_to_cell(0.49f, 0.5f, 1, 10, 0.2f);
    ASSERT_EQ(at4.col, 4);
    // Slightly past the boundary: stays.
    EXPECT_EQ(touch_to_cell(0.51f, 0.5f, 1, 10, 0.2f, at4).col, 4);
    // Well past it (more than 20% of a cell): moves.
    EXPECT_EQ(touch_to_cell(0.54f, 0.5f, 1, 10, 0.2f, at4).col, 5);
    // Back across: needs the same overshoot the other way.
    const TouchCell at5 = {0, 5};
    EXPECT_EQ(touch_to_cell(0.49f, 0.5f, 1, 10, 0.2f, at5).col, 5);
    EXPECT_EQ(touch_to_cell(0.46f, 0.5f, 1, 10, 0.2f, at5).col, 4);
    // Without hysteresis it flips at once.
    EXPECT_EQ(touch_to_cell(0.51f, 0.5f, 1, 10, 0.0f, at4).col, 5);
}

TEST(TouchCell, DegenerateInputIsSafe)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(touch_to_cell(0.5f, 0.5f, 0, 5, 0.2f), (TouchCell{0, 0}));
    EXPECT_EQ(touch_to_cell(0.5f, 0.5f, 5, 0, 0.2f), (TouchCell{0, 0}));
    EXPECT_EQ(touch_to_cell(0.5f, 0.5f, -3, -3, 0.2f), (TouchCell{0, 0}));
    EXPECT_TRUE(touch_to_cell(nan, nan, 4, 10, 0.2f).valid());
    EXPECT_EQ(touch_to_cell(inf, -inf, 4, 10, 0.2f), (TouchCell{0, 9}));
    EXPECT_EQ(touch_to_cell(9.0f, -9.0f, 4, 10, 0.2f), (TouchCell{0, 9}));
    // A stale previous cell outside the grid is ignored.
    EXPECT_EQ(touch_to_cell(0.5f, 0.5f, 4, 10, 0.2f, TouchCell{40, 40}), (TouchCell{2, 5}));
    EXPECT_FALSE(TouchCell{}.valid());
}

TEST(TouchLog, LineHasCoordinatesAndRawHex)
{
    RawBuffer raw;
    raw.touches(1).finger(0, 960, 470, 3);
    ps5::RawPadSample r;
    std::memcpy(&r, raw.bytes, sizeof(r));
    char line[256];
    const int n = ps5::format_touch_line(r, line, sizeof(line));
    ASSERT_GT(n, 0);
    const std::string text = line;
    EXPECT_NE(text.find("n=1 p0=(960,470,id3)"), std::string::npos);
    EXPECT_NE(text.find("raw34..4b=01 00 00 00"), std::string::npos);
    EXPECT_NE(text.find("c0 03 d6 01 03"), std::string::npos); // 960 = 0x03c0, 470 = 0x01d6
    // A tiny buffer truncates safely.
    char small[16];
    ps5::format_touch_line(r, small, sizeof(small));
    EXPECT_LT(std::strlen(small), sizeof(small));
}

TEST(TouchLog, LimiterRateLimitsAndCaps)
{
    ps5::TouchLogLimiter limiter(200000, 3);
    EXPECT_TRUE(limiter.allow(1000000));
    EXPECT_FALSE(limiter.allow(1100000));
    EXPECT_TRUE(limiter.allow(1250000));
    EXPECT_TRUE(limiter.allow(1500000));
    EXPECT_FALSE(limiter.allow(9000000)); // cap reached
    EXPECT_EQ(limiter.lines(), 3);
    ps5::TouchLogLimiter early(200000, 100);
    EXPECT_TRUE(early.allow(0)); // the first line is never held back
}

TEST(TouchDiagnostics, BlockShowsRangeAndIds)
{
    orbit::DiagnosticsInput in;
    in.touch_known = true;
    std::string text = orbit::format_diagnostics(in);
    EXPECT_NE(text.find("Touchpad"), std::string::npos);
    EXPECT_NE(text.find("no touch seen yet"), std::string::npos);

    Rig rig;
    rig.step(one(100, 50, 3));
    rig.step(one(1800, 900, 3));
    in.touch = rig.tracker.touch_stats();
    text = orbit::format_diagnostics(in);
    EXPECT_NE(text.find("x range seen:    100 .. 1800"), std::string::npos);
    EXPECT_NE(text.find("y range seen:    50 .. 900"), std::string::npos);
    EXPECT_NE(text.find("ids seen:        1 (3)"), std::string::npos);
    EXPECT_NE(text.find("layout looks right: yes"), std::string::npos);

    in.touch_known = false;
    EXPECT_NE(orbit::format_diagnostics(in).find("not reported"), std::string::npos);
}
