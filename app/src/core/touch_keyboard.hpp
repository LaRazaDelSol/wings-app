// Wings TV app - Touchpad typing: which touch mode the pad needs this frame.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// An open Keyboard calls note_keyboard_open() from handle(). The frame loop
// applies the result before reading the pad: absolute while a keyboard was
// handled last frame, browse otherwise (so closing the prompt restores it).

#pragma once

#include "core/input.hpp"

namespace hui
{

inline bool g_keyboard_open_seen = false;

inline void note_keyboard_open()
{
    g_keyboard_open_seen = true;
}

inline void apply_keyboard_touch_mode(InputTracker &tracker)
{
    tracker.set_touch_mode(g_keyboard_open_seen ? TouchMode::absolute : TouchMode::browse);
    g_keyboard_open_seen = false;
}

} // namespace hui
