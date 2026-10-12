// Wings cover-art module: DNS-over-HTTPS provider (RFC 8484 GET, A records only).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// OPT-IN: host names of the art CDNs are sent to the resolver operator
// (Cloudflare / Google by default). Endpoints must use an IPv4 literal host so
// no name resolution is needed to bootstrap (certificate checked against the
// IP SAN); anything else is ignored.
#pragma once

#include "wings/art/dns.hpp"
#include "wings/art/tls_client.hpp"

#include <vector>

namespace wings::art
{

class DohProvider final : public DnsProvider
{
  public:
    // endpoints: tried in order, e.g. {"https://1.1.1.1/dns-query", "https://8.8.8.8/dns-query"}.
    // `limits` is for tests (loopback fixtures); production uses the defaults.
    DohProvider(std::shared_ptr<tls::Shared> shared, std::vector<std::string> endpoints,
                tls::Limits limits = default_limits());

    bool resolve(const std::string &host, DnsResult *out) override;
    const char *name() const override { return "doh"; }

    // Consecutive lookups for which every endpoint failed (0 after any success).
    int failed_streak() const { return failed_streak_; }
    int lookups() const { return lookups_; }
    // Endpoints dropped because they were not https://<IPv4 literal>/...
    const std::vector<std::string> &rejected_endpoints() const { return rejected_; }

    static tls::Limits default_limits();
    static std::vector<std::uint8_t> build_query(const std::string &host);
    static bool parse_response(const std::uint8_t *data, std::size_t size, DnsResult *out);

  private:
    std::vector<std::string> endpoints_, rejected_;
    tls::Client client_;
    Mutex mutex_; // one lookup at a time (the shared connection is single-threaded)
    std::size_t preferred_ = 0;
    int failed_streak_ = 0;
    int lookups_ = 0;
};

} // namespace wings::art
