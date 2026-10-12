// Wings prototype: minimal HTTPS GET-to-memory client (OpenSSL + BSD sockets).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deliberately uses only what the app build can already link: libssl.a and
// libcrypto.a (PacBrew OpenSSL 3.5.2) plus the C library (getaddrinfo, poll,
// sockets). No libcurl, no std::thread requirement inside the client itself:
// a TlsClient instance is single-threaded (one per worker); the shared parts
// (SSL_CTX, DNS cache) are thread-safe.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wings::tls
{

enum class Error : std::uint8_t
{
    none,
    bad_url,       // not https://, credentials in URL, too long, bad host
    dns,           // getaddrinfo failed
    connect,       // TCP connect failed / timed out
    handshake,     // TLS handshake or certificate verification failed
    io,            // read/write error or timeout
    http_status,   // non-2xx final status (see Response::status)
    redirect,      // too many redirects, bad Location, or downgrade to http
    too_large,     // Content-Length or body exceeds the cap
    bad_type,      // Content-Type not in the image allow-list
    protocol,      // malformed HTTP response
};

const char *name(Error e);

struct Limits
{
    std::size_t max_body = 4u << 20;     // bytes accepted per image
    std::size_t max_header = 16u << 10;  // bytes of response headers
    int max_redirects = 3;
    int connect_timeout_ms = 8000;
    int io_timeout_ms = 15000;           // per read/write (SO_RCVTIMEO)
    int total_timeout_ms = 30000;        // per request, all redirects
    int idle_keepalive_ms = 20000;       // drop pooled conns idle longer
    int max_idle_conns = 6;              // per client
    bool allow_private = false;          // true only for loopback test fixtures
};

struct Timing
{
    double dns_ms = 0, connect_ms = 0, tls_ms = 0, first_byte_ms = 0, total_ms = 0;
    bool reused = false;        // keep-alive connection reused
    bool resumed = false;       // TLS session resumed
    bool dns_cached = false;
    int redirects = 0;
};

struct Response
{
    Error error = Error::none;
    std::string detail;         // human readable reason
    int status = 0;
    std::string content_type;   // lower-case, no parameters
    std::string final_url;
    std::vector<std::uint8_t> body;
    Timing timing;
    bool ok() const { return error == Error::none; }
};

// Pluggable name resolution. Fills IPv4 addresses (network byte order) and a
// TTL in seconds. Called with the Shared lock NOT held, possibly from several
// workers at once (the callee serialises if it needs to). When none is set the
// client uses getaddrinfo (host tests only: on the console that symbol lives in
// libScePosixForWebKit, which a native title does not load; see docs/tls-art.md).
using ResolveFn = bool (*)(void *ctx, const std::string &host, std::vector<std::uint32_t> *ipv4,
                           int *ttl_seconds, std::string *why);

struct GetOptions
{
    const char *accept = nullptr;  // Accept header; default asks for images
    bool any_content_type = false; // skip the image/* allow-list (DoH uses this)
};

// Shared, thread-safe. Holds the SSL_CTX (trust store, session cache) and the
// DNS cache. pem is the whole CA bundle in memory (e.g. an embedded blob or a
// file the app ships); an empty pem aborts: never fall back to "no verify".
class Shared
{
  public:
    static std::shared_ptr<Shared> create(const std::string &ca_bundle_pem, std::string *error);
    ~Shared();
    void set_resolver(ResolveFn fn, void *ctx);
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

    // Blocking. Follows redirects (https only), reuses a pooled connection to
    // the same host:port when one is idle. Never sends cookies or credentials.
    Response get(const std::string &url, const GetOptions &options = {});

    // Drop pooled connections (e.g. on app suspend).
    void close_all();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wings::tls
