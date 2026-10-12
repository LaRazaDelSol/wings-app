// Wings TV app - Artwork: fetched through an ArtSource, decoded off the frame.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Screens ask for a cover or background every frame they draw it. The
// first ask queues a fetch (ArtSource::art) on the art worker, which
// decodes the image at drawing size. The frame thread turns finished images
// into textures, a couple per frame so a burst of arrivals cannot stall a
// frame, and evicts the least recently drawn when over budget. The newest
// request is served first: what is on screen now loads before what scrolled
// past.

#pragma once

#include "orbit/backend.hpp"
#include "orbit/image.hpp"
#include "orbit/thread.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace orbit
{

// Where textures come from: OpenGL in the app and the preview, a fake in tests.
class TextureSink
{
  public:
    virtual ~TextureSink() = default;
    virtual std::uint32_t create(const image::Pixels &pixels) = 0;
    virtual void destroy(std::uint32_t texture) = 0;
};

struct Artwork
{
    std::uint32_t texture = 0;
    int width = 0;
    int height = 0;
    std::uint32_t ambient = 0; // covers only: the blurred colour wash
    double ready_at = 0.0;     // when it became drawable, for a fade-in
};

struct ArtLimits
{
    int cover_size = 400;   // longest side of a decoded cover
    int hero_width = 1920;  // backgrounds fit inside this ...
    int hero_height = 1080; // ... and this
    int max_covers = 96;    // textures kept before the least recent go (4K grids)
    int max_heroes = 4;
    int uploads_per_frame = 2;
    double retry_failed = 60.0; // seconds before a failed image is asked again
    // Decoded images waiting for upload: the worker stops decoding at this many
    // (the frame thread uploads uploads_per_frame per frame).
    int max_pending_uploads = 8;
};

class ArtCache
{
  public:
    ArtCache(ArtSource &source, TextureSink &sink, ArtLimits limits = {});
    ~ArtCache();
    ArtCache(const ArtCache &) = delete;
    ArtCache &operator=(const ArtCache &) = delete;

    void start();
    void stop();
    // Frame thread: the image when it is ready, else nullptr (and it is asked for).
    const Artwork *get(const std::string &art_key, ArtKind kind);
    // Frame thread, once per frame: textures from finished decodes, eviction.
    void tick();
    // Nothing queued, fetching or waiting for upload (previews wait for this).
    bool idle();
    // Releases every texture (before the GL context goes away).
    void clear();
    // Frame thread: images that failed are asked for again the next time they are
    // drawn instead of after limits.retry_failed (Settings turned something on,
    // or the player asked to retry).
    void retry_failed();

    // Worker step, public for tests: serves one request. False when none was ready.
    bool work_once();

  private:
    enum class State : std::uint8_t
    {
        loading,
        ready,
        failed,
    };
    struct Entry
    {
        State state = State::loading;
        Artwork art;
        double last_used = 0.0;
        double retry_at = 0.0;
    };
    struct Request
    {
        std::string key;
        std::string art_key;
        ArtKind kind = ArtKind::cover;
        double not_before = 0.0;
        int attempts = 0;
    };
    struct Decoded
    {
        std::string key;
        bool ok = false;
        image::Pixels pixels;
        image::Pixels ambient;
    };

    void worker();
    void evict(ArtKind kind, int keep);

    ArtSource &source_;
    TextureSink &sink_;
    ArtLimits limits_;
    std::map<std::string, Entry> entries_; // frame thread only
    Inbox inbox_;
    std::deque<Decoded> uploads_; // frame thread only
    // Decodes posted or waiting in uploads_: backpressure on the worker.
    std::atomic<int> pending_uploads_{0};

    Mutex mutex_;
    Signal signal_;
    std::deque<Request> requests_;
    int in_flight_ = 0;
    bool stopping_ = false;
    Thread thread_;
};

} // namespace orbit
