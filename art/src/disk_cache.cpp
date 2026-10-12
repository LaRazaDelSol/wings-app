// Wings cover-art module: URL-keyed disk cache with atomic writes and LRU eviction.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/disk_cache.hpp"

#include "fsutil.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

namespace wings::art
{

namespace
{

constexpr char kArtMagic[8] = {'W', 'A', 'R', 'T', 'v', '0', '0', '1'};
constexpr char kNegMagic[8] = {'W', 'N', 'E', 'G', 'v', '0', '0', '1'};
constexpr std::size_t kArtHeader = 8 + 4 + 4 + 8 + 4;

void put_u32(std::string *s, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put_u64(std::string *s, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
std::uint32_t get_u32(const char *p)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    return v;
}
std::uint64_t get_u64(const char *p)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    return v;
}

std::uint32_t fnv32(const char *p, std::size_t n)
{
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; ++i)
        h = (h ^ static_cast<unsigned char>(p[i])) * 16777619u;
    return h;
}

bool ends_with(const std::string &s, const char *suffix)
{
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

std::uint64_t DiskCache::hash_url(const std::string &url)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

std::string DiskCache::path_for_hash(std::uint64_t h, bool negative) const
{
    char name[40];
    std::snprintf(name, sizeof name, "/%02x/%016" PRIx64 "%s", static_cast<unsigned>(h >> 56), h,
                  negative ? ".neg" : ".art");
    return config_.root + name;
}

std::string DiskCache::path_for(const std::string &url, bool negative) const
{
    return path_for_hash(hash_url(url), negative);
}

DiskCache::DiskCache(DiskCacheConfig config)
    : config_(std::move(config)), clock_(config_.clock ? config_.clock : &system_clock())
{
}

DiskCache::~DiskCache()
{
    if (open_)
        flush();
}

bool DiskCache::open(std::string *error)
{
    Lock lock(mutex_);
    index_.clear();
    total_ = 0;
    if (config_.root.empty() || !fs::ensure_dirs(config_.root))
    {
        *error = "cannot create cache directory " + config_.root;
        return false;
    }
    // Prove the directory is writable now rather than at the first cover.
    {
        const std::string probe = config_.root + "/.probe";
        if (!fs::write_atomic(probe, "x", 1, nullptr, 0))
        {
            *error = "cache directory is not writable: " + config_.root;
            return false;
        }
        ::unlink(probe.c_str());
    }

    // 1. persisted index
    std::string text;
    if (fs::read_file(config_.root + "/index.v1", &text, 16u << 20) && text.compare(0, 8, "WINDEX1\n") == 0)
    {
        std::size_t pos = 8;
        while (pos < text.size())
        {
            std::size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            unsigned long long h = 0, size = 0, expires = 0;
            long long used = 0;
            char kind = 0;
            if (std::sscanf(line.c_str(), "%16llx %llu %lld %c %llu", &h, &size, &used, &kind, &expires) == 5 &&
                (kind == 'a' || kind == 'n'))
            {
                Item it;
                it.size = size;
                it.last_used = used;
                it.negative = kind == 'n';
                it.expires = static_cast<std::int64_t>(expires);
                index_[h] = it;
            }
        }
    }

    // 2. reconcile with the directory contents
    std::map<std::uint64_t, Item> seen;
    for (int s = 0; s < 256; ++s)
    {
        char sub[8];
        std::snprintf(sub, sizeof sub, "%02x", s);
        const std::string dir = config_.root + "/" + sub;
        std::vector<fs::DirEntry> files;
        if (!fs::list_dir(dir, &files))
            continue;
        for (const fs::DirEntry &f : files)
        {
            if (ends_with(f.name, ".tmp"))
            {
                ::unlink((dir + "/" + f.name).c_str()); // crash leftover
                continue;
            }
            const bool art = ends_with(f.name, ".art"), neg = ends_with(f.name, ".neg");
            unsigned long long h = 0;
            if ((!art && !neg) || f.name.size() != 20 || std::sscanf(f.name.c_str(), "%16llx", &h) != 1)
                continue;
            Item it;
            auto known = index_.find(h);
            it.size = f.size;
            it.last_used = known != index_.end() ? known->second.last_used : f.mtime;
            it.negative = neg;
            it.expires = known != index_.end() ? known->second.expires : 0;
            if (neg && it.expires == 0)
                it.expires = clock_->unix_seconds() + 24 * 3600; // unknown: let it age out
            seen[h] = it;
        }
    }
    bool trust_scan = !seen.empty() || index_.empty();
    if (!trust_scan)
    {
        // Listing returned nothing but the index remembers files: believe the
        // index if a few of them can still be stat()ed.
        int checked = 0, present = 0;
        for (const auto &kv : index_)
        {
            std::uint64_t sz;
            ++checked;
            if (fs::file_size(path_for_hash(kv.first, kv.second.negative), &sz))
                ++present;
            if (checked >= 8)
                break;
        }
        trust_scan = present == 0;
    }
    if (trust_scan)
        index_ = std::move(seen);
    total_ = 0;
    const std::int64_t now = clock_->unix_seconds();
    for (auto it = index_.begin(); it != index_.end();)
    {
        if (it->second.negative && it->second.expires <= now)
        {
            ::unlink(path_for_hash(it->first, true).c_str());
            it = index_.erase(it);
            continue;
        }
        total_ += it->second.size;
        ++it;
    }
    open_ = true;
    evict_locked();
    flush_locked();
    return true;
}

CacheGet DiskCache::get(const std::string &url, std::string *bytes, std::int64_t *fetched_at)
{
    const std::uint64_t h = hash_url(url);
    const std::string path = path_for_hash(h, false);
    std::string raw;
    bool good = false;
    bool existed = fs::read_file(path, &raw, config_.max_entry_bytes + url.size() + 4096);
    if (existed)
    {
        if (raw.size() >= kArtHeader && std::memcmp(raw.data(), kArtMagic, 8) == 0)
        {
            const std::uint32_t ulen = get_u32(raw.data() + 8), plen = get_u32(raw.data() + 12);
            const std::uint32_t sum = get_u32(raw.data() + 24);
            if (ulen == url.size() && raw.size() == kArtHeader + ulen + plen &&
                raw.compare(kArtHeader, ulen, url) == 0 &&
                fnv32(raw.data() + kArtHeader + ulen, plen) == sum)
            {
                if (fetched_at)
                    *fetched_at = static_cast<std::int64_t>(get_u64(raw.data() + 16));
                bytes->assign(raw, kArtHeader + ulen, plen);
                good = true;
            }
        }
    }
    Lock lock(mutex_);
    if (good)
    {
        Item &it = index_[h];
        if (it.size != raw.size())
        {
            total_ = total_ - it.size + raw.size();
            it.size = raw.size();
        }
        it.negative = false;
        it.last_used = clock_->unix_seconds();
        note_change_locked();
        ++stats_.hits;
        return CacheGet::hit;
    }
    if (existed)
    {
        // A file that is not ours, truncated, or for a colliding URL: drop it.
        // (A colliding URL's entry is simply refetched; 64-bit hashes make this rare.)
        ++stats_.corrupt;
        drop_locked(h);
    }
    ++stats_.misses;
    return CacheGet::miss;
}

bool DiskCache::put(const std::string &url, const std::uint8_t *data, std::size_t size)
{
    if (!open_ || size == 0 || size > config_.max_entry_bytes)
        return false;
    std::string head;
    head.reserve(kArtHeader + url.size());
    head.append(kArtMagic, 8);
    put_u32(&head, static_cast<std::uint32_t>(url.size()));
    put_u32(&head, static_cast<std::uint32_t>(size));
    put_u64(&head, static_cast<std::uint64_t>(clock_->unix_seconds()));
    put_u32(&head, fnv32(reinterpret_cast<const char *>(data), size));
    head += url;
    const std::uint64_t h = hash_url(url);
    const std::string path = path_for_hash(h, false);
    fs::ensure_dir(path.substr(0, path.rfind('/')));
    if (!fs::write_atomic(path, head.data(), head.size(), reinterpret_cast<const char *>(data), size))
        return false;
    Lock lock(mutex_);
    auto old = index_.find(h);
    if (old != index_.end())
    {
        total_ -= old->second.size;
        if (old->second.negative)
            ::unlink(path_for_hash(h, true).c_str());
    }
    else
        ::unlink(path_for_hash(h, true).c_str()); // a stale negative record from an older failure
    Item it;
    it.size = head.size() + size;
    it.last_used = clock_->unix_seconds();
    index_[h] = it;
    total_ += it.size;
    ++stats_.writes;
    ++writes_since_evict_;
    if (total_ > config_.budget_bytes || writes_since_evict_ >= config_.evict_every_writes)
        evict_locked();
    note_change_locked();
    return true;
}

void DiskCache::remove(const std::string &url)
{
    Lock lock(mutex_);
    drop_locked(hash_url(url));
}

bool DiskCache::contains(const std::string &url)
{
    Lock lock(mutex_);
    auto it = index_.find(hash_url(url));
    return it != index_.end() && !it->second.negative;
}

bool DiskCache::put_negative(const std::string &url, const std::string &reason, std::int64_t ttl_seconds)
{
    if (!open_)
        return false;
    const std::int64_t expires = clock_->unix_seconds() + ttl_seconds;
    std::string rec;
    rec.append(kNegMagic, 8);
    put_u32(&rec, static_cast<std::uint32_t>(url.size()));
    put_u32(&rec, static_cast<std::uint32_t>(reason.size()));
    put_u64(&rec, static_cast<std::uint64_t>(expires));
    rec += url;
    rec += reason;
    const std::uint64_t h = hash_url(url);
    const std::string path = path_for_hash(h, true);
    fs::ensure_dir(path.substr(0, path.rfind('/')));
    if (!fs::write_atomic(path, rec.data(), rec.size(), nullptr, 0))
        return false;
    Lock lock(mutex_);
    auto old = index_.find(h);
    if (old != index_.end())
    {
        total_ -= old->second.size;
        if (!old->second.negative)
            ::unlink(path_for_hash(h, false).c_str()); // the picture is gone for good
    }
    Item it;
    it.size = rec.size();
    it.last_used = clock_->unix_seconds();
    it.negative = true;
    it.expires = expires;
    index_[h] = it;
    total_ += it.size;
    note_change_locked();
    return true;
}

bool DiskCache::get_negative(const std::string &url, std::string *reason)
{
    const std::uint64_t h = hash_url(url);
    const std::int64_t now = clock_->unix_seconds();
    {
        Lock lock(mutex_);
        auto it = index_.find(h);
        // Fast path: nothing recorded (the common case) costs no file access.
        if (it == index_.end() || !it->second.negative)
            return false;
        if (it->second.expires <= now)
        {
            drop_locked(h);
            return false;
        }
    }
    std::string raw;
    if (!fs::read_file(path_for_hash(h, true), &raw, 1u << 16) || raw.size() < 24 ||
        std::memcmp(raw.data(), kNegMagic, 8) != 0)
    {
        Lock lock(mutex_);
        drop_locked(h);
        return false;
    }
    const std::uint32_t ulen = get_u32(raw.data() + 8), rlen = get_u32(raw.data() + 12);
    if (raw.size() != 24ull + ulen + rlen || raw.compare(24, ulen, url) != 0)
        return false; // a colliding URL's record
    if (static_cast<std::int64_t>(get_u64(raw.data() + 16)) <= now)
    {
        Lock lock(mutex_);
        drop_locked(h);
        return false;
    }
    if (reason)
        reason->assign(raw, 24 + ulen, rlen);
    return true;
}

void DiskCache::clear_negatives()
{
    Lock lock(mutex_);
    for (auto it = index_.begin(); it != index_.end();)
    {
        if (it->second.negative)
        {
            ::unlink(path_for_hash(it->first, true).c_str());
            total_ -= it->second.size;
            it = index_.erase(it);
        }
        else
            ++it;
    }
    note_change_locked();
}

void DiskCache::drop_locked(std::uint64_t h)
{
    auto it = index_.find(h);
    if (it != index_.end())
    {
        ::unlink(path_for_hash(h, it->second.negative).c_str());
        total_ -= it->second.size;
        index_.erase(it);
        note_change_locked();
    }
    else
    {
        ::unlink(path_for_hash(h, false).c_str());
        ::unlink(path_for_hash(h, true).c_str());
    }
}

void DiskCache::evict_locked()
{
    writes_since_evict_ = 0;
    if (total_ <= config_.budget_bytes)
        return;
    // Evict to slightly under the budget so the next write does not evict again.
    const std::uint64_t target = config_.budget_bytes - config_.budget_bytes / 32;
    std::vector<std::pair<std::int64_t, std::uint64_t>> order; // last_used, hash
    order.reserve(index_.size());
    for (const auto &kv : index_)
        order.emplace_back(kv.second.last_used, kv.first);
    std::sort(order.begin(), order.end());
    for (const auto &o : order)
    {
        if (total_ <= target)
            break;
        auto it = index_.find(o.second);
        if (it == index_.end())
            continue;
        ::unlink(path_for_hash(o.second, it->second.negative).c_str());
        total_ -= it->second.size;
        index_.erase(it);
        ++stats_.evicted;
    }
    note_change_locked();
}

void DiskCache::evict()
{
    Lock lock(mutex_);
    evict_locked();
}

void DiskCache::note_change_locked()
{
    if (++changes_since_flush_ >= config_.index_flush_every)
        flush_locked();
}

void DiskCache::flush_locked()
{
    changes_since_flush_ = 0;
    if (!open_)
        return;
    std::string text = "WINDEX1\n";
    char line[96];
    for (const auto &kv : index_)
    {
        std::snprintf(line, sizeof line, "%016" PRIx64 " %" PRIu64 " %lld %c %lld\n", kv.first,
                      kv.second.size, static_cast<long long>(kv.second.last_used),
                      kv.second.negative ? 'n' : 'a', static_cast<long long>(kv.second.expires));
        text += line;
    }
    fs::write_atomic(config_.root + "/index.v1", text.data(), text.size(), nullptr, 0);
}

void DiskCache::flush()
{
    Lock lock(mutex_);
    flush_locked();
}

void DiskCache::clear()
{
    Lock lock(mutex_);
    for (const auto &kv : index_)
        ::unlink(path_for_hash(kv.first, kv.second.negative).c_str());
    index_.clear();
    total_ = 0;
    flush_locked();
}

DiskCache::Stats DiskCache::stats()
{
    Lock lock(mutex_);
    Stats s = stats_;
    s.bytes = total_;
    for (const auto &kv : index_)
        (kv.second.negative ? s.negatives : s.entries)++;
    return s;
}

} // namespace wings::art
