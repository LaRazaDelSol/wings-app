// Wings cover-art module: image format sniffing and the pre-decode size guard.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads only the first bytes of a file (no decoder, no allocation) to learn the
// format and the pixel size, so a hostile or broken image is refused BEFORE
// libwebp / stb_image allocate for it. The same check is described as a patch
// for orbit::image::decode in docs/IMAGE_GUARD.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wings::art
{

enum class ImageFormat : std::uint8_t
{
    unknown,
    webp,
    png,
    jpeg,
    avif, // recognised so it can be refused with a clear reason (no decoder)
};

const char *name(ImageFormat f);

struct ImageInfo
{
    ImageFormat format = ImageFormat::unknown;
    int width = 0;
    int height = 0;
    bool animated = false; // animated WebP (not supported by the app's decoder)
};

// Magic bytes only. Never trust the extension or the Content-Type: the PS CDN
// answers *.png URLs with JPEG, other CDNs send no Content-Type at all.
ImageFormat sniff_image(const std::uint8_t *data, std::size_t size);

// Format plus dimensions from the header. False when the header is truncated
// or inconsistent (info->format is still set when the magic matched).
bool probe_image(const std::uint8_t *data, std::size_t size, ImageInfo *info);

struct ImageGuard
{
    long long max_pixels = 4'000'000; // 4 Mpx: 16 MB of RGBA before any resize
    int max_side = 4096;
};

enum class Verdict : std::uint8_t
{
    ok,
    unknown_format,   // not webp/png/jpeg (HTML, JSON, SVG, GIF ...)
    unsupported,      // recognised but not decodable here (AVIF) or animated
    unreadable,       // header truncated or zero-sized
    too_many_pixels,
    side_too_long,
};

const char *name(Verdict v);

Verdict check_image(const std::uint8_t *data, std::size_t size, const ImageGuard &guard,
                    ImageInfo *info = nullptr);

} // namespace wings::art
