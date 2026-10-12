// Wings cover-art module: name resolution interface and TTL cache.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// getaddrinfo is NOT usable from a native console title (the SDK exports it
// only from libScePosixForWebKit, which a title does not load; see
// docs/tls-art.md finding 2), so the module never calls it. A DnsProvider
// turns a host name into IPv4 addresses:
//
//   * SystemDns   console resolver (sceNetResolver*), compiled in only with
//                 -DWINGS_ART_SCENET; UNVERIFIED on hardware, see README.
//   * DohProvider DNS-over-HTTPS to an IP-literal endpoint; opt-in (privacy).
//   * CachingDns  wraps any provider with a TTL cache.
#pragma once

#include "wings/art/platform.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wings::art
{

struct DnsResult
{
    std::vector<std::uint32_t> ipv4; // network byte order, as in in_addr
    int ttl_seconds = 300;
    bool nxdomain = false; // the name does not exist (authoritative)
    std::string error;     // filled on failure
};

class DnsProvider
{
  public:
    virtual ~DnsProvider() = default;
    // Blocking; may be called from several workers at once. Returns false on
    // failure with out->error set (out->nxdomain for a definite "no such name").
    virtual bool resolve(const std::string &host, DnsResult *out) = 0;
    virtual const char *name() const = 0;
};

// TTL cache (clamped to 30 s .. 1 h; NXDOMAIN is remembered for 60 s) in front
// of another provider. Misses are serialised so N workers asking for the same
// new CDN host cause one lookup.
class CachingDns final : public DnsProvider
{
  public:
    CachingDns(std::shared_ptr<DnsProvider> inner, const Clock *clock = nullptr);
    bool resolve(const std::string &host, DnsResult *out) override;
    const char *name() const override { return inner_->name(); }
    void clear();
    std::size_t size();
    int lookups() const { return lookups_; } // inner lookups, for tests/stats

  private:
    struct Entry
    {
        DnsResult result;
        std::int64_t expires_ms = 0;
    };
    std::shared_ptr<DnsProvider> inner_;
    const Clock *clock_;
    Mutex mutex_, miss_mutex_;
    std::map<std::string, Entry> cache_;
    int lookups_ = 0;
};

#ifdef WINGS_ART_SCENET
// Console resolver. sceNetInit + a pool are created once; each lookup uses a
// short-lived resolver id. UNVERIFIED from a native title: needs libSceNet in
// the eboot's imports (add libSceNet.so to APP_IMPORT_STUBS) and the on-console
// probe in wings/art/probe/ before it may become the default.
class SystemDns final : public DnsProvider
{
  public:
    SystemDns();
    ~SystemDns() override;
    bool resolve(const std::string &host, DnsResult *out) override;
    const char *name() const override { return "system"; }
    bool ready() const { return pool_ >= 0; }

  private:
    Mutex mutex_;
    int pool_ = -1;
    bool inited_ = false;
};
#endif

constexpr bool system_dns_compiled()
{
#ifdef WINGS_ART_SCENET
    return true;
#else
    return false;
#endif
}

} // namespace wings::art
