// Wings TV app - Artwork decoding: WebP, PNG and JPEG to sized RGBA.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Catalogue covers and backgrounds are WebP; their fallbacks are PNG or JPEG.
// The app's heap is fixed (src/runtime/app_heap.c), so images are decoded
// straight to the size they are drawn at: libwebp scales while decoding, and
// PNG/JPEG are reduced with an area filter right after. Nothing here touches
// OpenGL; textures are created on the frame thread from the result.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::image
{

struct Pixels
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, rows top to bottom

    bool empty() const
    {
        return width <= 0 || height <= 0;
    }
};

enum class Format : std::uint8_t
{
    unknown,
    webp,
    png,
    jpeg,
};

Format sniff(std::string_view bytes);

// Decodes and fits the image inside max_width x max_height, keeping its
// aspect ratio and never enlarging it. Returns false with a reason on error.
bool decode(std::string_view bytes, int max_width, int max_height, Pixels *out,
            std::string *error = nullptr);

// Area-averaging reduction (or nearest enlargement) to exactly width x height.
Pixels resize(const Pixels &in, int width, int height);

// A small, heavily blurred, slightly darkened copy: the colour wash behind a
// game whose only artwork is a square cover (the web app's ambient layout).
Pixels ambient(const Pixels &in, int size = 48);

} // namespace orbit::image
