// Wings cover-art module: URL-keyed disk cache with atomic writes and LRU eviction.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout:  <root>/<xx>/<fnv1a64(url) as 16 hex>.art   original compressed bytes
//          <root>/<xx>/<same>.neg                      negative-cache record
//          <root>/index.v1                             size + last-use per entry
//
// * Each .art file carries a header (magic, URL, fetch time, payload length,
//   checksum): a hash collision, a truncated or a bit-rotted file is detected
//   and treated as a miss (and deleted).
// * Publication is atomic: write "<file>.tmp", fsync, rename. Leftover .tmp
//   files from a crash are deleted at open().
// * LRU is kept in the index (not file mtimes: utimes is not available to a
//   native title) and persisted every few changes and at close. The index is
//   reconciled with a directory scan at open; if the console's readdir shows
//   nothing, a sample of the index is stat()ed and trusted when files exist.
// * Eviction runs at open() and after every `evict_every_writes` puts, down
//   to `budget_bytes` (default 96 MiB).
// * Game-page backgrounds are 5-10 times the size of a cover, so they have a budget of their own
//   (`hero_budget_bytes`) and cannot push covers out. A background fetched ahead of time (the focused
//   tile) is "speculative" until a page really reads it; one nobody opened for half an hour is evicted first.
// Thread-safe.
#pragma once

#include "wings/art/platform.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace wings::art
{

struct DiskCacheConfig
{
    std::string root;                           // e.g. "/download0/wings/art"
    std::uint64_t budget_bytes = 96ull << 20;   // LRU eviction target
    std::uint64_t max_entry_bytes = 8ull << 20; // refuse larger payloads
    // Backgrounds (hero and speculative entries together) may hold this much; 0: no separate limit.
    std::uint64_t hero_budget_bytes = 24ull << 20;
    int evict_every_writes = 200;
    int index_flush_every = 64;                 // index changes between index writes
    const Clock *clock = nullptr;               // default: system clock
};

enum class CacheGet : std::uint8_t
{
    miss,
    hit,
};

enum class EntryKind : std::uint8_t
{
    normal,      // a cover or anything else
    hero,        // a game-page background that was asked for by a page
    speculative, // a background fetched ahead of time and not read yet (a read makes it `hero`)
};

class DiskCache
{
  public:
    explicit DiskCache(DiskCacheConfig config);
    ~DiskCache();
    DiskCache(const DiskCache &) = delete;
    DiskCache &operator=(const DiskCache &) = delete;

    // Creates the directories, loads/reconciles the index, removes leftovers,
    // evicts. False (with *error) when the root cannot be created or written.
    bool open(std::string *error);
    bool is_open() const { return open_; }

    // A valid entry: bytes, and when it was fetched (unix seconds).
    CacheGet get(const std::string &url, std::string *bytes, std::int64_t *fetched_at);
    bool put(const std::string &url, const std::uint8_t *data, std::size_t size,
             EntryKind kind = EntryKind::normal);
    void remove(const std::string &url);
    // Index lookup only (no file access): is a good entry recorded for url?
    bool contains(const std::string &url);

    // Negative records: "this URL is permanently bad until expires_at".
    bool put_negative(const std::string &url, const std::string &reason, std::int64_t ttl_seconds);
    bool get_negative(const std::string &url, std::string *reason); // false when absent/expired
    void clear_negatives();                                          // "retry failed covers"

    void evict();               // now, down to the budget
    void flush();               // write the index now
    void clear();               // delete everything (Settings > Clear art cache)

    struct Stats
    {
        std::uint64_t entries = 0, negatives = 0, bytes = 0;
        std::uint64_t hits = 0, misses = 0, writes = 0, evicted = 0, corrupt = 0;
    };
    Stats stats();
    const std::string &root() const { return config_.root; }

    static std::uint64_t hash_url(const std::string &url);
    std::string path_for(const std::string &url, bool negative) const; // for tests

  private:
    struct Item
    {
        std::uint64_t size = 0;
        std::int64_t last_used = 0;
        bool negative = false;
        EntryKind entry = EntryKind::normal;
        std::int64_t expires = 0; // negative records only (unix seconds)
    };
    void evict_locked();
    void flush_locked();
    void note_change_locked();
    void drop_locked(std::uint64_t hash);
    std::string path_for_hash(std::uint64_t hash, bool negative) const;

    DiskCacheConfig config_;
    const Clock *clock_;
    Mutex mutex_;
    std::map<std::uint64_t, Item> index_;
    std::uint64_t total_ = 0;
    int writes_since_evict_ = 0, changes_since_flush_ = 0;
    bool open_ = false;
    Stats stats_;
};

} // namespace wings::art
