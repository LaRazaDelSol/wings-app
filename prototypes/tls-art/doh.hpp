// Wings prototype: DNS-over-HTTPS resolver for hosts the app must reach without
// getaddrinfo (not usable in a native title) and without trusting the console's
// DNS (which many jailbroken consoles point at a PSN-blocking resolver).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Queries an A record with the RFC 8484 GET form against an IP-literal
// endpoint (certificate verified against the IP SAN), so no name resolution is
// needed to bootstrap. Same trick Pegasus uses ("https://1.1.1.1/dns-query").
#pragma once

#include "tls_client.hpp"

#include <pthread.h>

namespace wings::tls
{

class DohResolver
{
  public:
    // endpoints: tried in order, e.g. {"https://1.1.1.1/dns-query", "https://8.8.8.8/dns-query"}
    DohResolver(std::shared_ptr<Shared> shared, std::vector<std::string> endpoints);
    ~DohResolver();

    // Installs itself as shared's resolver.
    void install();

    // ResolveFn-compatible entry point (ctx = DohResolver*).
    static bool resolve_fn(void *ctx, const std::string &host, std::vector<std::uint32_t> *ipv4,
                           int *ttl_seconds, std::string *why);

    bool resolve(const std::string &host, std::vector<std::uint32_t> *ipv4, int *ttl_seconds,
                 std::string *why);

    int lookups() const { return lookups_; }

    // Exposed for tests.
    static std::vector<std::uint8_t> build_query(const std::string &host);
    static bool parse_response(const std::uint8_t *data, std::size_t size,
                               std::vector<std::uint32_t> *ipv4, int *ttl_seconds,
                               std::string *why);

  private:
    std::shared_ptr<Shared> shared_;
    std::vector<std::string> endpoints_;
    Client client_;
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER; // one lookup at a time
    int lookups_ = 0;
};

} // namespace wings::tls
