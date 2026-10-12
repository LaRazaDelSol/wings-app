// Wings TV app - Artwork: fetched through an ArtSource, decoded off the frame.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/art.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace orbit
{

namespace
{

// A backend may answer 202 while it fetches an image; ask again shortly.
// The poll is cheap (the art service answers from memory, nothing is
// re-fetched), so 0.25 s; the horizon stays about 30 s.
constexpr double kPendingDelay = 0.25;
constexpr int kPendingAttempts = 120; // about 30 s, as before
// More queued requests than this means the player scrolled past them.
constexpr std::size_t kMaxQueued = 96;

std::string make_key(const std::string &art_key, ArtKind kind)
{
    switch (kind)
    {
    case ArtKind::hero:
        return art_key + "#hero";
    case ArtKind::hero_low:
        return art_key + "#low";
    default:
        return art_key + "#cover";
    }
}

// Backgrounds of either kind (they share one budget).
bool is_hero(const std::string &key)
{
    const auto ends = [&key](const char *tail, std::size_t n)
    { return key.size() > n && key.compare(key.size() - n, n, tail) == 0; };
    return ends("#hero", 5) || ends("#low", 4);
}

} // namespace

ArtCache::ArtCache(ArtSource &source, TextureSink &sink, ArtLimits limits)
    : source_(source), sink_(sink), limits_(limits)
{
}

ArtCache::~ArtCache()
{
    stop();
}

void ArtCache::start()
{
    thread_.start([this] { worker(); });
}

void ArtCache::stop()
{
    {
        Lock lock(mutex_);
        stopping_ = true;
    }
    signal_.notify();
    thread_.join();
}

const Artwork *ArtCache::get(const std::string &art_key, ArtKind kind)
{
    const std::string key = make_key(art_key, kind);
    const double now = now_seconds();
    auto found = entries_.find(key);
    if (found != entries_.end())
    {
        Entry &entry = found->second;
        entry.last_used = now;
        if (entry.state == State::ready)
            return &entry.art;
        if (entry.state == State::loading || now < entry.retry_at)
            return nullptr;
    }
    Entry &entry = entries_[key];
    entry.state = State::loading;
    entry.last_used = now;
    {
        Lock lock(mutex_);
        Request request;
        request.key = key;
        request.art_key = art_key;
        request.kind = kind;
        requests_.push_back(std::move(request));
        // Anything past the limit was asked for long ago and is likely gone
        // from the screen. It is marked failed briefly and asked again if it
        // is drawn again.
        while (requests_.size() > kMaxQueued)
        {
            const std::string dropped = requests_.front().key;
            requests_.pop_front();
            inbox_.post(
                [this, dropped]
                {
                    auto it = entries_.find(dropped);
                    if (it != entries_.end() && it->second.state == State::loading)
                        entries_.erase(it);
                });
        }
    }
    signal_.notify();
    return nullptr;
}

void ArtCache::tick()
{
    inbox_.drain();
    const double now = now_seconds();
    for (int n = 0; n < limits_.uploads_per_frame && !uploads_.empty(); ++n)
    {
        Decoded decoded = std::move(uploads_.front());
        uploads_.pop_front();
        pending_uploads_.fetch_sub(1);
        signal_.notify(); // the worker may have been waiting for room
        auto found = entries_.find(decoded.key);
        if (found == entries_.end())
            continue; // dropped while it was decoding
        Entry &entry = found->second;
        if (!decoded.ok)
        {
            entry.state = State::failed;
            entry.retry_at = now + limits_.retry_failed;
            continue;
        }
        entry.art.texture = sink_.create(decoded.pixels);
        entry.art.width = decoded.pixels.width;
        entry.art.height = decoded.pixels.height;
        entry.art.ambient = decoded.ambient.empty() ? 0 : sink_.create(decoded.ambient);
        entry.art.ready_at = now;
        entry.state = entry.art.texture != 0 ? State::ready : State::failed;
        if (entry.state == State::failed)
            entry.retry_at = now + limits_.retry_failed;
    }
    evict(ArtKind::cover, limits_.max_covers);
    evict(ArtKind::hero, limits_.max_heroes);
}

void ArtCache::evict(ArtKind kind, int keep)
{
    std::vector<std::pair<double, std::string>> ready;
    for (const auto &[key, entry] : entries_)
    {
        if (entry.state == State::ready && is_hero(key) == (kind == ArtKind::hero))
            ready.emplace_back(entry.last_used, key);
    }
    if (static_cast<int>(ready.size()) <= keep)
        return;
    std::sort(ready.begin(), ready.end());
    const double now = now_seconds();
    const std::size_t extra = ready.size() - static_cast<std::size_t>(keep);
    for (std::size_t i = 0; i < extra; ++i)
    {
        // Never take away something drawn in the last frame or so.
        if (now - ready[i].first < 0.5)
            break;
        auto it = entries_.find(ready[i].second);
        sink_.destroy(it->second.art.texture);
        if (it->second.art.ambient != 0)
            sink_.destroy(it->second.art.ambient);
        entries_.erase(it);
    }
}

bool ArtCache::idle()
{
    inbox_.drain();
    Lock lock(mutex_);
    return requests_.empty() && in_flight_ == 0 && uploads_.empty();
}

void ArtCache::clear()
{
    for (auto &[key, entry] : entries_)
    {
        if (entry.state != State::ready)
            continue;
        sink_.destroy(entry.art.texture);
        if (entry.art.ambient != 0)
            sink_.destroy(entry.art.ambient);
    }
    entries_.clear();
    pending_uploads_.fetch_sub(static_cast<int>(uploads_.size()));
    uploads_.clear();
}

void ArtCache::retry_failed()
{
    for (auto &[key, entry] : entries_)
    {
        if (entry.state == State::failed)
            entry.retry_at = 0.0;
    }
}

bool ArtCache::work_once()
{
    // The frame thread uploads a couple of textures per frame: do not decode
    // further ahead than a few frames' worth.
    if (pending_uploads_.load() >= limits_.max_pending_uploads)
        return false;
    Request request;
    {
        Lock lock(mutex_);
        const double now = now_seconds();
        // Newest first: the player is looking at what was asked for last.
        auto pick = requests_.end();
        for (auto it = requests_.end(); it != requests_.begin();)
        {
            --it;
            if (it->not_before <= now)
            {
                pick = it;
                break;
            }
        }
        if (pick == requests_.end())
            return false;
        request = std::move(*pick);
        requests_.erase(pick);
        ++in_flight_;
    }

    Result<Art> art = source_.art(request.art_key, request.kind);
    Decoded decoded;
    decoded.key = request.key;
    // Still being fetched by the backend, or its artwork workers are busy or
    // starting (503): ask again shortly rather than giving up.
    const bool again = (art.ok() && art.value.pending) || art.status == 503;
    if (again && request.attempts < kPendingAttempts)
    {
        Lock lock(mutex_);
        request.not_before = now_seconds() + kPendingDelay;
        ++request.attempts;
        requests_.push_front(std::move(request));
        --in_flight_;
        return true;
    }
    if (art.ok() && !art.value.pending)
    {
        const bool hero = request.kind != ArtKind::cover;
        const int width = hero ? limits_.hero_width : limits_.cover_size;
        const int height = hero ? limits_.hero_height : limits_.cover_size;
        decoded.ok = image::decode(art.value.bytes, width, height, &decoded.pixels);
        if (decoded.ok && !hero)
            decoded.ambient = image::ambient(decoded.pixels);
    }
    pending_uploads_.fetch_add(1);
    inbox_.post([this, decoded = std::move(decoded)]() mutable
                { uploads_.push_back(std::move(decoded)); });
    {
        Lock lock(mutex_);
        --in_flight_;
    }
    return true;
}

void ArtCache::worker()
{
    for (;;)
    {
        {
            Lock lock(mutex_);
            if (stopping_)
                return;
        }
        if (work_once())
            continue;
        Lock lock(mutex_);
        if (stopping_)
            return;
        // Sleep until a request arrives or a pending one becomes due.
        double wait = 1.0;
        const double now = now_seconds();
        for (const Request &request : requests_)
            wait = std::min(wait, std::max(0.01, request.not_before - now));
        signal_.wait(mutex_, wait);
    }
}

} // namespace orbit
