// Wings cover-art module: image format sniffing and the pre-decode size guard.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/image_probe.hpp"

#include <algorithm>
#include <cstring>

namespace wings::art
{

const char *name(ImageFormat f)
{
    switch (f)
    {
    case ImageFormat::unknown: return "unknown";
    case ImageFormat::webp: return "webp";
    case ImageFormat::png: return "png";
    case ImageFormat::jpeg: return "jpeg";
    case ImageFormat::avif: return "avif";
    }
    return "?";
}

const char *name(Verdict v)
{
    switch (v)
    {
    case Verdict::ok: return "ok";
    case Verdict::unknown_format: return "unknown image format";
    case Verdict::unsupported: return "unsupported image format";
    case Verdict::unreadable: return "unreadable image header";
    case Verdict::too_many_pixels: return "image has too many pixels";
    case Verdict::side_too_long: return "image side too long";
    }
    return "?";
}

namespace
{
std::uint32_t be32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint32_t le32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
} // namespace

ImageFormat sniff_image(const std::uint8_t *d, std::size_t n)
{
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0)
        return ImageFormat::webp;
    if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0)
        return ImageFormat::png;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return ImageFormat::jpeg;
    if (n >= 12 && std::memcmp(d + 4, "ftyp", 4) == 0 &&
        (std::memcmp(d + 8, "avif", 4) == 0 || std::memcmp(d + 8, "avis", 4) == 0))
        return ImageFormat::avif;
    return ImageFormat::unknown;
}

bool probe_image(const std::uint8_t *d, std::size_t n, ImageInfo *info)
{
    *info = ImageInfo();
    info->format = sniff_image(d, n);
    switch (info->format)
    {
    case ImageFormat::png:
        // 8 magic bytes, then the IHDR chunk: length(4) "IHDR" width(4) height(4)
        if (n < 24 || std::memcmp(d + 12, "IHDR", 4) != 0)
            return false;
        info->width = static_cast<int>(std::min<std::uint32_t>(be32(d + 16), 0x7fffffffu));
        info->height = static_cast<int>(std::min<std::uint32_t>(be32(d + 20), 0x7fffffffu));
        return info->width > 0 && info->height > 0;
    case ImageFormat::webp:
    {
        if (n < 30)
            return false;
        if (std::memcmp(d + 12, "VP8X", 4) == 0)
        {
            info->animated = (d[20] & 0x02) != 0;
            info->width = static_cast<int>((d[24] | (d[25] << 8) | (d[26] << 16)) + 1);
            info->height = static_cast<int>((d[27] | (d[28] << 8) | (d[29] << 16)) + 1);
            return true;
        }
        if (std::memcmp(d + 12, "VP8L", 4) == 0)
        {
            if (d[20] != 0x2f)
                return false;
            const std::uint32_t bits = le32(d + 21);
            info->width = static_cast<int>((bits & 0x3fff) + 1);
            info->height = static_cast<int>(((bits >> 14) & 0x3fff) + 1);
            return true;
        }
        if (std::memcmp(d + 12, "VP8 ", 4) == 0)
        {
            // frame tag (3 bytes), start code 9d 01 2a, 14-bit width, 14-bit height
            if (d[23] != 0x9d || d[24] != 0x01 || d[25] != 0x2a)
                return false;
            info->width = (d[26] | (d[27] << 8)) & 0x3fff;
            info->height = (d[28] | (d[29] << 8)) & 0x3fff;
            return info->width > 0 && info->height > 0;
        }
        return false;
    }
    case ImageFormat::jpeg:
    {
        std::size_t i = 2;
        while (i + 4 <= n)
        {
            if (d[i] != 0xFF)
                return false;
            while (i < n && d[i] == 0xFF) // fill bytes
                ++i;
            if (i >= n)
                return false;
            const std::uint8_t marker = d[i++];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
                continue; // standalone markers have no length
            if (marker == 0xD9 || marker == 0xDA)
                return false; // end of image / start of scan before any frame header
            if (i + 2 > n)
                return false;
            const std::size_t len = static_cast<std::size_t>(d[i] << 8 | d[i + 1]);
            if (len < 2)
                return false;
            const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                             marker != 0xC8 && marker != 0xCC;
            if (sof)
            {
                if (len < 8 || i + 7 > n)
                    return false;
                info->height = d[i + 3] << 8 | d[i + 4];
                info->width = d[i + 5] << 8 | d[i + 6];
                return info->width > 0 && info->height > 0;
            }
            i += len;
        }
        return false;
    }
    default:
        return false;
    }
}

Verdict check_image(const std::uint8_t *d, std::size_t n, const ImageGuard &g, ImageInfo *out)
{
    ImageInfo local;
    ImageInfo *info = out ? out : &local;
    const bool readable = probe_image(d, n, info);
    if (info->format == ImageFormat::unknown)
        return Verdict::unknown_format;
    if (info->format == ImageFormat::avif)
        return Verdict::unsupported;
    if (!readable)
        return Verdict::unreadable;
    if (info->animated)
        return Verdict::unsupported;
    if (info->width > g.max_side || info->height > g.max_side)
        return Verdict::side_too_long;
    if (static_cast<long long>(info->width) * info->height > g.max_pixels)
        return Verdict::too_many_pixels;
    return Verdict::ok;
}

} // namespace wings::art
