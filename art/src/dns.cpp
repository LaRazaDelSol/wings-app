// Wings cover-art module: TTL cache and (optional) console resolver.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/dns.hpp"

#include <algorithm>

namespace wings::art
{

CachingDns::CachingDns(std::shared_ptr<DnsProvider> inner, const Clock *clock)
    : inner_(std::move(inner)), clock_(clock ? clock : &system_clock())
{
}

bool CachingDns::resolve(const std::string &host, DnsResult *out)
{
    auto lookup = [&]() -> int // 1 hit ok, 2 hit negative, 0 miss
    {
        Lock lock(mutex_);
        auto it = cache_.find(host);
        if (it == cache_.end())
            return 0;
        if (clock_->mono_ms() >= it->second.expires_ms)
        {
            cache_.erase(it);
            return 0;
        }
        *out = it->second.result;
        return it->second.result.nxdomain ? 2 : 1;
    };
    if (int hit = lookup())
        return hit == 1;

    // One lookup per new name at a time; whoever waited re-checks the cache.
    Lock miss(miss_mutex_);
    if (int hit = lookup())
        return hit == 1;

    DnsResult fresh;
    const bool ok = inner_->resolve(host, &fresh);
    {
        Lock lock(mutex_);
        ++lookups_;
        int ttl = 0;
        if (ok && !fresh.ipv4.empty())
            ttl = std::min(std::max(fresh.ttl_seconds, 30), 3600);
        else if (fresh.nxdomain)
            ttl = 60; // a definite "no such name" is remembered briefly
        if (ttl > 0)
        {
            if (cache_.size() >= 512)
                cache_.clear();
            Entry e;
            e.result = fresh;
            e.expires_ms = clock_->mono_ms() + static_cast<std::int64_t>(ttl) * 1000;
            cache_[host] = std::move(e);
        }
    }
    *out = std::move(fresh);
    return ok;
}

void CachingDns::clear()
{
    Lock lock(mutex_);
    cache_.clear();
}

std::size_t CachingDns::size()
{
    Lock lock(mutex_);
    return cache_.size();
}

#ifdef WINGS_ART_SCENET
// Prototypes written from the public libSceNet API (the SDK ships only import
// stubs, no header). Return values: 0 on success, a negative SCE error else.
extern "C"
{
    int sceNetInit(void);
    int sceNetPoolCreate(const char *name, int size, int flags);
    int sceNetPoolDestroy(int memid);
    int sceNetResolverCreate(const char *name, int memid, int flags);
    int sceNetResolverStartNtoa(int rid, const char *hostname, std::uint32_t *addr, int timeout,
                                int retry, int flags);
    int sceNetResolverDestroy(int rid);
}

SystemDns::SystemDns()
{
    sceNetInit(); // already initialised by someone else is fine
    inited_ = true;
    pool_ = sceNetPoolCreate("wings-art", 64 * 1024, 0);
}

SystemDns::~SystemDns()
{
    if (pool_ >= 0)
        sceNetPoolDestroy(pool_);
}

bool SystemDns::resolve(const std::string &host, DnsResult *out)
{
    if (pool_ < 0)
    {
        out->error = "sceNetPoolCreate failed (" + std::to_string(pool_) + ")";
        return false;
    }
    Lock lock(mutex_);
    const int rid = sceNetResolverCreate("wings-art", pool_, 0);
    if (rid < 0)
    {
        out->error = "sceNetResolverCreate failed (" + std::to_string(rid) + ")";
        return false;
    }
    std::uint32_t addr = 0;
    const int rc = sceNetResolverStartNtoa(rid, host.c_str(), &addr, 0, 0, 0);
    sceNetResolverDestroy(rid);
    if (rc < 0 || addr == 0)
    {
        out->error = "sceNetResolverStartNtoa failed (" + std::to_string(rc) + ")";
        return false;
    }
    out->ipv4.assign(1, addr);
    out->ttl_seconds = 300;
    return true;
}
#endif

} // namespace wings::art
