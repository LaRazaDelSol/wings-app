// Wings cover-art module: minimal HTTPS GET-to-memory client (OpenSSL + BSD sockets).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Uses only what the app build can link: libssl.a and libcrypto.a (PacBrew
// OpenSSL 3.5.2) plus the C library. No libcurl, no getaddrinfo. A Client is
// single-threaded (one per worker); Shared (SSL_CTX, trust store, session
// cache, DNS provider) is thread-safe.
//
// Safety properties (all covered by tests/):
//   * https only, also across redirects (at most 3); no credentials in URLs,
//     no IPv6 or ambiguous numeric hosts; no cookies/Authorization/Referer.
//   * certificate chain and host name (or IP SAN) always verified, TLS >= 1.2;
//     there is no insecure mode and an empty CA bundle is refused.
//   * resolved addresses in loopback / RFC 1918 / link-local / CGNAT /
//     multicast / reserved space are refused (DNS rebinding, Pegasus on
//     127.0.0.1:6970, the LAN).
//   * body cap enforced from Content-Length and while reading (also chunked);
//     identity encoding only; Content-Type must be image/* (not svg), octet
//     stream or empty.
//   * every wait is sliced, so a cancel request or the total timeout is honoured
//     within ~250 ms.
#pragma once

#include "wings/art/dns.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wings::art::tls
{

enum class Error : std::uint8_t
{
    none,
    bad_url,     // not https://, credentials, too long, bad/ambiguous host
    dns,         // name resolution failed (see Response::nxdomain)
    blocked,     // resolved only to non-public addresses (or localhost)
    connect,     // TCP connect failed / timed out
    handshake,   // TLS handshake or certificate verification failed
    io,          // read/write error or timeout
    http_status, // non-2xx final status (Response::status)
    redirect,    // too many redirects, bad Location, downgrade to http
    too_large,   // Content-Length or body exceeds the cap
    bad_type,    // Content-Type not in the image allow-list
    protocol,    // malformed HTTP response
    cancelled,   // GetOptions::cancelled returned true
};

const char *name(Error e);

struct Limits
{
    std::size_t max_body = 2u << 20;   // bytes accepted per response
    std::size_t max_header = 16u << 10;
    int max_redirects = 3;
    int connect_timeout_ms = 8000;
    int io_timeout_ms = 15000;         // without any progress
    int total_timeout_ms = 30000;      // per request, all redirects
    int idle_keepalive_ms = 20000;     // drop pooled connections idle longer
    int max_idle_conns = 6;            // per client
    bool allow_private = false;        // TESTS ONLY: loopback fixtures
};

struct Timing
{
    double dns_ms = 0, connect_ms = 0, tls_ms = 0, first_byte_ms = 0, total_ms = 0;
    bool reused = false;  // keep-alive connection reused
    bool resumed = false; // TLS session resumed
    bool dns_cached = false;
    int redirects = 0;
};

struct Response
{
    Error error = Error::none;
    std::string detail;
    int status = 0;
    int retry_after_s = -1;     // Retry-After (seconds) of a 429/503, else -1
    bool nxdomain = false;      // error == dns and the name does not exist
    long verify_error = 0;      // X509 verify code when the handshake failed
    bool time_error = false;    // certificate "not yet valid" / "expired": maybe the console clock
    std::string content_type;   // lower-case, no parameters
    std::string final_url;
    std::vector<std::uint8_t> body;
    Timing timing;
    bool ok() const { return error == Error::none; }
};

struct GetOptions
{
    const char *accept = nullptr;   // default asks for images
    bool any_content_type = false;  // skip the image allow-list (DoH uses this)
    std::size_t max_body = 0;       // 0: Limits::max_body
    bool (*cancelled)(void *) = nullptr; // polled between waits
    void *cancel_ctx = nullptr;
    // Optional. Called with every redirect target before it is followed; false refuses it
    // (Error::redirect). Lets a caller pin the hosts a download may be served from.
    bool (*redirect_ok)(void *, const std::string &url) = nullptr;
    // Optional. Called with the body bytes received so far (also for small redirect bodies).
    void (*progress)(void *, std::uint64_t bytes) = nullptr;
    void *hook_ctx = nullptr;
};

class Shared
{
  public:
    // pem: the whole CA bundle. Fails (nullptr, *info says why) when it holds no
    // certificate or when OpenSSL has no entropy (RAND_status() != 1). On
    // success *info = "<n> CA certificates".
    static std::shared_ptr<Shared> create(const std::string &ca_bundle_pem, std::string *info);
    ~Shared();
    // Replaces the name resolver (nullptr: every host name fails to resolve).
    void set_dns(std::shared_ptr<DnsProvider> dns);
    std::shared_ptr<DnsProvider> dns() const;
    int ca_count() const;
    struct Impl;
    Impl *impl() { return impl_.get(); }

  private:
    Shared();
    std::unique_ptr<Impl> impl_;
};

class Client
{
  public:
    Client(std::shared_ptr<Shared> shared, Limits limits = {});
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Blocking. Follows https redirects, reuses an idle connection to the same
    // host:port, retries once on a stale pooled connection.
    Response get(const std::string &url, const GetOptions &options = {});
    void close_all();
    int idle_connections() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// URL helpers shared with the service (exposed for tests).
struct ParsedUrl
{
    std::string host, target;
    int port = 443;
};
bool parse_https_url(const std::string &url, ParsedUrl *out, std::string *why);
bool is_public_ipv4(std::uint32_t host_order_ip);

} // namespace wings::art::tls
