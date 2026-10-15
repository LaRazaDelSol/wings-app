// Wings cover-art module: minimal HTTPS GET-to-memory client (OpenSSL + BSD sockets).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/tls_client.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <utility>

namespace wings::art::tls
{

const char *name(Error e)
{
    switch (e)
    {
    case Error::none: return "none";
    case Error::bad_url: return "bad_url";
    case Error::dns: return "dns";
    case Error::blocked: return "blocked";
    case Error::connect: return "connect";
    case Error::handshake: return "handshake";
    case Error::io: return "io";
    case Error::http_status: return "http_status";
    case Error::redirect: return "redirect";
    case Error::too_large: return "too_large";
    case Error::bad_type: return "bad_type";
    case Error::protocol: return "protocol";
    case Error::cancelled: return "cancelled";
    case Error::sink: return "sink";
    }
    return "?";
}

namespace
{

using SteadyClock = std::chrono::steady_clock;

double ms_since(SteadyClock::time_point t)
{
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - t).count();
}

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Percent-encodes bytes a request target must not contain (spaces and
// non-ASCII in catalog URLs are common), leaving existing %XX escapes alone.
std::string escape_target(const std::string &in)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '%' && i + 2 < in.size() + 0 && is_hex(in[i + 1]) && is_hex(in[i + 2]))
        {
            out.push_back('%');
            continue;
        }
        if (c <= 0x20 || c >= 0x7f || std::strchr("\"<>\\^`{|}", c))
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
        else
            out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string key_of(const ParsedUrl &u)
{
    return u.host + ":" + std::to_string(u.port);
}

bool is_ip_literal(const std::string &host)
{
    in_addr a{};
    return inet_pton(AF_INET, host.c_str(), &a) == 1;
}

// Resolves a Location header against the URL it came from.
std::string resolve_location(const ParsedUrl &base, const std::string &loc)
{
    if (loc.compare(0, 8, "https://") == 0 || loc.compare(0, 7, "http://") == 0)
        return loc;
    const std::string origin =
        "https://" + base.host + (base.port == 443 ? "" : ":" + std::to_string(base.port));
    if (loc.compare(0, 2, "//") == 0)
        return "https:" + loc;
    if (!loc.empty() && loc[0] == '/')
        return origin + loc;
    // Schemes other than http(s) ("ftp:", "data:", "javascript:") must not be
    // treated as relative paths.
    const std::size_t colon = loc.find(':');
    const std::size_t slash = loc.find('/');
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
        return loc;
    std::string dir = base.target.substr(0, base.target.find('?'));
    dir = dir.substr(0, dir.rfind('/') + 1);
    return origin + dir + loc;
}

} // namespace

bool parse_https_url(const std::string &url, ParsedUrl *out, std::string *why)
{
    if (url.size() > 2048)
        return *why = "url too long", false;
    if (lower(url.substr(0, 8)) != "https://")
        return *why = "only https:// is allowed", false;
    for (unsigned char c : url)
        if (c < 0x20 || c == 0x7f)
            return *why = "control character in url", false;
    std::string rest = url.substr(8);
    const std::size_t hash = rest.find('#');
    if (hash != std::string::npos)
        rest.resize(hash);
    const std::size_t slash = rest.find_first_of("/?");
    std::string auth = rest.substr(0, slash);
    std::string target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (target[0] == '?')
        target = "/" + target;
    if (auth.find('@') != std::string::npos)
        return *why = "credentials in URL refused", false;
    if (auth.empty() || auth[0] == '[')
        return *why = "empty host or IPv6 literal", false;
    int port = 443;
    const std::size_t colon = auth.rfind(':');
    if (colon != std::string::npos)
    {
        const std::string p = auth.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos)
            return *why = "bad port", false;
        port = std::atoi(p.c_str());
        if (port < 1 || port > 65535)
            return *why = "bad port", false;
        auth.resize(colon);
    }
    if (auth.empty() || auth.size() > 253)
        return *why = "bad host", false;
    for (unsigned char c : auth)
        if (!(std::isalnum(c) || c == '-' || c == '.' || c == '_'))
            return *why = "bad host character", false;
    const std::string host = lower(auth);
    if (host.find("..") != std::string::npos || host[0] == '.')
        return *why = "bad host", false;
    // Numeric hosts must be a canonical dotted quad. "2130706433", "0x7f.1" and
    // "127.1" are accepted by some resolvers as 127.0.0.1.
    const bool numeric = host.find_first_not_of("0123456789.") == std::string::npos;
    if ((numeric && !is_ip_literal(host)) || host.compare(0, 2, "0x") == 0)
        return *why = "ambiguous numeric host refused", false;
    out->host = host;
    out->port = port;
    out->target = escape_target(target);
    return true;
}

// RFC 1918, loopback, link-local, CGNAT, TEST-NETs, benchmarking, multicast
// and reserved space are refused.
bool is_public_ipv4(std::uint32_t ip)
{
    const unsigned a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
    if (a == 0 || a == 10 || a == 127 || a >= 224)
        return false;
    if (a == 172 && b >= 16 && b <= 31)
        return false;
    if (a == 192 && b == 168)
        return false;
    if (a == 169 && b == 254)
        return false;
    if (a == 100 && b >= 64 && b <= 127)
        return false;
    if (a == 192 && b == 0 && (c == 0 || c == 2))
        return false;
    if (a == 198 && (b == 18 || b == 19))
        return false;
    if (a == 198 && b == 51 && c == 100)
        return false;
    if (a == 203 && b == 0 && c == 113)
        return false;
    return true;
}

// ---------------------------------------------------------------- Shared

struct Shared::Impl
{
    SSL_CTX *ctx = nullptr;
    int host_index = -1; // SSL ex_data slot holding the session-cache key
    int ca_count = 0;
    Mutex mutex;
    std::map<std::string, SSL_SESSION *> sessions;
    std::shared_ptr<DnsProvider> dns;
};

namespace
{
int on_new_session(SSL *ssl, SSL_SESSION *session)
{
    auto *impl = static_cast<Shared::Impl *>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
    auto *key = static_cast<std::string *>(SSL_get_ex_data(ssl, impl->host_index));
    if (!key)
        return 0;
    Lock lock(impl->mutex);
    if (impl->sessions.size() >= 64 && !impl->sessions.count(*key))
    {
        for (auto &kv : impl->sessions)
            SSL_SESSION_free(kv.second);
        impl->sessions.clear();
    }
    auto &slot = impl->sessions[*key];
    if (slot)
        SSL_SESSION_free(slot);
    slot = session; // we keep the reference
    return 1;
}

void free_key(void *, void *ptr, CRYPTO_EX_DATA *, int, long, void *)
{
    delete static_cast<std::string *>(ptr);
}
} // namespace

Shared::Shared() : impl_(new Impl) {}

Shared::~Shared()
{
    if (impl_->ctx)
        SSL_CTX_free(impl_->ctx);
    for (auto &kv : impl_->sessions)
        SSL_SESSION_free(kv.second);
}

void Shared::set_dns(std::shared_ptr<DnsProvider> dns)
{
    Lock lock(impl_->mutex);
    impl_->dns = std::move(dns);
}

std::shared_ptr<DnsProvider> Shared::dns() const
{
    Lock lock(impl_->mutex);
    return impl_->dns;
}

int Shared::ca_count() const
{
    return impl_->ca_count;
}

std::shared_ptr<Shared> Shared::create(const std::string &pem, std::string *info)
{
#ifndef SO_NOSIGPIPE
    // OpenSSL writes with write(); without SO_NOSIGPIPE a closed peer would
    // raise SIGPIPE and kill the process.
    signal(SIGPIPE, SIG_IGN);
#endif
    std::shared_ptr<Shared> s(new Shared);
    s->impl_->ctx = SSL_CTX_new(TLS_client_method());
    if (!s->impl_->ctx)
        return *info = "SSL_CTX_new failed", nullptr;
    // A console build must prove its entropy source works before any
    // handshake: a silently unseeded generator makes session keys guessable.
    if (RAND_status() != 1)
        return *info = "OpenSSL has no entropy source on this system", nullptr;
    SSL_CTX *ctx = s->impl_->ctx;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_IGNORE_UNEXPECTED_EOF);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_set_app_data(ctx, s->impl_.get());
    SSL_CTX_sess_set_new_cb(ctx, on_new_session);
    s->impl_->host_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, free_key);

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    BIO *bio = pem.empty() ? nullptr : BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    int added = 0;
    while (bio)
    {
        X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!cert)
            break;
        if (X509_STORE_add_cert(store, cert) == 1)
            ++added;
        X509_free(cert);
    }
    if (bio)
        BIO_free(bio);
    ERR_clear_error();
    if (added == 0)
        return *info = "no CA certificates loaded; refusing to run without verification", nullptr;
    s->impl_->ca_count = added;
    *info = std::to_string(added) + " CA certificates";
    return s;
}

// ---------------------------------------------------------------- Client

struct Client::Impl
{
    std::shared_ptr<Shared> shared;
    Limits limits;

    struct Conn
    {
        int fd = -1;
        SSL *ssl = nullptr;
        std::string key;
        SteadyClock::time_point last_used;
    };
    std::deque<Conn> idle; // most recently used at the front

    // Per-request state (a Client is single-threaded).
    const GetOptions *opt = nullptr;
    SteadyClock::time_point started;
    Error stop = Error::none; // set when a wait gave up

    void destroy(Conn &c)
    {
        if (c.ssl)
        {
            SSL_set_quiet_shutdown(c.ssl, 1); // no close_notify round trip
            SSL_free(c.ssl);
        }
        if (c.fd >= 0)
            ::close(c.fd);
        c = Conn();
    }

    void trim_idle()
    {
        while (!idle.empty() && (static_cast<int>(idle.size()) > limits.max_idle_conns ||
                                 ms_since(idle.back().last_used) > limits.idle_keepalive_ms))
        {
            destroy(idle.back());
            idle.pop_back();
        }
    }

    bool take_idle(const std::string &key, Conn *out)
    {
        trim_idle();
        for (auto it = idle.begin(); it != idle.end(); ++it)
            if (it->key == key)
            {
                *out = *it;
                idle.erase(it);
                return true;
            }
        return false;
    }

    void park(Conn &c)
    {
        c.last_used = SteadyClock::now();
        idle.push_front(c);
        c = Conn();
        trim_idle();
    }

    // ---- waiting: every wait is sliced so cancel / total timeout are prompt
    bool aborted(std::string *why)
    {
        if (opt && opt->cancelled && opt->cancelled(opt->cancel_ctx))
        {
            stop = Error::cancelled;
            *why = "cancelled";
            return true;
        }
        if (ms_since(started) > limits.total_timeout_ms)
        {
            stop = Error::io;
            *why = "total timeout";
            return true;
        }
        return false;
    }

    bool wait_fd(int fd, bool for_write, SteadyClock::time_point since, int limit_ms,
                 std::string *why)
    {
        for (;;)
        {
            if (aborted(why))
                return false;
            if (ms_since(since) > limit_ms)
            {
                stop = Error::io;
                *why = for_write ? "write timeout" : "read timeout";
                return false;
            }
            pollfd p{fd, static_cast<short>(for_write ? POLLOUT : POLLIN), 0};
            const int r = ::poll(&p, 1, 250);
            if (r > 0)
                return true;
            if (r < 0 && errno != EINTR)
            {
                stop = Error::io;
                *why = "poll failed";
                return false;
            }
        }
    }

    // >0 bytes, 0 end of stream, -1 failure (stop/why set)
    int io_read(SSL *ssl, int fd, void *buf, int n, std::string *why)
    {
        const auto since = SteadyClock::now();
        for (;;)
        {
            ERR_clear_error();
            const int r = SSL_read(ssl, buf, n);
            if (r > 0)
                return r;
            const int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN)
                return 0;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            {
                if (!wait_fd(fd, e == SSL_ERROR_WANT_WRITE, since, limits.io_timeout_ms, why))
                    return -1;
                continue;
            }
            if (e == SSL_ERROR_SYSCALL && r == 0)
                return 0;
            ERR_clear_error();
            stop = Error::io;
            *why = "TLS read error";
            return -1;
        }
    }

    bool io_write(SSL *ssl, int fd, const char *p, std::size_t n, std::string *why)
    {
        const auto since = SteadyClock::now();
        while (n)
        {
            ERR_clear_error();
            const int w = SSL_write(ssl, p, static_cast<int>(std::min<std::size_t>(n, 1 << 16)));
            if (w > 0)
            {
                p += w;
                n -= static_cast<std::size_t>(w);
                continue;
            }
            const int e = SSL_get_error(ssl, w);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            {
                if (!wait_fd(fd, e == SSL_ERROR_WANT_WRITE, since, limits.io_timeout_ms, why))
                    return false;
                continue;
            }
            ERR_clear_error();
            stop = Error::io;
            *why = "TLS write error";
            return false;
        }
        return true;
    }

    // ---- DNS
    Error resolve(const ParsedUrl &u, std::vector<sockaddr_in> *addrs, bool *cached,
                  bool *nxdomain, std::string *why)
    {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(static_cast<std::uint16_t>(u.port));
        in_addr literal{};
        if (inet_pton(AF_INET, u.host.c_str(), &literal) == 1)
        {
            sin.sin_addr = literal;
            addrs->assign(1, sin);
            *cached = true;
            return Error::none;
        }
        std::shared_ptr<DnsProvider> dns = shared->dns();
        if (!dns)
            return *why = "no DNS provider configured", Error::dns;
        DnsResult r;
        if (!dns->resolve(u.host, &r))
        {
            *nxdomain = r.nxdomain;
            *why = r.error.empty() ? "name resolution failed" : r.error;
            return Error::dns;
        }
        if (r.ipv4.empty())
            return *why = "resolver returned no address", Error::dns;
        for (std::uint32_t ip : r.ipv4)
        {
            sin.sin_addr.s_addr = ip;
            addrs->push_back(sin);
        }
        return Error::none;
    }

    // ---- connect + handshake
    Error open(const ParsedUrl &u, Conn *c, Timing *t, Response *resp, std::string *why)
    {
        auto t0 = SteadyClock::now();
        std::vector<sockaddr_in> addrs;
        bool cached = false;
        bool nx = false;
        Error de = resolve(u, &addrs, &cached, &nx, why);
        t->dns_ms += ms_since(t0);
        if (de != Error::none)
        {
            resp->nxdomain = nx;
            return de;
        }
        t->dns_cached = t->dns_cached || cached;

        t0 = SteadyClock::now();
        int fd = -1;
        bool any_public = false;
        for (const sockaddr_in &a : addrs)
        {
            if (!limits.allow_private && !is_public_ipv4(ntohl(a.sin_addr.s_addr)))
                continue; // never let a catalog point the app at the LAN or at Pegasus on loopback
            any_public = true;
            if (aborted(why))
                return stop;
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                continue;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&a), sizeof a);
            if (rc < 0 && (errno == EINPROGRESS || errno == EALREADY))
            {
                rc = -1;
                const auto since = SteadyClock::now();
                for (;;)
                {
                    if (aborted(why))
                    {
                        ::close(fd);
                        return stop;
                    }
                    if (ms_since(since) > limits.connect_timeout_ms)
                        break;
                    pollfd pfd{fd, POLLOUT, 0};
                    const int pr = ::poll(&pfd, 1, 100);
                    if (pr > 0)
                    {
                        int err = 0;
                        socklen_t len = sizeof err;
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                            rc = 0;
                        break;
                    }
                    if (pr < 0 && errno != EINTR)
                        break;
                }
            }
            if (rc == 0)
                break;
            ::close(fd);
            fd = -1;
        }
        if (!any_public)
            return *why = "host resolves only to non-public addresses", Error::blocked;
        if (fd < 0)
            return *why = "tcp connect failed or timed out", Error::connect;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        t->connect_ms += ms_since(t0);

        t0 = SteadyClock::now();
        Shared::Impl *s = shared->impl();
        SSL *ssl = SSL_new(s->ctx);
        if (!ssl)
        {
            ::close(fd);
            return *why = "SSL_new failed", Error::handshake;
        }
        c->key = key_of(u);
        SSL_set_ex_data(ssl, s->host_index, new std::string(c->key)); // freed by SSL_free
        SSL_set_fd(ssl, fd);
        if (is_ip_literal(u.host))
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), u.host.c_str()); // IP SAN, no SNI
        else
        {
            SSL_set_tlsext_host_name(ssl, u.host.c_str());
            SSL_set1_host(ssl, u.host.c_str()); // host name verification
        }
        {
            Lock lock(s->mutex);
            auto it = s->sessions.find(c->key);
            if (it != s->sessions.end() && it->second)
                SSL_set_session(ssl, it->second);
        }
        const auto hs_start = SteadyClock::now();
        int rc;
        for (;;)
        {
            ERR_clear_error();
            rc = SSL_connect(ssl);
            if (rc == 1)
                break;
            const int e = SSL_get_error(ssl, rc);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            {
                if (!wait_fd(fd, e == SSL_ERROR_WANT_WRITE, hs_start, limits.io_timeout_ms, why))
                {
                    SSL_free(ssl);
                    ::close(fd);
                    return stop;
                }
                continue;
            }
            break;
        }
        if (rc != 1)
        {
            char buf[160];
            ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
            const long v = SSL_get_verify_result(ssl);
            *why = std::string("TLS: ") + buf;
            if (v != X509_V_OK)
            {
                *why += std::string(" (verify: ") + X509_verify_cert_error_string(v) + ")";
                resp->verify_error = v;
                resp->time_error = v == X509_V_ERR_CERT_NOT_YET_VALID ||
                                   v == X509_V_ERR_CERT_HAS_EXPIRED ||
                                   v == X509_V_ERR_CRL_NOT_YET_VALID;
            }
            ERR_clear_error();
            SSL_free(ssl);
            ::close(fd);
            return Error::handshake;
        }
        t->resumed = t->resumed || SSL_session_reused(ssl) != 0;
        t->tls_ms += ms_since(t0);
        c->fd = fd;
        c->ssl = ssl;
        c->last_used = SteadyClock::now();
        return Error::none;
    }

    // Where body bytes go: a vector, or (streaming) the caller's sink. `total` counts bytes either way.
    struct BodyOut
    {
        std::vector<std::uint8_t> *vec = nullptr;
        bool (*sink)(void *, const std::uint8_t *, std::size_t) = nullptr;
        void *sink_ctx = nullptr;
        std::uint64_t total = 0;
        bool sink_failed = false;
        bool push(const std::uint8_t *p, std::size_t k)
        {
            if (sink)
            {
                if (!sink(sink_ctx, p, k))
                    return sink_failed = true, false;
                total += k;
                return true;
            }
            vec->insert(vec->end(), p, p + k);
            total = vec->size();
            return true;
        }
    };

    // ---- HTTP over an open connection
    struct Reader
    {
        Impl *im;
        SSL *ssl;
        int fd;
        std::string buf;
        std::size_t pos = 0;
        std::string why;
        std::size_t avail() const { return buf.size() - pos; }
        bool fill()
        {
            if (pos > (64u << 10) || (pos > 0 && pos == buf.size()))
            {
                buf.erase(0, pos);
                pos = 0;
            }
            char tmp[16384];
            const int n = im->io_read(ssl, fd, tmp, sizeof tmp, &why);
            if (n <= 0)
                return false;
            buf.append(tmp, static_cast<std::size_t>(n));
            return true;
        }
        bool line(std::string *out, std::size_t cap)
        {
            for (;;)
            {
                const std::size_t p = buf.find("\r\n", pos);
                if (p != std::string::npos)
                {
                    *out = buf.substr(pos, p - pos);
                    pos = p + 2;
                    return true;
                }
                if (avail() > cap || !fill())
                    return false;
            }
        }
        // Moves n bytes into dst (null: discard). false on EOF, error, sink refusal or over limit.
        bool take(std::size_t n, BodyOut *dst, std::size_t limit)
        {
            while (n)
            {
                if (avail() == 0 && !fill())
                    return false;
                const std::size_t k = std::min(n, avail());
                if (dst)
                {
                    if (dst->total + k > limit)
                        return false;
                    const auto *p = reinterpret_cast<const std::uint8_t *>(buf.data() + pos);
                    if (!dst->push(p, k))
                        return false;
                }
                pos += k;
                n -= k;
                if (dst && im->opt && im->opt->progress)
                    im->opt->progress(im->opt->hook_ctx, dst->total);
            }
            return true;
        }
    };

    struct Head
    {
        int status = 0;
        std::map<std::string, std::string> h;
        std::string get(const char *k) const
        {
            auto it = h.find(k);
            return it == h.end() ? std::string() : it->second;
        }
    };

    // Returns io (with *got_bytes false) when a pooled connection was stale.
    Error one_request(Conn &c, const ParsedUrl &u, const GetOptions &o, Head *head, Reader *rd,
                      Timing *t, std::string *why, bool *got_bytes)
    {
        const auto t0 = SteadyClock::now();
        const std::string req =
            "GET " + u.target + " HTTP/1.1\r\nHost: " + u.host +
            (u.port == 443 ? "" : ":" + std::to_string(u.port)) +
            "\r\nUser-Agent: WingsTV/0.1\r\nAccept: " +
            std::string(o.accept ? o.accept : "image/webp,image/png,image/jpeg,image/*;q=0.5") +
            "\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n";
        if (!io_write(c.ssl, c.fd, req.data(), req.size(), why))
            return stop == Error::none ? Error::io : stop;
        std::string line;
        if (!rd->line(&line, limits.max_header))
        {
            *why = rd->why.empty() ? "no response" : rd->why;
            return stop == Error::none ? Error::io : stop;
        }
        *got_bytes = true;
        if (line.compare(0, 5, "HTTP/") != 0 || line.size() < 12)
            return *why = "bad status line", Error::protocol;
        head->status = std::atoi(line.c_str() + 9);
        std::size_t total = line.size();
        for (;;)
        {
            if (!rd->line(&line, limits.max_header))
            {
                *why = rd->why.empty() ? "truncated headers" : rd->why;
                return stop == Error::none ? Error::io : stop;
            }
            if (line.empty())
                break;
            total += line.size();
            if (total > limits.max_header)
                return *why = "headers too large", Error::protocol;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos)
                return *why = "bad header", Error::protocol;
            const std::string k = lower(trim(line.substr(0, colon)));
            const std::string v = trim(line.substr(colon + 1));
            auto &slot = head->h[k];
            slot = slot.empty() ? v : slot + ", " + v;
        }
        if (head->status >= 100 && head->status < 200)
            return *why = "unexpected 1xx", Error::protocol;
        if (head->status < 100 || head->status > 599)
            return *why = "bad status code", Error::protocol;
        t->first_byte_ms += ms_since(t0);
        return Error::none;
    }

    // Reads the body (cap enforced). *keep reports whether the connection stays usable.
    Error read_body(Reader *rd, const Head &head, BodyOut *body,
                    std::size_t cap, bool *keep, std::string *why)
    {
        auto fail_io = [&]()
        {
            if (body->sink_failed)
                return Error::sink;
            return stop == Error::none ? Error::io : stop;
        };
        if (lower(head.get("connection")).find("close") != std::string::npos)
            *keep = false;
        const std::string enc = lower(head.get("content-encoding"));
        if (!enc.empty() && enc != "identity")
            return *why = "unexpected content-encoding " + enc, Error::protocol;
        const std::string te = lower(head.get("transfer-encoding"));
        if (te.find("chunked") != std::string::npos)
        {
            for (;;)
            {
                std::string l;
                if (!rd->line(&l, 256))
                    return *why = "bad chunk size line", fail_io();
                const std::size_t semi = l.find(';');
                if (semi != std::string::npos)
                    l.resize(semi);
                l = trim(l);
                if (l.empty() || l.size() > 8 || l.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    return *why = "bad chunk size", Error::protocol;
                const unsigned long long n = std::strtoull(l.c_str(), nullptr, 16);
                if (n == 0)
                {
                    std::string trailer;
                    do
                    {
                        if (!rd->line(&trailer, 4096))
                            return *why = "bad trailer", fail_io();
                    } while (!trailer.empty());
                    return Error::none;
                }
                if (n > cap || body->total + n > cap)
                    return *why = "body exceeds cap", Error::too_large;
                if (!rd->take(static_cast<std::size_t>(n), body, cap))
                    return *why = "truncated chunk", fail_io();
                std::string crlf;
                if (!rd->line(&crlf, 8) || !crlf.empty())
                    return *why = "bad chunk terminator", Error::protocol;
            }
        }
        const std::string cl = head.get("content-length");
        if (!cl.empty())
        {
            // Duplicated headers arrive joined with ", "; all values must agree.
            unsigned long long n = 0;
            bool first = true;
            std::size_t start = 0;
            while (start <= cl.size())
            {
                std::size_t comma = cl.find(',', start);
                if (comma == std::string::npos)
                    comma = cl.size();
                const std::string part = trim(cl.substr(start, comma - start));
                if (part.empty() || part.size() > 18 ||
                    part.find_first_not_of("0123456789") != std::string::npos)
                    return *why = "bad content-length", Error::protocol;
                const unsigned long long v = std::strtoull(part.c_str(), nullptr, 10);
                if (!first && v != n)
                    return *why = "conflicting content-length", Error::protocol;
                n = v;
                first = false;
                start = comma + 1;
            }
            if (n > cap)
                return *why = "content-length " + cl + " exceeds cap", Error::too_large;
            if (body->vec)
                body->vec->reserve(static_cast<std::size_t>(n));
            if (!rd->take(static_cast<std::size_t>(n), body, cap))
                return *why = "truncated body", fail_io();
            return Error::none;
        }
        // No length: the body runs to connection close.
        *keep = false;
        for (;;)
        {
            if (body->total + rd->avail() > cap)
                return *why = "body exceeds cap", Error::too_large;
            if (rd->avail() > 0)
            {
                const auto *p = reinterpret_cast<const std::uint8_t *>(rd->buf.data() + rd->pos);
                if (!body->push(p, rd->avail()))
                    return *why = "sink refused the data", Error::sink;
                if (opt && opt->progress)
                    opt->progress(opt->hook_ctx, body->total);
            }
            rd->pos = rd->buf.size();
            if (!rd->fill())
            {
                if (stop != Error::none)
                    return *why = rd->why, stop;
                return Error::none;
            }
        }
    }

    Response &fail(Response &r, Error e, const std::string &why)
    {
        r.error = e;
        r.detail = why;
        r.body.clear();
        r.body.shrink_to_fit();
        return r;
    }

    Response get(const std::string &first_url, const GetOptions &o)
    {
        Response r;
        opt = &o;
        started = SteadyClock::now();
        stop = Error::none;
        const std::size_t cap = o.max_body ? o.max_body : limits.max_body;
        std::string url = first_url;
        for (int hop = 0; hop <= limits.max_redirects; ++hop)
        {
            std::string why;
            if (aborted(&why))
                return fail(r, stop, why);
            ParsedUrl u;
            if (!parse_https_url(url, &u, &why))
                return fail(r, hop ? Error::redirect : Error::bad_url, why);
            if (!limits.allow_private &&
                (u.host == "localhost" || (u.host.size() > 10 && u.host.compare(u.host.size() - 10, 10, ".localhost") == 0)))
                return fail(r, Error::blocked, "localhost refused");
            r.final_url = url;

            Conn c;
            bool reused = take_idle(key_of(u), &c);
            Head head;
            Reader rd{this, nullptr, -1, {}, 0, {}};
            bool keep = true;
            Error e = Error::none;
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (!reused)
                {
                    e = open(u, &c, &r.timing, &r, &why);
                    if (e != Error::none)
                        return fail(r, e, why);
                }
                r.timing.reused = r.timing.reused || reused;
                head = Head();
                rd = Reader{this, c.ssl, c.fd, {}, 0, {}};
                bool got = false;
                e = one_request(c, u, o, &head, &rd, &r.timing, &why, &got);
                if (e == Error::io && reused && !got && stop == Error::none)
                {
                    destroy(c); // stale keep-alive connection: retry once on a fresh one
                    reused = false;
                    r.timing.reused = false;
                    continue;
                }
                break;
            }
            if (e != Error::none)
            {
                destroy(c);
                return fail(r, e, why);
            }
            r.status = head.status;

            const bool redirect = head.status == 301 || head.status == 302 || head.status == 303 ||
                                  head.status == 307 || head.status == 308;
            if (redirect)
            {
                std::vector<std::uint8_t> small;
                BodyOut small_out;
                small_out.vec = &small;
                const bool drained = read_body(&rd, head, &small_out, 64 << 10, &keep, &why) == Error::none;
                if (drained && keep && rd.avail() == 0)
                    park(c);
                else
                    destroy(c);
                if (stop == Error::cancelled)
                    return fail(r, Error::cancelled, "cancelled");
                const std::string loc = head.get("location");
                if (loc.empty())
                    return fail(r, Error::redirect, "redirect without Location");
                if (hop >= limits.max_redirects)
                    return fail(r, Error::redirect, "too many redirects");
                url = resolve_location(u, loc);
                if (lower(url.substr(0, 8)) != "https://")
                    return fail(r, Error::redirect, "redirect to non-https refused");
                if (o.redirect_ok && !o.redirect_ok(o.hook_ctx, url))
                    return fail(r, Error::redirect, "redirect target not allowed");
                ++r.timing.redirects;
                continue;
            }

            if (head.status < 200 || head.status > 299)
            {
                const std::string ra = head.get("retry-after");
                if (!ra.empty() && ra.size() < 9 && ra.find_first_not_of("0123456789") == std::string::npos)
                    r.retry_after_s = std::min(std::atoi(ra.c_str()), 86400);
                destroy(c);
                return fail(r, Error::http_status, "HTTP " + std::to_string(head.status));
            }

            std::string ct = head.get("content-type");
            const std::size_t semi = ct.find(';');
            if (semi != std::string::npos)
                ct.resize(semi);
            r.content_type = lower(trim(ct));
            const bool image = r.content_type.compare(0, 6, "image/") == 0 &&
                               r.content_type.find("svg") == std::string::npos;
            const bool blob = r.content_type.empty() || r.content_type == "application/octet-stream" ||
                              r.content_type == "binary/octet-stream";
            if (!image && !blob && !o.any_content_type)
            {
                destroy(c);
                return fail(r, Error::bad_type, "content-type '" + r.content_type + "'");
            }

            BodyOut out;
            out.vec = &r.body;
            if (o.sink)
            {
                out.vec = nullptr;
                out.sink = o.sink;
                out.sink_ctx = o.sink_ctx;
            }
            e = read_body(&rd, head, &out, cap, &keep, &why);
            if (e != Error::none)
            {
                destroy(c);
                return fail(r, e, why);
            }
            if (keep && rd.avail() == 0)
                park(c);
            else
                destroy(c);
            r.timing.total_ms = ms_since(started);
            return r;
        }
        return fail(r, Error::redirect, "too many redirects");
    }
};

Client::Client(std::shared_ptr<Shared> shared, Limits limits) : impl_(new Impl)
{
    impl_->shared = std::move(shared);
    impl_->limits = limits;
}

Client::~Client()
{
    close_all();
}

void Client::close_all()
{
    for (auto &c : impl_->idle)
        impl_->destroy(c);
    impl_->idle.clear();
}

int Client::idle_connections() const
{
    return static_cast<int>(impl_->idle.size());
}

Response Client::get(const std::string &url, const GetOptions &options)
{
    return impl_->get(url, options);
}

} // namespace wings::art::tls
