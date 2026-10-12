// Wings TV app - Touchpad typing on the on-screen keyboard (synthetic frames).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/input.hpp"
#include "core/touch.hpp"
#include "core/touch_keyboard.hpp"
#include "ui/components/keyboard.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace hui;

namespace
{

// The pad position (0..1) at the centre of cell `index` of `count`.
float centre(int index, int count)
{
    const float u = (static_cast<float>(index) + 0.5f) / static_cast<float>(count);
    return kTouchMargin + u * (1.0f - 2.0f * kTouchMargin);
}

InputFrame finger(float x, float y, std::uint8_t count = 1)
{
    InputFrame f;
    f.touch_active = true;
    f.touch_count = count;
    f.touch_x = x;
    f.touch_y = y;
    return f;
}

InputFrame click(float x, float y, std::uint8_t count = 1)
{
    InputFrame f = finger(x, y, count);
    f.pressed |= action_bit(Action::touch);
    f.held |= action_bit(Action::touch);
    f.touch_click = true;
    return f;
}

struct Rig
{
    ui::Keyboard keys;
    ui::Feedback feedback;
    std::string text;
    int rows() const
    {
        return static_cast<int>(keys.layouts()[static_cast<std::size_t>(keys.layout())].rows.size());
    }
    int cols() const
    {
        return keys.layouts()[static_cast<std::size_t>(keys.layout())].columns;
    }
    void send(const InputFrame &f)
    {
        keys.handle(f, feedback);
        text += keys.typed();
    }
};

} // namespace

TEST(TouchKeyboard, FingerFocusesEveryRowAndMovesAcrossColumns)
{
    Rig rig;
    for (int r = 0; r < rig.rows(); ++r)
    {
        rig.send(finger(0.0f, centre(r, rig.rows())));
        EXPECT_EQ(rig.keys.focus_row(), r);
        EXPECT_EQ(rig.keys.focus_key(), 0) << "left edge, row " << r;
        int last = 0;
        for (int c = 0; c < rig.cols(); ++c)
        {
            rig.send(finger(centre(c, rig.cols()), centre(r, rig.rows())));
            EXPECT_EQ(rig.keys.focus_row(), r);
            EXPECT_GE(rig.keys.focus_key(), last);
            last = rig.keys.focus_key();
        }
        rig.send(finger(1.0f, centre(r, rig.rows())));
        const int count =
            static_cast<int>(rig.keys.layouts()[0].rows[static_cast<std::size_t>(r)].size());
        EXPECT_EQ(rig.keys.focus_key(), count - 1) << "right edge, row " << r;
    }
}

TEST(TouchKeyboard, HysteresisHoldsKeyNearBorder)
{
    Rig rig;
    const int cols = rig.cols();
    const float y = centre(1, rig.rows());
    rig.send(finger(centre(3, cols), y));
    const int key = rig.keys.focus_key();
    // 10% of a key past the border: still the same key; 60% past: moved on.
    const float border = centre(3, cols) + 0.5f * (1.0f - 2.0f * kTouchMargin) / cols;
    const float step = (1.0f - 2.0f * kTouchMargin) / cols;
    rig.send(finger(border + 0.1f * step, y));
    EXPECT_EQ(rig.keys.focus_key(), key);
    rig.send(finger(border + 0.6f * step, y));
    EXPECT_NE(rig.keys.focus_key(), key);
}

TEST(TouchKeyboard, ClickTypesFocusedKeyExactlyOnce)
{
    Rig rig;
    const float y = centre(1, rig.rows());
    rig.send(finger(centre(2, rig.cols()), y));
    const int key = rig.keys.focus_key();
    // The finger slides while the pad goes down: the click uses the latched key.
    rig.send(click(centre(5, rig.cols()), y));
    EXPECT_EQ(rig.text.size(), 1u);
    EXPECT_EQ(rig.keys.focus_key(), key);
    InputFrame held = finger(centre(5, rig.cols()), y);
    held.held |= action_bit(Action::touch);
    rig.send(held);
    rig.send(held);
    EXPECT_EQ(rig.text.size(), 1u);
    EXPECT_EQ(rig.keys.focus_key(), key);
}

TEST(TouchKeyboard, DpadOverridesUntilFingerLifts)
{
    Rig rig;
    const float y = centre(1, rig.rows());
    rig.send(finger(centre(2, rig.cols()), y));
    InputFrame pad = finger(centre(2, rig.cols()), y);
    pad.nav = Direction::right;
    rig.send(pad);
    const int key = rig.keys.focus_key();
    rig.send(finger(centre(7, rig.cols()), centre(2, rig.rows())));
    EXPECT_EQ(rig.keys.focus_key(), key); // touch is cancelled
    rig.send(click(0.5f, 0.5f));
    EXPECT_TRUE(rig.text.empty());
    rig.send(InputFrame{}); // finger lifts
    rig.send(finger(centre(7, rig.cols()), centre(2, rig.rows())));
    EXPECT_EQ(rig.keys.focus_row(), 2);
}

TEST(TouchKeyboard, TwoFingersIgnored)
{
    Rig rig;
    const int row = rig.keys.focus_row(), key = rig.keys.focus_key();
    rig.send(finger(0.9f, 0.9f, 2));
    EXPECT_EQ(rig.keys.focus_row(), row);
    EXPECT_EQ(rig.keys.focus_key(), key);
    rig.send(click(0.9f, 0.9f, 2));
    EXPECT_TRUE(rig.text.empty());
}

TEST(TouchKeyboard, NoFingerNoTouchAction)
{
    Rig rig;
    InputFrame f;
    f.pressed |= action_bit(Action::touch);
    rig.send(f);
    EXPECT_TRUE(rig.text.empty());
}

TEST(TouchKeyboard, ModeIsAbsoluteWhileOpenAndBrowseAfter)
{
    InputTracker tracker;
    Rig rig;
    g_keyboard_open_seen = false;
    apply_keyboard_touch_mode(tracker);
    EXPECT_EQ(tracker.touch_mode(), TouchMode::browse);
    rig.send(InputFrame{});
    apply_keyboard_touch_mode(tracker);
    EXPECT_EQ(tracker.touch_mode(), TouchMode::absolute);
    apply_keyboard_touch_mode(tracker); // no keyboard handled last frame
    EXPECT_EQ(tracker.touch_mode(), TouchMode::browse);
}
