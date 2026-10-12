// Wings prototype: minimal HTTPS GET-to-memory client (OpenSSL + BSD sockets).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tls_client.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <pthread.h>

namespace wings::tls
{

namespace detail
{
// The console's C++ runtime has no std::mutex (see orbit/thread.hpp), so use
// pthreads directly, as the app does.
struct Mutex
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
};
struct LockGuard
{
    explicit LockGuard(Mutex &mu) : mu_(mu) { pthread_mutex_lock(&mu_.m); }
    ~LockGuard() { pthread_mutex_unlock(&mu_.m); }
    Mutex &mu_;
};
} // namespace detail
using detail::LockGuard;
using detail::Mutex;

const char *name(Error e)
{
    switch (e)
    {
    case Error::none: return "none";
    case Error::bad_url: return "bad_url";
    case Error::dns: return "dns";
    case Error::connect: return "connect";
    case Error::handshake: return "handshake";
    case Error::io: return "io";
    case Error::http_status: return "http_status";
    case Error::redirect: return "redirect";
    case Error::too_large: return "too_large";
    case Error::bad_type: return "bad_type";
    case Error::protocol: return "protocol";
    }
    return "?";
}

namespace
{

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
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

// Percent-encodes bytes a URL path must not contain (spaces and non-ASCII in
// catalog URLs are common), leaving existing %XX escapes alone.
std::string escape_target(const std::string &in)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '%' && i + 2 < in.size() && is_hex(in[i + 1]) && is_hex(in[i + 2]))
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

struct Url
{
    std::string host, target;
    int port = 443;
    std::string key() const { return host + ":" + std::to_string(port); }
};

bool parse_url(const std::string &url, Url *out, std::string *why)
{
    if (url.size() > 2048)
        return *why = "url too long", false;
    if (lower(url.substr(0, 8)) != "https://")
        return *why = "only https:// is allowed", false;
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
    for (unsigned char c : auth)
        if (!(std::isalnum(c) || c == '-' || c == '.' || c == '_'))
            return *why = "bad host character", false;
    out->host = lower(auth);
    out->port = port;
    out->target = escape_target(target);
    return true;
}

// RFC 1918, loopback, link-local, CGNAT, multicast/reserved and 0/8 are refused.
bool is_public_ipv4(std::uint32_t ip)
{
    const unsigned a = ip >> 24, b = (ip >> 16) & 255;
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
    return true;
}

// Resolves a Location header against the URL it came from.
std::string resolve_location(const Url &base, const std::string &loc)
{
    if (loc.compare(0, 8, "https://") == 0 || loc.compare(0, 7, "http://") == 0)
        return loc;
    const std::string origin =
        "https://" + base.host + (base.port == 443 ? "" : ":" + std::to_string(base.port));
    if (loc.compare(0, 2, "//") == 0)
        return "https:" + loc;
    if (!loc.empty() && loc[0] == '/')
        return origin + loc;
    std::string dir = base.target.substr(0, base.target.find('?'));
    dir = dir.substr(0, dir.rfind('/') + 1);
    return origin + dir + loc;
}

} // namespace

// ---------------------------------------------------------------- Shared

struct Shared::Impl
{
    SSL_CTX *ctx = nullptr;
    int host_index = -1; // SSL ex_data slot holding the session-cache key

    struct DnsEntry
    {
        std::vector<std::pair<sockaddr_storage, socklen_t>> addrs;
        Clock::time_point expires;
    };
    Mutex mutex;
    std::map<std::string, DnsEntry> dns;
    std::map<std::string, SSL_SESSION *> sessions;
    ResolveFn resolver = nullptr;
    void *resolver_ctx = nullptr;
};

namespace
{
int on_new_session(SSL *ssl, SSL_SESSION *session)
{
    auto *impl = static_cast<Shared::Impl *>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
    auto *key = static_cast<std::string *>(SSL_get_ex_data(ssl, impl->host_index));
    if (!key)
        return 0;
    LockGuard lock(impl->mutex);
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

void Shared::set_resolver(ResolveFn fn, void *ctx)
{
    LockGuard lock(impl_->mutex);
    impl_->resolver = fn;
    impl_->resolver_ctx = ctx;
}

Shared::~Shared()
{
    if (impl_->ctx)
        SSL_CTX_free(impl_->ctx);
    for (auto &kv : impl_->sessions)
        SSL_SESSION_free(kv.second);
}

std::shared_ptr<Shared> Shared::create(const std::string &pem, std::string *error)
{
    std::shared_ptr<Shared> s(new Shared);
    s->impl_->ctx = SSL_CTX_new(TLS_client_method());
    if (!s->impl_->ctx)
        return *error = "SSL_CTX_new failed", nullptr;
    // A console build must prove its entropy source works before any handshake:
    // a silently unseeded generator would make every session key guessable.
    if (RAND_status() != 1)
        return *error = "OpenSSL has no entropy source on this system", nullptr;
    SSL_CTX *ctx = s->impl_->ctx;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_set_app_data(ctx, s->impl_.get());
    SSL_CTX_sess_set_new_cb(ctx, on_new_session);
    s->impl_->host_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, free_key);

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
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
        return *error = "no CA certificates loaded; refusing to run without verification", nullptr;
    *error = std::to_string(added) + " CA certificates";
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
        Clock::time_point last_used;
    };
    std::deque<Conn> idle; // most recently used at the front

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
        while (!idle.empty() &&
               (static_cast<int>(idle.size()) > limits.max_idle_conns ||
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

    // ---- DNS
    bool resolve(const Url &u, std::vector<std::pair<sockaddr_storage, socklen_t>> *addrs,
                 bool *cached, std::string *why)
    {
        Shared::Impl *s = shared->impl();
        in_addr literal{};
        if (inet_pton(AF_INET, u.host.c_str(), &literal) == 1)
        {
            sockaddr_in sin{};
            sin.sin_family = AF_INET;
            sin.sin_addr = literal;
            sin.sin_port = htons(static_cast<std::uint16_t>(u.port));
            sockaddr_storage ss{};
            std::memcpy(&ss, &sin, sizeof sin);
            addrs->assign(1, {ss, static_cast<socklen_t>(sizeof sin)});
            *cached = true;
            return true;
        }
        {
            LockGuard lock(s->mutex);
            auto it = s->dns.find(u.host);
            if (it != s->dns.end() && Clock::now() < it->second.expires)
            {
                *addrs = it->second.addrs;
                for (auto &a : *addrs)
                    reinterpret_cast<sockaddr_in *>(&a.first)->sin_port =
                        htons(static_cast<std::uint16_t>(u.port));
                *cached = true;
                return true;
            }
        }
        std::vector<std::uint32_t> ips;
        int ttl = 600;
        ResolveFn fn;
        void *fn_ctx;
        {
            LockGuard lock(s->mutex);
            fn = s->resolver;
            fn_ctx = s->resolver_ctx;
        }
        if (fn)
        {
            if (!fn(fn_ctx, u.host, &ips, &ttl, why) || ips.empty())
            {
                if (why->empty())
                    *why = "resolver returned no address";
                return false;
            }
        }
        else
        {
            addrinfo hints{};
            hints.ai_family = AF_INET; // PS5 home networks are IPv4; skip AAAA
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *res = nullptr;
            const int rc = getaddrinfo(u.host.c_str(), nullptr, &hints, &res);
            if (rc != 0 || !res)
                return *why = std::string("getaddrinfo: ") + gai_strerror(rc), false;
            for (addrinfo *p = res; p; p = p->ai_next)
                if (p->ai_family == AF_INET)
                    ips.push_back(reinterpret_cast<sockaddr_in *>(p->ai_addr)->sin_addr.s_addr);
            freeaddrinfo(res);
        }
        Shared::Impl::DnsEntry entry;
        for (std::uint32_t ip : ips)
        {
            sockaddr_in sin{};
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = ip;
            sin.sin_port = htons(static_cast<std::uint16_t>(u.port));
            sockaddr_storage ss{};
            std::memcpy(&ss, &sin, sizeof sin);
            entry.addrs.emplace_back(ss, static_cast<socklen_t>(sizeof sin));
        }
        entry.expires = Clock::now() + std::chrono::seconds(ttl);
        *addrs = entry.addrs;
        LockGuard lock(s->mutex);
        s->dns[u.host] = std::move(entry);
        return true;
    }

    // ---- connect + handshake
    Error open(const Url &u, Conn *c, Timing *t, std::string *why)
    {
        auto t0 = Clock::now();
        std::vector<std::pair<sockaddr_storage, socklen_t>> addrs;
        bool cached = false;
        if (!resolve(u, &addrs, &cached, why))
            return Error::dns;
        t->dns_cached = t->dns_cached || cached;
        t->dns_ms += ms_since(t0);

        t0 = Clock::now();
        int fd = -1;
        bool any_public = false;
        for (auto &a : addrs)
        {
            const std::uint32_t ip = ntohl(reinterpret_cast<sockaddr_in *>(&a.first)->sin_addr.s_addr);
            if (!limits.allow_private && !is_public_ipv4(ip))
                continue; // never let a catalog point the app at the LAN or at Pegasus on loopback
            any_public = true;
            fd = ::socket(a.first.ss_family, SOCK_STREAM, 0);
            if (fd < 0)
                continue;
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&a.first), a.second);
            if (rc < 0 && errno == EINPROGRESS)
            {
                pollfd pfd{fd, POLLOUT, 0};
                rc = ::poll(&pfd, 1, limits.connect_timeout_ms);
                int err = 0;
                socklen_t len = sizeof err;
                if (rc == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                    rc = 0;
                else
                    rc = -1;
            }
            if (rc == 0)
            {
                fcntl(fd, F_SETFL, flags);
                break;
            }
            ::close(fd);
            fd = -1;
        }
        if (!any_public)
            return *why = "host resolves only to non-public addresses", Error::dns;
        if (fd < 0)
            return *why = "tcp connect failed or timed out", Error::connect;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        timeval tv{limits.io_timeout_ms / 1000, (limits.io_timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        t->connect_ms += ms_since(t0);

        t0 = Clock::now();
        Shared::Impl *s = shared->impl();
        SSL *ssl = SSL_new(s->ctx);
        c->key = u.key();
        SSL_set_ex_data(ssl, s->host_index, new std::string(c->key)); // freed by SSL_free
        SSL_set_fd(ssl, fd);
        in_addr literal{};
        if (inet_pton(AF_INET, u.host.c_str(), &literal) == 1)
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), u.host.c_str()); // IP literal: certificate IP SAN, no SNI
        else
        {
            SSL_set_tlsext_host_name(ssl, u.host.c_str());
            SSL_set1_host(ssl, u.host.c_str()); // hostname verification
        }
        {
            LockGuard lock(s->mutex);
            auto it = s->sessions.find(c->key);
            if (it != s->sessions.end() && it->second)
                SSL_set_session(ssl, it->second);
        }
        const int rc = SSL_connect(ssl);
        if (rc != 1)
        {
            char buf[160];
            unsigned long e = ERR_get_error();
            ERR_error_string_n(e, buf, sizeof buf);
            const long v = SSL_get_verify_result(ssl);
            *why = std::string("TLS: ") + buf;
            if (v != X509_V_OK)
                *why += std::string(" (verify: ") + X509_verify_cert_error_string(v) + ")";
            ERR_clear_error();
            SSL_free(ssl);
            ::close(fd);
            return Error::handshake;
        }
        t->resumed = t->resumed || SSL_session_reused(ssl) != 0;
        t->tls_ms += ms_since(t0);
        c->fd = fd;
        c->ssl = ssl;
        c->last_used = Clock::now();
        return Error::none;
    }

    // ---- HTTP over an open connection
    static bool write_all(SSL *ssl, const char *p, std::size_t n)
    {
        while (n)
        {
            const int w = SSL_write(ssl, p, static_cast<int>(std::min<std::size_t>(n, 1 << 20)));
            if (w <= 0)
                return false;
            p += w;
            n -= static_cast<std::size_t>(w);
        }
        return true;
    }

    struct Reader
    {
        SSL *ssl;
        std::string buf; // unread bytes
        bool eof = false;
        bool fill()
        {
            char tmp[16384];
            const int n = SSL_read(ssl, tmp, sizeof tmp);
            if (n <= 0)
            {
                eof = true;
                ERR_clear_error();
                return false;
            }
            buf.append(tmp, static_cast<std::size_t>(n));
            return true;
        }
        bool line(std::string *out, std::size_t cap)
        {
            for (;;)
            {
                const std::size_t p = buf.find("\r\n");
                if (p != std::string::npos)
                {
                    *out = buf.substr(0, p);
                    buf.erase(0, p + 2);
                    return true;
                }
                if (buf.size() > cap || !fill())
                    return false;
            }
        }
        // Moves n bytes into dst (null: discard). false on EOF or over limit.
        bool take(std::size_t n, std::vector<std::uint8_t> *dst, std::size_t limit)
        {
            while (n)
            {
                if (buf.empty() && !fill())
                    return false;
                const std::size_t k = std::min(n, buf.size());
                if (dst)
                {
                    if (dst->size() + k > limit)
                        return false;
                    dst->insert(dst->end(), buf.begin(), buf.begin() + static_cast<long>(k));
                }
                buf.erase(0, k);
                n -= k;
            }
            return true;
        }
    };

    struct Head
    {
        int status = 0;
        std::map<std::string, std::string> h;
    };

    // Returns io (with *got_bytes false) when the connection was stale.
    Error one_request(Conn &c, const Url &u, const GetOptions &opt, Head *head, Reader *rd,
                      Timing *t, std::string *why, bool *got_bytes)
    {
        const auto t0 = Clock::now();
        std::string req = "GET " + u.target + " HTTP/1.1\r\nHost: " + u.host +
                          (u.port == 443 ? "" : ":" + std::to_string(u.port)) +
                          "\r\nUser-Agent: WingsTV/0.1\r\nAccept: " +
                          std::string(opt.accept ? opt.accept
                                                 : "image/webp,image/png,image/jpeg,image/*;q=0.5") +
                          "\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n";
        if (!write_all(c.ssl, req.data(), req.size()))
            return *why = "write failed", Error::io;
        std::string line;
        if (!rd->line(&line, limits.max_header))
            return *why = "no response", Error::io;
        *got_bytes = true;
        if (line.compare(0, 5, "HTTP/") != 0 || line.size() < 12)
            return *why = "bad status line", Error::protocol;
        head->status = std::atoi(line.c_str() + 9);
        std::size_t total = line.size();
        for (;;)
        {
            if (!rd->line(&line, limits.max_header))
                return *why = "truncated headers", Error::io;
            if (line.empty())
                break;
            total += line.size();
            if (total > limits.max_header)
                return *why = "headers too large", Error::protocol;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos)
                return *why = "bad header", Error::protocol;
            std::string k = lower(trim(line.substr(0, colon)));
            std::string v = trim(line.substr(colon + 1));
            auto &slot = head->h[k];
            slot = slot.empty() ? v : slot + ", " + v;
        }
        if (head->status >= 100 && head->status < 200)
            return *why = "unexpected 1xx", Error::protocol;
        t->first_byte_ms += ms_since(t0);
        return Error::none;
    }

    // Reads the body (cap enforced). *keep_alive reports whether c stays usable.
    Error read_body(Reader *rd, const Head &head, std::vector<std::uint8_t> *body,
                    std::size_t cap, bool *keep_alive, std::string *why)
    {
        auto find = [&](const char *k) -> std::string
        {
            auto it = head.h.find(k);
            return it == head.h.end() ? std::string() : it->second;
        };
        if (lower(find("connection")).find("close") != std::string::npos)
            *keep_alive = false;
        const std::string enc = lower(find("content-encoding"));
        if (!enc.empty() && enc != "identity")
            return *why = "unexpected content-encoding " + enc, Error::protocol;
        const std::string te = lower(find("transfer-encoding"));
        if (te.find("chunked") != std::string::npos)
        {
            for (;;)
            {
                std::string l;
                if (!rd->line(&l, 256))
                    return *why = "bad chunk size line", Error::io;
                const std::size_t semi = l.find(';');
                if (semi != std::string::npos)
                    l.resize(semi);
                char *end = nullptr;
                const unsigned long long n = std::strtoull(l.c_str(), &end, 16);
                if (end == l.c_str())
                    return *why = "bad chunk size", Error::protocol;
                if (n == 0)
                {
                    std::string trailer;
                    do
                    {
                        if (!rd->line(&trailer, 4096))
                            return *why = "bad trailer", Error::io;
                    } while (!trailer.empty());
                    return Error::none;
                }
                if (n > cap || body->size() + n > cap)
                    return *why = "body exceeds cap", Error::too_large;
                if (!rd->take(static_cast<std::size_t>(n), body, cap))
                    return *why = "truncated chunk", Error::io;
                std::string crlf;
                if (!rd->line(&crlf, 8) || !crlf.empty())
                    return *why = "bad chunk terminator", Error::protocol;
            }
        }
        const std::string cl = find("content-length");
        if (!cl.empty())
        {
            char *end = nullptr;
            const unsigned long long n = std::strtoull(cl.c_str(), &end, 10);
            if (*end != '\0' && *end != ',')
                return *why = "bad content-length", Error::protocol;
            if (n > cap)
                return *why = "content-length " + cl + " exceeds cap", Error::too_large;
            body->reserve(static_cast<std::size_t>(n));
            if (!rd->take(static_cast<std::size_t>(n), body, cap))
                return *why = "truncated body", Error::io;
            return Error::none;
        }
        // No length: the body runs to connection close.
        *keep_alive = false;
        for (;;)
        {
            if (body->size() + rd->buf.size() > cap)
                return *why = "body exceeds cap", Error::too_large;
            body->insert(body->end(), rd->buf.begin(), rd->buf.end());
            rd->buf.clear();
            if (!rd->fill())
                return Error::none;
        }
    }

    Response get(const std::string &first_url, const GetOptions &opt)
    {
        Response r;
        const auto start = Clock::now();
        std::string url = first_url;
        for (int hop = 0; hop <= limits.max_redirects; ++hop)
        {
            if (ms_since(start) > limits.total_timeout_ms)
                return fail(r, Error::io, "total timeout");
            Url u;
            std::string why;
            if (!parse_url(url, &u, &why))
                return fail(r, hop ? Error::redirect : Error::bad_url, why);
            r.final_url = url;

            Conn c;
            bool reused = take_idle(u.key(), &c);
            Head head;
            Reader rd{nullptr, {}, false};
            bool keep = true;
            Error e = Error::none;
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (!reused)
                {
                    e = open(u, &c, &r.timing, &why);
                    if (e != Error::none)
                        return fail(r, e, why);
                }
                r.timing.reused = r.timing.reused || reused;
                head = Head();
                rd = Reader{c.ssl, {}, false};
                bool got = false;
                e = one_request(c, u, opt, &head, &rd, &r.timing, &why, &got);
                if (e == Error::io && reused && !got)
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

            const bool redirect = head.status == 301 || head.status == 302 ||
                                  head.status == 303 || head.status == 307 || head.status == 308;
            if (redirect)
            {
                auto it = head.h.find("location");
                std::vector<std::uint8_t> sink;
                const bool drained =
                    read_body(&rd, head, &sink, 64 << 10, &keep, &why) == Error::none;
                if (drained && keep)
                    park(c);
                else
                    destroy(c);
                if (it == head.h.end())
                    return fail(r, Error::redirect, "redirect without Location");
                url = resolve_location(u, it->second);
                if (lower(url.substr(0, 8)) != "https://")
                    return fail(r, Error::redirect, "redirect to non-https refused");
                ++r.timing.redirects;
                continue;
            }

            if (head.status < 200 || head.status > 299)
            {
                destroy(c);
                return fail(r, Error::http_status, "HTTP " + std::to_string(head.status));
            }

            std::string ct = head.h.count("content-type") ? head.h["content-type"] : "";
            const std::size_t semi = ct.find(';');
            if (semi != std::string::npos)
                ct.resize(semi);
            r.content_type = lower(trim(ct));
            const bool image = r.content_type.compare(0, 6, "image/") == 0 &&
                               r.content_type.find("svg") == std::string::npos;
            const bool blob = r.content_type.empty() || r.content_type == "application/octet-stream" ||
                              r.content_type == "binary/octet-stream";
            if (!image && !blob && !opt.any_content_type)
            {
                destroy(c);
                return fail(r, Error::bad_type, "content-type '" + r.content_type + "'");
            }

            e = read_body(&rd, head, &r.body, limits.max_body, &keep, &why);
            if (e != Error::none)
            {
                destroy(c);
                return fail(r, e, why);
            }
            if (keep && rd.buf.empty())
                park(c);
            else
                destroy(c);
            r.timing.total_ms = ms_since(start);
            return r;
        }
        return fail(r, Error::redirect, "too many redirects");
    }

    void park(Conn &c)
    {
        c.last_used = Clock::now();
        idle.push_front(c);
        c = Conn();
        trim_idle();
    }

    Response &fail(Response &r, Error e, const std::string &why)
    {
        r.error = e;
        r.detail = why;
        r.body.clear();
        return r;
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

Response Client::get(const std::string &url, const GetOptions &options)
{
    return impl_->get(url, options);
}

} // namespace wings::tls
