// ps5-homebrew-ui - The 120-byte scePad sample layout and its parser.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header-only and free of scePad externs so host tests can feed synthetic
// buffers. Touch offsets follow the OpenOrbis ScePadData layout (see
// docs/research/search-touchpad.md); the console run confirms them through the
// Touchpad block of diagnostics.txt.

#pragma once

#include "core/input.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hui::ps5
{

struct RawTouch
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
    std::uint8_t reserved[3];
};
static_assert(sizeof(RawTouch) == 8);

// scePadRead sample layout (120 bytes), as typed in ProsperoLight.
struct RawPadSample
{
    std::uint32_t buttons;
    std::uint8_t left_x;
    std::uint8_t left_y;
    std::uint8_t right_x;
    std::uint8_t right_y;
    std::uint8_t l2;
    std::uint8_t r2;
    std::uint8_t reserved0[42]; // 0x0a..0x33: pads, orientation, acceleration, gyro
    std::uint8_t touch_num;     // 0x34
    std::uint8_t reserved_t[3];
    std::uint32_t reserved_t2; // 0x38
    RawTouch touch[2];         // 0x3c, 0x44
    std::int32_t connected;    // 0x4c
    std::uint64_t timestamp_us;
    std::uint8_t extension[16];
    std::uint8_t connected_count;
    std::uint8_t reserved1[15];
};
static_assert(sizeof(RawPadSample) == 120);
static_assert(offsetof(RawPadSample, touch_num) == 0x34);
static_assert(offsetof(RawPadSample, touch) == 0x3c);
static_assert(offsetof(RawPadSample, touch) + sizeof(RawTouch) == 0x44);
static_assert(offsetof(RawPadSample, connected) == 0x4c);
static_assert(offsetof(RawPadSample, timestamp_us) == 0x50);
static_assert(offsetof(RawPadSample, connected_count) == 0x68);

constexpr std::size_t kRawPadSampleSize = sizeof(RawPadSample);
// Bytes 0x34..0x4b: the whole touch block, logged as hex.
constexpr std::size_t kTouchBlockOffset = 0x34;
constexpr std::size_t kTouchBlockSize = 0x4c - 0x34;

// Fills the touch fields of s from raw. A touch count above 2 is garbage
// (wrong layout or a corrupt read): no touches, touch_bad set.
inline void parse_touch(const RawPadSample &raw, PadSample &s)
{
    s.touch_count = 0;
    s.touch_bad = false;
    s.touch[0] = TouchPoint{};
    s.touch[1] = TouchPoint{};
    if (raw.touch_num > 2)
    {
        s.touch_bad = true;
        return;
    }
    s.touch_count = raw.touch_num;
    for (int i = 0; i < raw.touch_num; ++i)
    {
        s.touch[i].down = true;
        s.touch[i].x = raw.touch[i].x;
        s.touch[i].y = raw.touch[i].y;
        s.touch[i].id = raw.touch[i].id;
    }
}

// The whole sample (buttons, sticks, triggers, connection, touch).
inline void parse_sample(const RawPadSample &r, PadSample &s)
{
    s.buttons = r.buttons;
    s.left_x = r.left_x;
    s.left_y = r.left_y;
    s.right_x = r.right_x;
    s.right_y = r.right_y;
    s.l2 = r.l2;
    s.r2 = r.r2;
    s.connected = r.connected != 0;
    s.timestamp_us = r.timestamp_us;
    parse_touch(r, s);
}

// Parses a raw byte buffer; false (s untouched) when it is shorter than a sample.
inline bool parse_buffer(const void *data, std::size_t size, PadSample &s)
{
    if (data == nullptr || size < kRawPadSampleSize)
        return false;
    RawPadSample raw;
    std::memcpy(&raw, data, sizeof(raw));
    parse_sample(raw, s);
    return true;
}

// "n=1 p0=(960,470,id3) p1=(0,0,id0) raw34..4b=01 00 ..." for the title log.
inline int format_touch_line(const RawPadSample &raw, char *out, std::size_t size)
{
    int n = std::snprintf(out, size, "touch n=%u p0=(%u,%u,id%u) p1=(%u,%u,id%u) raw34..4b=",
                          static_cast<unsigned>(raw.touch_num), static_cast<unsigned>(raw.touch[0].x),
                          static_cast<unsigned>(raw.touch[0].y), static_cast<unsigned>(raw.touch[0].id),
                          static_cast<unsigned>(raw.touch[1].x), static_cast<unsigned>(raw.touch[1].y),
                          static_cast<unsigned>(raw.touch[1].id));
    if (n < 0)
        return n;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&raw) + kTouchBlockOffset;
    for (std::size_t i = 0; i < kTouchBlockSize && static_cast<std::size_t>(n) + 4 < size; ++i)
        n += std::snprintf(out + n, size - static_cast<std::size_t>(n), "%s%02x", i == 0 ? "" : " ",
                           static_cast<unsigned>(bytes[i]));
    return n;
}

// One log line per interval, at most max_lines in all.
class TouchLogLimiter
{
  public:
    explicit TouchLogLimiter(std::uint64_t interval_us = 200000, int max_lines = 100)
        : interval_us_(interval_us), max_lines_(max_lines)
    {
    }
    bool allow(std::uint64_t now_us)
    {
        if (lines_ >= max_lines_)
            return false;
        if (lines_ > 0 && now_us >= last_us_ && now_us - last_us_ < interval_us_)
            return false;
        last_us_ = now_us;
        ++lines_;
        return true;
    }
    int lines() const
    {
        return lines_;
    }

  private:
    std::uint64_t interval_us_;
    int max_lines_;
    int lines_ = 0;
    std::uint64_t last_us_ = 0;
};

} // namespace hui::ps5
