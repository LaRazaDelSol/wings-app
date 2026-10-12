// ps5-homebrew-ui - Touchpad model: touch points, gestures and key mapping.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure logic, no platform calls: the pad layer fills TouchSample from the raw
// scePad buffer, TouchGestures turns the stream into scroll / swipe / tap, and
// touch_to_cell maps a finger position onto a key grid.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace hui
{

// The DualSense touchpad as the pad reports it (DS4 header values, unverified
// on the console: the diagnostics file shows the range actually seen).
constexpr int kTouchWidth = 1920;
constexpr int kTouchHeight = 942;

struct TouchPoint
{
    bool down = false;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t id = 0; // a new contact gets a new id
};

// One pad sample's touch state plus the physical click.
struct TouchSample
{
    bool valid = true; // false: the pad is gone or the system owns input
    bool click = false;
    bool bad = false; // the raw touch count was out of range
    std::uint8_t count = 0;
    TouchPoint point[2];
};

enum class TouchSwipe : std::uint8_t
{
    none,
    left,  // finger moved left  (next tab)
    right, // finger moved right (previous tab)
};

// browse: vertical drag scrolls, horizontal swipe switches tabs.
// absolute: the pad is a pointer (on-screen keyboard); no scroll or swipe.
enum class TouchMode : std::uint8_t
{
    browse,
    absolute,
};

// What the gestures produced during one frame.
struct TouchOutput
{
    bool active = false; // a finger is down at the end of the frame
    float x = 0.0f;      // 0..1 across the pad (last known position)
    float y = 0.0f;      // 0..1 down the pad
    std::uint8_t count = 0;
    bool began = false; // a contact started this frame
    bool ended = false; // a contact ended this frame
    bool tap = false;   // a short, still, single-finger, unclicked touch ended
    TouchSwipe swipe = TouchSwipe::none;
    // Movement this frame in pad heights (dx/942, dy/942), finger-down positive.
    float dx = 0.0f;
    float dy = 0.0f;
    // dy while a vertical drag is locked in (zero otherwise).
    float scroll = 0.0f;
    std::uint32_t down_ms = 0; // how long the current contact has lasted
};

// Everything the first console run needs to verify the layout.
struct TouchStats
{
    std::uint32_t samples = 0;  // samples with a finger down
    std::uint32_t contacts = 0; // contacts begun
    std::uint32_t bad = 0;      // samples with an impossible touch count
    std::uint32_t taps = 0;
    std::uint32_t swipes_left = 0;
    std::uint32_t swipes_right = 0;
    std::uint32_t scroll_frames = 0;
    std::uint32_t clicks_on_touch = 0; // click edges while a finger was down
    std::uint8_t max_fingers = 0;
    std::uint16_t min_x = 0xffff, max_x = 0;
    std::uint16_t min_y = 0xffff, max_y = 0;
    std::uint32_t id_changes = 0;
    std::uint8_t last_id = 0;
    std::uint64_t id_mask = 0; // bit (id & 63) set for every id seen

    int distinct_ids() const;
    bool seen() const
    {
        return samples != 0;
    }
};

class TouchGestures
{
  public:
    // Feeds every sample read this frame in order and returns the frame's output.
    TouchOutput update(std::span<const TouchSample> samples, std::uint64_t now_us);
    // Drops the current contact without any gesture (pad lost).
    void reset();

    void set_mode(TouchMode mode)
    {
        mode_ = mode;
    }
    TouchMode mode() const
    {
        return mode_;
    }
    const TouchStats &stats() const
    {
        return stats_;
    }

    // Tuning, in pad pixels (1920x942) and microseconds.
    static constexpr float kLockPx = 40.0f;            // movement before a drag direction is chosen
    static constexpr float kTapTravelPx = 24.0f;       // a tap may not move more than this
    static constexpr std::uint64_t kTapMaxUs = 250000; // ... nor last longer
    static constexpr float kSwipeFraction = 0.35f;     // of the pad width
    static constexpr std::uint64_t kSwipeMaxUs = 600000;
    static constexpr float kSwipeAxisRatio = 1.5f;
    static constexpr std::uint64_t kClickGuardUs = 100000; // after a click ends

  private:
    enum class Lock : std::uint8_t
    {
        undecided,
        vertical,
        horizontal,
    };

    void feed(const TouchSample &sample, std::uint64_t now_us, TouchOutput &out);
    void begin(const TouchPoint &p, bool multi, bool tainted, std::uint64_t now_us,
               TouchOutput &out);
    void finish(std::uint64_t now_us, TouchOutput &out);
    void observe(const TouchSample &sample);

    TouchMode mode_ = TouchMode::browse;
    TouchStats stats_;

    bool active_ = false;
    std::uint8_t fingers_ = 0;
    bool multi_ = false;   // two fingers were down: no gesture for this contact
    bool tainted_ = false; // the click was involved: no gesture for this contact
    Lock lock_ = Lock::undecided;
    std::uint8_t id_ = 0;
    float start_x_ = 0.0f, start_y_ = 0.0f;
    float last_x_ = 0.0f, last_y_ = 0.0f; // pad pixels
    std::uint64_t start_us_ = 0;
    float travel_ = 0.0f;

    bool click_ = false;
    bool have_release_ = false;
    std::uint64_t release_us_ = 0;
    bool seen_any_ = false;
    std::uint8_t stats_last_id_ = 0;
};

// A cell in a rows x cols key grid.
struct TouchCell
{
    int row = -1;
    int col = -1;
    bool valid() const
    {
        return row >= 0 && col >= 0;
    }
    bool operator==(const TouchCell &o) const
    {
        return row == o.row && col == o.col;
    }
};

// Edge dead zone of the absolute mapping: the outer 4% of the pad clamp to the
// outermost keys, the rest is stretched over the whole grid.
constexpr float kTouchMargin = 0.04f;

// Maps a normalised pad position (0..1) onto a rows x cols grid. With a valid
// previous cell the result only changes once the finger is more than
// hysteresis (a fraction of a cell, 0.2 is a good value) past the boundary.
// Non-finite or out-of-range input is clamped; an empty grid gives {0, 0}.
TouchCell touch_to_cell(float x, float y, int rows, int cols, float hysteresis,
                        TouchCell previous = {});

} // namespace hui
