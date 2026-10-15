// Wings TV app - streaming download to disk (see stream_download.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/stream_download.hpp"

#include "orbit/pegasus_manager.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace orbit
{

bool ufs2_magic_ok(const std::uint8_t *four)
{
    return four != nullptr && four[0] == 0x19 && four[1] == 0x01 && four[2] == 0x54 && four[3] == 0x19;
}

bool stream_size_ok(std::uint64_t want)
{
    return want >= kUfs2MagicOffset + 4 && want <= kStreamMaxImage;
}

bool stream_space_ok(double free_bytes, std::uint64_t want)
{
    if (free_bytes < 0.0)
        return true;
    return free_bytes >= static_cast<double>(want) + static_cast<double>(kStreamSpare);
}

bool stream_redirect_allowed(const std::string &url)
{
    return pegasus_url_allowed(url, PegasusUrlRole::redirect);
}

ChunkSink::ChunkSink(Config config) : config_(std::move(config)) {}

ChunkSink::~ChunkSink()
{
    if (!done_)
        abort();
    delete static_cast<Sha256 *>(hash_);
}

bool ChunkSink::fail(StreamError why)
{
    if (failure_ == StreamError::none)
        failure_ = why;
    abort();
    return false;
}

bool ChunkSink::open()
{
    if (file_ != nullptr || config_.part_path.empty())
        return fail(StreamError::bad_request);
    file_ = std::fopen(config_.part_path.c_str(), "wb");
    if (file_ == nullptr)
        return fail(StreamError::disk);
    std::setvbuf(file_, nullptr, _IOFBF, 64u << 10);
    delete static_cast<Sha256 *>(hash_);
    hash_ = new Sha256();
    bytes_ = 0;
    std::memset(magic_, 0, sizeof magic_);
    failure_ = StreamError::none;
    done_ = false;
    return true;
}

bool ChunkSink::write(const std::uint8_t *data, std::size_t n)
{
    if (file_ == nullptr)
        return false;
    if (config_.cancel && config_.cancel())
        return fail(StreamError::cancelled);
    if (n == 0)
        return true;
    if (bytes_ + n > config_.limit)
        return fail(StreamError::too_large);
    // Keep the four magic bytes as they go by (no second read of the file).
    const std::uint64_t lo = kUfs2MagicOffset, hi = kUfs2MagicOffset + 4;
    if (bytes_ < hi && bytes_ + n > lo)
    {
        for (std::uint64_t at = std::max(bytes_, lo); at < std::min<std::uint64_t>(bytes_ + n, hi); ++at)
            magic_[at - lo] = data[at - bytes_];
    }
    if (std::fwrite(data, 1, n, file_) != n)
        return fail(StreamError::disk);
    static_cast<Sha256 *>(hash_)->update(data, n);
    bytes_ += n;
    if (config_.progress)
        config_.progress(bytes_);
    return true;
}

bool ChunkSink::finish(std::string *sha256_hex, bool *ufs2)
{
    if (file_ == nullptr)
        return false;
    bool good = std::fflush(file_) == 0;
    good = good && ::fsync(::fileno(file_)) == 0;
    good = (std::fclose(file_) == 0) && good;
    file_ = nullptr;
    if (!good)
        return fail(StreamError::disk);
    if (sha256_hex != nullptr)
        *sha256_hex = static_cast<Sha256 *>(hash_)->finish_hex();
    if (ufs2 != nullptr)
        *ufs2 = bytes_ >= kUfs2MagicOffset + 4 && ufs2_magic_ok(magic_);
    done_ = true;
    return true;
}

void ChunkSink::abort()
{
    if (file_ != nullptr)
    {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!done_ && !config_.part_path.empty())
        std::remove(config_.part_path.c_str());
}

StreamResult stream_download_to_file(StreamingDownloader &downloader, const StreamRequest &request)
{
    StreamResult out;
    auto fail = [&out](StreamError e)
    {
        out.ok = false;
        out.error = e;
        return out;
    };
    std::string hex_check = request.sha256;
    const bool hex_ok = hex_check.size() == 64 &&
                        hex_check.find_first_not_of("0123456789abcdef") == std::string::npos;
    if (request.url.empty() || request.final_path.empty() || !hex_ok || !stream_size_ok(request.size))
        return fail(StreamError::bad_request);
    if (request.free_bytes && !stream_space_ok(request.free_bytes(request.final_path), request.size))
        return fail(StreamError::no_space);

    const std::string part = request.final_path + ".part";
    ChunkSink::Config cfg;
    cfg.part_path = part;
    cfg.limit = request.size;
    cfg.progress = request.hooks.progress;
    cfg.cancel = request.hooks.cancel;
    ChunkSink sink(std::move(cfg));
    if (!sink.open())
        return fail(sink.failed());

    StreamHooks hooks = request.hooks;
    if (!hooks.redirect_ok)
        hooks.redirect_ok = &stream_redirect_allowed;
    // The transport's own cap is one byte over the listed size, so an oversize file is noticed.
    const std::string why = downloader.get_stream(
        request.url, request.size + 1, hooks,
        [&sink](const std::uint8_t *p, std::size_t n) { return sink.write(p, n); }, &out.status);
    out.bytes = sink.bytes();
    if (sink.failed() != StreamError::none)
        return fail(sink.failed()); // cancel, too much data or disk; the .part file is gone
    if (!why.empty())
    {
        out.detail = why;
        sink.abort();
        if (hooks.cancel && hooks.cancel())
            return fail(StreamError::cancelled);
        return fail(StreamError::network);
    }
    if (out.status < 200 || out.status > 299)
    {
        sink.abort();
        return fail(StreamError::http_status);
    }
    if (sink.bytes() != request.size)
    {
        sink.abort();
        return fail(StreamError::size_mismatch);
    }
    bool ufs2 = false;
    if (!sink.finish(&out.sha256, &ufs2))
        return fail(StreamError::disk);
    auto drop = [&]()
    {
        std::remove(part.c_str());
    };
    if (out.sha256 != request.sha256)
    {
        drop();
        return fail(StreamError::checksum);
    }
    if (request.require_ufs2 && !ufs2)
    {
        drop();
        return fail(StreamError::not_image);
    }
    std::remove(request.final_path.c_str());
    if (std::rename(part.c_str(), request.final_path.c_str()) != 0)
    {
        drop();
        return fail(StreamError::disk);
    }
    out.ok = true;
    return out;
}

} // namespace orbit
