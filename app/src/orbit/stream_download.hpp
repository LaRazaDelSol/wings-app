// Wings TV app - streaming download to disk: a big HTTPS body goes to a .part file in chunks (never held
// in memory), is hashed on the way, checked against the size and SHA-256 the release lists and, for a
// package image, against the UFS2 superblock magic, and only then renamed into place.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure pieces (limits, free-space rule, magic rule, ChunkSink) have no network; the network arrives
// through the StreamingDownloader interface (main.cpp wires it to the art service's TLS client).
// Pattern after the Pegasus release downloader in pegasus_manager.cpp and Orbit's backend/tvapp.c.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace orbit
{

// An image Wings will fetch is at most this big; after it the volume keeps this much free.
constexpr std::uint64_t kStreamMaxImage = 256ull << 20;
constexpr std::uint64_t kStreamSpare = 64ull << 20;
// UFS2 superblock magic 0x19540119 (stored little endian: 19 01 54 19) at 65536 + 1372.
constexpr std::uint64_t kUfs2MagicOffset = 65536 + 1372;

// The four magic bytes read at kUfs2MagicOffset.
bool ufs2_magic_ok(const std::uint8_t *four);
// A listed size is acceptable: big enough to hold the magic, at most kStreamMaxImage.
bool stream_size_ok(std::uint64_t want);
// Enough room for `want` plus kStreamSpare. free_bytes < 0 means unknown, which does not block.
bool stream_space_ok(double free_bytes, std::uint64_t want);

enum class StreamError : std::uint8_t
{
    none,
    cancelled,
    bad_request,   // size outside the limits, empty paths, bad checksum text
    no_space,      // less free space than size + kStreamSpare
    network,       // transport failure; StreamResult::detail says which
    http_status,   // non-2xx final status (StreamResult::status)
    too_large,     // more bytes arrived than listed
    size_mismatch, // fewer bytes than listed
    checksum,      // SHA-256 differs
    not_image,     // UFS2 magic missing
    disk           // open/write/flush/rename failed
};

// Receives the body in order. Writes the .part file, hashes it, counts, caps, reports, polls cancel.
class ChunkSink
{
  public:
    struct Config
    {
        std::string part_path;
        std::uint64_t limit = 0; // bytes accepted in total
        std::function<void(std::uint64_t)> progress;
        std::function<bool()> cancel; // true: stop
    };
    explicit ChunkSink(Config config);
    ~ChunkSink(); // deletes the .part file unless finish() succeeded
    ChunkSink(const ChunkSink &) = delete;
    ChunkSink &operator=(const ChunkSink &) = delete;

    bool open();                                          // creates/truncates the .part file
    bool write(const std::uint8_t *data, std::size_t n);  // false: failed() says why, the file is gone
    // After the last byte: flush + fsync + close. Fills hex and *ufs2 (magic seen at its offset).
    bool finish(std::string *sha256_hex, bool *ufs2);
    void abort(); // close and delete the .part file

    std::uint64_t bytes() const { return bytes_; }
    StreamError failed() const { return failure_; }

  private:
    bool fail(StreamError why);
    Config config_;
    std::FILE *file_ = nullptr;
    void *hash_ = nullptr; // Sha256 (kept out of this header)
    std::uint64_t bytes_ = 0;
    std::uint8_t magic_[4] = {0, 0, 0, 0};
    StreamError failure_ = StreamError::none;
    bool done_ = false;
};

struct StreamHooks
{
    std::function<bool(const std::string &)> redirect_ok; // false refuses that redirect target
    std::function<void(std::uint64_t)> progress;          // body bytes so far
    std::function<bool()> cancel;                         // true: stop
};

class StreamingDownloader
{
  public:
    virtual ~StreamingDownloader() = default;
    // The same network gate as the cover art. False with a sentence.
    virtual bool ready(std::string *why) = 0;
    // Blocks (worker thread). Delivers the final 2xx body to `sink` (false aborts) in at most 16 KiB
    // pieces; nothing is delivered for other statuses. *status is the final HTTP status (0: none).
    // Returns "" on success, else a short reason.
    virtual std::string get_stream(const std::string &url, std::uint64_t max_bytes, const StreamHooks &hooks,
                                   const std::function<bool(const std::uint8_t *, std::size_t)> &sink,
                                   int *status) = 0;
};

// https on github.com, objects.githubusercontent.com or release-assets.githubusercontent.com only
// (the same rule the Pegasus downloader pins its redirects to).
bool stream_redirect_allowed(const std::string &url);

struct StreamRequest
{
    std::string url;            // the caller already validated the first URL
    std::string final_path;     // the finished file; "<final_path>.part" is the work file
    std::uint64_t size = 0;     // listed size, exact
    std::string sha256;         // listed digest, 64 lower-case hex
    bool require_ufs2 = true;   // check the package image magic
    StreamHooks hooks;          // redirect_ok defaults to stream_redirect_allowed
    // Free bytes on the volume holding final_path, negative/unset: unknown (not blocking).
    std::function<double(const std::string &path)> free_bytes;
};

struct StreamResult
{
    bool ok = false;
    StreamError error = StreamError::none;
    int status = 0;
    std::string detail;       // transport reason
    std::uint64_t bytes = 0;  // bytes received
    std::string sha256;       // of what was received (when it got that far)
};

// Checks, streams, verifies, renames. On any failure the .part file is deleted and final_path is
// left as it was; on success the .part file replaced final_path.
StreamResult stream_download_to_file(StreamingDownloader &downloader, const StreamRequest &request);

} // namespace orbit
