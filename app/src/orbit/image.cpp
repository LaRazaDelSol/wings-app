// Wings TV app - Artwork decoding: WebP, PNG and JPEG to sized RGBA.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/image.hpp"

#include <webp/decode.h>

#include <algorithm>
#include <cstring>

extern "C"
{
    unsigned char *stbi_load_from_memory(const unsigned char *buffer, int length, int *x, int *y,
                                         int *channels, int desired_channels);
    int stbi_info_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp);
    void stbi_image_free(void *pixels);
    const char *stbi_failure_reason(void);
}

namespace orbit::image
{

namespace
{

// Images larger than this are refused BEFORE any pixel buffer is allocated:
// a hostile or broken file must not exhaust the fixed heap. 4 Mpx is 16 MB of
// RGBA; PS CDN posters are 260 px, Pegasus covers are a few hundred.
// (wings/art/docs/IMAGE_GUARD.md; the art module applies the same limit
// before anything is cached.)
constexpr int kMaxSourceSide = 4096;
constexpr long long kMaxSourcePixels = 4'000'000;

bool size_allowed(int width, int height)
{
    return width > 0 && height > 0 && width <= kMaxSourceSide && height <= kMaxSourceSide &&
           static_cast<long long>(width) * height <= kMaxSourcePixels;
}

Pixels resize_raw(const std::uint8_t *rgba, int in_width, int in_height, int width, int height);

void set_error(std::string *error, const char *reason)
{
    if (error != nullptr)
        *error = reason;
}

void fit(int width, int height, int max_width, int max_height, int *out_width, int *out_height)
{
    double scale = 1.0;
    if (width > max_width)
        scale = static_cast<double>(max_width) / width;
    if (height * scale > max_height)
        scale = static_cast<double>(max_height) / height;
    *out_width = std::max(1, static_cast<int>(width * scale + 0.5));
    *out_height = std::max(1, static_cast<int>(height * scale + 0.5));
}

bool decode_webp(std::string_view bytes, int max_width, int max_height, Pixels *out,
                 std::string *error)
{
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config))
    {
        set_error(error, "WebP decoder unavailable");
        return false;
    }
    const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
    if (WebPGetFeatures(data, bytes.size(), &config.input) != VP8_STATUS_OK)
    {
        set_error(error, "not a readable WebP image");
        return false;
    }
    if (config.input.has_animation)
    {
        set_error(error, "animated WebP is not supported");
        return false;
    }
    const int width = config.input.width;
    const int height = config.input.height;
    if (!size_allowed(width, height))
    {
        set_error(error, "WebP image size out of range");
        return false;
    }
    int target_width = 0;
    int target_height = 0;
    fit(width, height, max_width, max_height, &target_width, &target_height);
    if (target_width != width || target_height != height)
    {
        config.options.use_scaling = 1;
        config.options.scaled_width = target_width;
        config.options.scaled_height = target_height;
    }
    // Decode straight into our own buffer: no second full-size copy.
    out->width = target_width;
    out->height = target_height;
    out->rgba.assign(static_cast<std::size_t>(target_width) * target_height * 4, 0);
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = out->rgba.data();
    config.output.u.RGBA.stride = target_width * 4;
    config.output.u.RGBA.size = out->rgba.size();
    const VP8StatusCode status = WebPDecode(data, bytes.size(), &config);
    WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK)
    {
        *out = Pixels();
        set_error(error, "WebP decoding failed");
        return false;
    }
    return true;
}

bool decode_stb(std::string_view bytes, int max_width, int max_height, Pixels *out,
                std::string *error)
{
    if (bytes.size() > 0x7fffffff)
    {
        set_error(error, "image too large");
        return false;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    // Header only: stb reads the PNG IHDR / JPEG SOF without allocating pixels.
    if (stbi_info_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                              static_cast<int>(bytes.size()), &width, &height, &channels) == 0)
    {
        set_error(error, "not a readable image");
        return false;
    }
    if (!size_allowed(width, height))
    {
        set_error(error, "image size out of range");
        return false;
    }
    unsigned char *pixels =
        stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                              static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        set_error(error, stbi_failure_reason() != nullptr ? stbi_failure_reason()
                                                          : "image decoding failed");
        return false;
    }
    if (!size_allowed(width, height))
    {
        stbi_image_free(pixels);
        set_error(error, "image size out of range");
        return false;
    }
    int target_width = 0;
    int target_height = 0;
    fit(width, height, max_width, max_height, &target_width, &target_height);
    if (target_width == width && target_height == height)
    {
        out->width = width;
        out->height = height;
        out->rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    }
    else
    {
        // Reduce straight from the decoder's buffer: no second full-size copy.
        *out = resize_raw(pixels, width, height, target_width, target_height);
    }
    stbi_image_free(pixels);
    return true;
}

// One axis of an area-averaging resample: each output sample is the average
// of the source span it covers, with fractional weights at the ends.
template <typename T>
void resample_row(const T *in, int in_count, int in_stride, float *out, int out_count,
                  int out_stride)
{
    const double scale = static_cast<double>(in_count) / out_count;
    for (int o = 0; o < out_count; ++o)
    {
        const double start = o * scale;
        const double end = std::min(static_cast<double>(in_count), (o + 1) * scale);
        float sum[4] = {};
        double weight = 0.0;
        for (int i = static_cast<int>(start); i < static_cast<int>(end + 0.999999) && i < in_count;
             ++i)
        {
            const double left = std::max(start, static_cast<double>(i));
            const double right = std::min(end, static_cast<double>(i + 1));
            double w = right - left;
            if (scale < 1.0)
                w = 1.0; // enlargement: nearest source sample
            if (w <= 0.0)
                continue;
            for (int c = 0; c < 4; ++c)
                sum[c] += static_cast<float>(w) * static_cast<float>(in[i * in_stride + c]);
            weight += w;
            if (scale < 1.0)
                break;
        }
        for (int c = 0; c < 4; ++c)
            out[o * out_stride + c] = weight > 0.0 ? static_cast<float>(sum[c] / weight) : 0.0f;
    }
}

void box_blur(std::vector<float> &plane, int width, int height, int radius)
{
    std::vector<float> line(static_cast<std::size_t>(std::max(width, height)) * 4);
    const auto pass = [&](int count, int lines, int step, int line_step)
    {
        for (int l = 0; l < lines; ++l)
        {
            float *base = plane.data() + static_cast<std::size_t>(l) * line_step * 4;
            for (int i = 0; i < count; ++i)
            {
                float sum[4] = {};
                for (int k = -radius; k <= radius; ++k)
                {
                    const int j = std::clamp(i + k, 0, count - 1);
                    for (int c = 0; c < 4; ++c)
                        sum[c] += base[static_cast<std::size_t>(j) * step * 4 + c];
                }
                for (int c = 0; c < 4; ++c)
                    line[static_cast<std::size_t>(i) * 4 + c] = sum[c] / (2 * radius + 1);
            }
            for (int i = 0; i < count; ++i)
            {
                for (int c = 0; c < 4; ++c)
                    base[static_cast<std::size_t>(i) * step * 4 + c] =
                        line[static_cast<std::size_t>(i) * 4 + c];
            }
        }
    };
    pass(width, height, 1, width); // rows
    pass(height, width, width, 1); // columns
}

Pixels resize_raw(const std::uint8_t *rgba, int in_width, int in_height, int width, int height)
{
    Pixels out;
    if (rgba == nullptr || in_width <= 0 || in_height <= 0 || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    // Streaming separable area filter: each output row averages the source
    // rows it covers, each of those resampled horizontally on the fly. Only
    // two rows of floats are alive at a time, whatever the source size.
    std::vector<float> row(static_cast<std::size_t>(width) * 4);
    std::vector<float> sum(static_cast<std::size_t>(width) * 4);
    const double scale = static_cast<double>(in_height) / height;
    for (int y = 0; y < height; ++y)
    {
        const double top = y * scale;
        const double bottom = std::min(static_cast<double>(in_height), (y + 1) * scale);
        std::fill(sum.begin(), sum.end(), 0.0f);
        double weight = 0.0;
        for (int source = static_cast<int>(top); source < in_height; ++source)
        {
            double w = std::min(bottom, static_cast<double>(source + 1)) -
                       std::max(top, static_cast<double>(source));
            if (scale < 1.0)
                w = 1.0; // enlargement: the nearest source row
            if (w <= 0.0)
                break;
            resample_row(rgba + static_cast<std::size_t>(source) * in_width * 4, in_width, 4,
                         row.data(), width, 4);
            for (std::size_t i = 0; i < sum.size(); ++i)
                sum[i] += static_cast<float>(w) * row[i];
            weight += w;
            if (scale < 1.0 || source + 1 >= bottom)
                break;
        }
        std::uint8_t *target = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::size_t i = 0; i < sum.size(); ++i)
        {
            const float value = weight > 0.0 ? static_cast<float>(sum[i] / weight) : 0.0f;
            target[i] = static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

} // namespace

Format sniff(std::string_view bytes)
{
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP")
        return Format::webp;
    if (bytes.size() >= 8 && bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8))
        return Format::png;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8 &&
        static_cast<unsigned char>(bytes[2]) == 0xFF)
        return Format::jpeg;
    return Format::unknown;
}

bool decode(std::string_view bytes, int max_width, int max_height, Pixels *out, std::string *error)
{
    *out = Pixels();
    if (max_width <= 0 || max_height <= 0)
    {
        set_error(error, "invalid target size");
        return false;
    }
    switch (sniff(bytes))
    {
    case Format::webp:
        return decode_webp(bytes, max_width, max_height, out, error);
    case Format::png:
    case Format::jpeg:
        return decode_stb(bytes, max_width, max_height, out, error);
    default:
        set_error(error, "unsupported image format");
        return false;
    }
}

Pixels resize(const Pixels &in, int width, int height)
{
    if (in.empty() || in.rgba.size() < static_cast<std::size_t>(in.width) * in.height * 4)
        return Pixels();
    return resize_raw(in.rgba.data(), in.width, in.height, width, height);
}

Pixels ambient(const Pixels &in, int size)
{
    Pixels small = resize(in, size, size);
    if (small.empty())
        return small;
    std::vector<float> plane(small.rgba.begin(), small.rgba.end());
    // Three box passes approximate a Gaussian; the radius is large for the
    // size, so the result is a soft field of the cover's main colours.
    for (int pass = 0; pass < 3; ++pass)
        box_blur(plane, small.width, small.height, std::max(1, size / 10));
    for (std::size_t i = 0; i < plane.size(); i += 4)
    {
        for (int c = 0; c < 3; ++c)
            small.rgba[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(
                std::clamp(plane[i + static_cast<std::size_t>(c)] * 0.82f, 0.0f, 255.0f));
        small.rgba[i + 3] = 255;
    }
    return small;
}

} // namespace orbit::image
