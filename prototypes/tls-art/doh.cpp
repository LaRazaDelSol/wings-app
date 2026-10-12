// Wings prototype: DNS-over-HTTPS resolver (RFC 8484 GET, A records only).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "doh.hpp"

#include <algorithm>
#include <cstring>

namespace wings::tls
{

namespace
{

std::string base64url(const std::vector<std::uint8_t> &in)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == in.size())
    {
        const unsigned v = in[i] << 16;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
    }
    else if (i + 2 == in.size())
    {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
    }
    return out; // no padding, as RFC 8484 requires
}

// Advances over a (possibly compressed) name. Returns false on malformed data.
bool skip_name(const std::uint8_t *d, std::size_t n, std::size_t *pos)
{
    for (int guard = 0; guard < 128; ++guard)
    {
        if (*pos >= n)
            return false;
        const std::uint8_t len = d[*pos];
        if (len == 0)
        {
            ++*pos;
            return true;
        }
        if ((len & 0xC0) == 0xC0)
        {
            *pos += 2; // a pointer ends the name
            return *pos <= n;
        }
        if (len & 0xC0)
            return false;
        *pos += 1u + len;
    }
    return false;
}

std::uint16_t be16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

} // namespace

std::vector<std::uint8_t> DohResolver::build_query(const std::string &host)
{
    std::vector<std::uint8_t> q = {0, 0,       // id 0: RFC 8484 recommends it for cacheability
                                   0x01, 0x00, // recursion desired
                                   0, 1, 0, 0, 0, 0, 0, 0};
    std::size_t start = 0;
    while (start <= host.size())
    {
        std::size_t dot = host.find('.', start);
        if (dot == std::string::npos)
            dot = host.size();
        const std::size_t len = dot - start;
        if (len > 0 && len < 64)
        {
            q.push_back(static_cast<std::uint8_t>(len));
            q.insert(q.end(), host.begin() + static_cast<long>(start), host.begin() + static_cast<long>(dot));
        }
        start = dot + 1;
    }
    q.push_back(0);
    q.insert(q.end(), {0, 1, 0, 1}); // QTYPE A, QCLASS IN
    return q;
}

bool DohResolver::parse_response(const std::uint8_t *d, std::size_t n,
                                 std::vector<std::uint32_t> *ipv4, int *ttl_seconds,
                                 std::string *why)
{
    if (n < 12)
        return *why = "short DNS response", false;
    const unsigned flags = be16(d + 2);
    const unsigned rcode = flags & 0xF;
    if (rcode != 0)
        return *why = rcode == 3 ? "NXDOMAIN" : "DNS rcode " + std::to_string(rcode), false;
    const unsigned qd = be16(d + 4), an = be16(d + 6);
    std::size_t pos = 12;
    for (unsigned i = 0; i < qd; ++i)
    {
        if (!skip_name(d, n, &pos) || pos + 4 > n)
            return *why = "bad question", false;
        pos += 4;
    }
    unsigned min_ttl = 3600;
    for (unsigned i = 0; i < an; ++i)
    {
        if (!skip_name(d, n, &pos) || pos + 10 > n)
            return *why = "bad answer", false;
        const unsigned type = be16(d + pos), len = be16(d + pos + 8);
        const unsigned ttl = (static_cast<unsigned>(d[pos + 4]) << 24) | (d[pos + 5] << 16) | (d[pos + 6] << 8) | d[pos + 7];
        pos += 10;
        if (pos + len > n)
            return *why = "truncated rdata", false;
        if (type == 1 && len == 4)
        {
            std::uint32_t ip;
            std::memcpy(&ip, d + pos, 4); // already network byte order
            ipv4->push_back(ip);
            min_ttl = std::min(min_ttl, ttl);
        }
        pos += len; // CNAMEs are followed by the server; we only collect A
    }
    if (ipv4->empty())
        return *why = "no A record", false;
    *ttl_seconds = static_cast<int>(std::max(30u, min_ttl));
    return true;
}

DohResolver::DohResolver(std::shared_ptr<Shared> shared, std::vector<std::string> endpoints)
    : shared_(shared), endpoints_(std::move(endpoints)), client_(shared)
{
}

DohResolver::~DohResolver() = default;

void DohResolver::install()
{
    shared_->set_resolver(&DohResolver::resolve_fn, this);
}

bool DohResolver::resolve_fn(void *ctx, const std::string &host, std::vector<std::uint32_t> *ipv4,
                             int *ttl_seconds, std::string *why)
{
    return static_cast<DohResolver *>(ctx)->resolve(host, ipv4, ttl_seconds, why);
}

bool DohResolver::resolve(const std::string &host, std::vector<std::uint32_t> *ipv4,
                          int *ttl_seconds, std::string *why)
{
    pthread_mutex_lock(&mutex_);
    ++lookups_;
    const std::string wire = base64url(build_query(host));
    GetOptions opt;
    opt.accept = "application/dns-message";
    opt.any_content_type = true;
    std::string last = "no DoH endpoint configured";
    bool ok = false;
    for (const std::string &endpoint : endpoints_)
    {
        Response r = client_.get(endpoint + "?dns=" + wire, opt);
        if (!r.ok())
        {
            last = endpoint + ": " + std::string(name(r.error)) + " " + r.detail;
            continue;
        }
        if (r.content_type != "application/dns-message")
        {
            last = endpoint + ": unexpected content-type '" + r.content_type + "'";
            continue;
        }
        ipv4->clear();
        std::string reason;
        if (parse_response(r.body.data(), r.body.size(), ipv4, ttl_seconds, &reason))
        {
            ok = true;
            break;
        }
        last = endpoint + ": " + reason;
        if (reason == "NXDOMAIN")
            break; // authoritative answer; do not ask another resolver
    }
    pthread_mutex_unlock(&mutex_);
    if (!ok)
        *why = "DoH: " + last;
    return ok;
}

} // namespace wings::tls
