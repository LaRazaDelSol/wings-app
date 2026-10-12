// ps5-homebrew-ui - Touchpad model: touch points, gestures and key mapping.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/touch.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace hui
{

namespace
{

float clamp01(float v)
{
    if (!(v > 0.0f)) // also catches NaN
        return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

int axis_cell(float n, int count, float hysteresis, int previous)
{
    const float u = clamp01((clamp01(n) - kTouchMargin) / (1.0f - 2.0f * kTouchMargin));
    const float pos = u * static_cast<float>(count);
    const float h = hysteresis > 0.0f ? hysteresis : 0.0f;
    if (previous >= 0 && previous < count)
    {
        const float lo = static_cast<float>(previous) - h;
        const float hi = static_cast<float>(previous) + 1.0f + h;
        if (pos >= lo && pos < hi)
            return previous;
    }
    const int index = static_cast<int>(std::floor(pos));
    return std::clamp(index, 0, count - 1);
}

} // namespace

int TouchStats::distinct_ids() const
{
    return std::popcount(id_mask);
}

TouchCell touch_to_cell(float x, float y, int rows, int cols, float hysteresis, TouchCell previous)
{
    if (rows <= 0 || cols <= 0)
        return TouchCell{0, 0};
    TouchCell cell;
    cell.row = axis_cell(y, rows, hysteresis, previous.row);
    cell.col = axis_cell(x, cols, hysteresis, previous.col);
    return cell;
}

void TouchGestures::reset()
{
    active_ = false;
    multi_ = false;
    tainted_ = false;
    lock_ = Lock::undecided;
    click_ = false;
    have_release_ = false;
}

void TouchGestures::observe(const TouchSample &sample)
{
    const int n = std::min<int>(sample.count, 2);
    int down = 0;
    bool first = true;
    for (int i = 0; i < n; ++i)
    {
        const TouchPoint &p = sample.point[i];
        if (!p.down)
            continue;
        ++down;
        stats_.min_x = std::min(stats_.min_x, p.x);
        stats_.max_x = std::max(stats_.max_x, p.x);
        stats_.min_y = std::min(stats_.min_y, p.y);
        stats_.max_y = std::max(stats_.max_y, p.y);
        stats_.id_mask |= std::uint64_t{1} << (p.id & 63);
        if (first)
        {
            if (seen_any_ && p.id != stats_last_id_)
                ++stats_.id_changes;
            stats_last_id_ = p.id;
            seen_any_ = true;
            first = false;
        }
    }
    if (down > 0)
    {
        ++stats_.samples;
        stats_.max_fingers = std::max<std::uint8_t>(stats_.max_fingers, static_cast<std::uint8_t>(down));
    }
    if (sample.bad)
        ++stats_.bad;
    stats_.last_id = stats_last_id_;
}

void TouchGestures::begin(const TouchPoint &p, bool multi, bool tainted, std::uint64_t now_us,
                          TouchOutput &out)
{
    active_ = true;
    multi_ = multi;
    tainted_ = tainted;
    lock_ = Lock::undecided;
    id_ = p.id;
    start_x_ = last_x_ = p.x;
    start_y_ = last_y_ = p.y;
    start_us_ = now_us;
    travel_ = 0.0f;
    out.began = true;
    ++stats_.contacts;
}

void TouchGestures::finish(std::uint64_t now_us, TouchOutput &out)
{
    out.ended = true;
    active_ = false;
    if (multi_ || tainted_)
        return;
    const std::uint64_t duration = now_us > start_us_ ? now_us - start_us_ : 0;
    const float tx = last_x_ - start_x_;
    const float ty = last_y_ - start_y_;
    if (mode_ == TouchMode::browse && lock_ != Lock::vertical && duration <= kSwipeMaxUs &&
        std::fabs(tx) >= kSwipeFraction * static_cast<float>(kTouchWidth - 1) &&
        std::fabs(tx) >= kSwipeAxisRatio * std::fabs(ty))
    {
        out.swipe = tx < 0.0f ? TouchSwipe::left : TouchSwipe::right;
        if (tx < 0.0f)
            ++stats_.swipes_left;
        else
            ++stats_.swipes_right;
    }
    else if (travel_ < kTapTravelPx && duration <= kTapMaxUs)
    {
        out.tap = true;
        ++stats_.taps;
    }
}

void TouchGestures::feed(const TouchSample &sample, std::uint64_t now_us, TouchOutput &out)
{
    if (!sample.valid)
    {
        reset();
        fingers_ = 0;
        return;
    }
    observe(sample);

    int fingers = 0;
    const TouchPoint *first = nullptr;
    for (int i = 0; i < std::min<int>(sample.count, 2); ++i)
    {
        if (!sample.point[i].down)
            continue;
        ++fingers;
        if (first == nullptr)
            first = &sample.point[i];
    }
    fingers_ = static_cast<std::uint8_t>(fingers);

    // The click latch: a finger that is part of a click, or that lands just
    // after one, never makes a gesture.
    if (sample.click && !click_)
    {
        click_ = true;
        if (fingers > 0)
            ++stats_.clicks_on_touch;
    }
    else if (!sample.click && click_)
    {
        click_ = false;
        have_release_ = true;
        release_us_ = now_us;
    }
    const bool guard = click_ || (have_release_ && now_us - release_us_ < kClickGuardUs);

    if (fingers == 0)
    {
        if (active_)
            finish(now_us, out);
        multi_ = false;
        return;
    }

    if (!active_)
    {
        begin(*first, fingers >= 2, guard, now_us, out);
    }
    else if (!multi_ && fingers == 1 && first->id != id_)
    {
        // The pad swapped fingers between two samples: a new contact.
        finish(now_us, out);
        begin(*first, false, guard, now_us, out);
    }
    else if (fingers >= 2)
    {
        multi_ = true;
    }
    if (guard)
        tainted_ = true;

    const float px = first->x;
    const float py = first->y;
    const float ddx = px - last_x_;
    const float ddy = py - last_y_;
    last_x_ = px;
    last_y_ = py;
    if (multi_ || tainted_)
        return;
    travel_ += std::sqrt(ddx * ddx + ddy * ddy);
    out.dx += ddx / static_cast<float>(kTouchHeight);
    out.dy += ddy / static_cast<float>(kTouchHeight);
    if (lock_ == Lock::undecided)
    {
        const float tx = px - start_x_;
        const float ty = py - start_y_;
        if (std::sqrt(tx * tx + ty * ty) >= kLockPx)
            lock_ = std::fabs(ty) >= std::fabs(tx) ? Lock::vertical : Lock::horizontal;
    }
    if (lock_ == Lock::vertical && mode_ == TouchMode::browse)
        out.scroll += ddy / static_cast<float>(kTouchHeight);
}

TouchOutput TouchGestures::update(std::span<const TouchSample> samples, std::uint64_t now_us)
{
    TouchOutput out;
    for (const TouchSample &sample : samples)
        feed(sample, now_us, out);
    out.active = active_;
    out.count = active_ ? fingers_ : 0;
    out.x = clamp01(last_x_ / static_cast<float>(kTouchWidth - 1));
    out.y = clamp01(last_y_ / static_cast<float>(kTouchHeight - 1));
    out.down_ms = active_ && now_us > start_us_
                      ? static_cast<std::uint32_t>((now_us - start_us_) / 1000)
                      : 0;
    if (out.scroll != 0.0f)
        ++stats_.scroll_frames;
    return out;
}

} // namespace hui
