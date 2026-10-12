// Wings cover-art module: DNS-over-HTTPS provider (RFC 8484 GET, A records only).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/doh.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <algorithm>
#include <cstring>

namespace wings::art
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
        const unsigned v = (static_cast<unsigned>(in[i]) << 16) | (static_cast<unsigned>(in[i + 1]) << 8) | in[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == in.size())
    {
        const unsigned v = static_cast<unsigned>(in[i]) << 16;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
    }
    else if (i + 2 == in.size())
    {
        const unsigned v = (static_cast<unsigned>(in[i]) << 16) | (static_cast<unsigned>(in[i + 1]) << 8);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
    }
    return out; // no padding, as RFC 8484 requires
}

// Advances over a (possibly compressed) name. False on malformed data.
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

// https://<IPv4 literal>[:port]/path
bool endpoint_ok(const std::string &e)
{
    tls::ParsedUrl u;
    std::string why;
    if (!tls::parse_https_url(e, &u, &why))
        return false;
    in_addr a{};
    return inet_pton(AF_INET, u.host.c_str(), &a) == 1;
}

} // namespace

tls::Limits DohProvider::default_limits()
{
    tls::Limits l;
    l.max_body = 16u << 10;
    l.connect_timeout_ms = 5000;
    l.io_timeout_ms = 6000;
    l.total_timeout_ms = 10000;
    l.idle_keepalive_ms = 60000;
    l.max_idle_conns = 2;
    return l;
}

std::vector<std::uint8_t> DohProvider::build_query(const std::string &host)
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

bool DohProvider::parse_response(const std::uint8_t *d, std::size_t n, DnsResult *out)
{
    out->ipv4.clear();
    if (n < 12)
        return out->error = "short DNS response", false;
    const unsigned flags = be16(d + 2);
    if (!(flags & 0x8000))
        return out->error = "not a DNS response", false;
    if (flags & 0x0200)
        return out->error = "truncated DNS response", false;
    const unsigned rcode = flags & 0xF;
    if (rcode != 0)
    {
        out->nxdomain = rcode == 3;
        return out->error = rcode == 3 ? "NXDOMAIN" : "DNS rcode " + std::to_string(rcode), false;
    }
    const unsigned qd = be16(d + 4), an = be16(d + 6);
    if (qd > 4 || an > 128)
        return out->error = "implausible DNS counts", false;
    std::size_t pos = 12;
    for (unsigned i = 0; i < qd; ++i)
    {
        if (!skip_name(d, n, &pos) || pos + 4 > n)
            return out->error = "bad question", false;
        pos += 4;
    }
    unsigned min_ttl = 3600;
    for (unsigned i = 0; i < an; ++i)
    {
        if (!skip_name(d, n, &pos) || pos + 10 > n)
            return out->error = "bad answer", false;
        const unsigned type = be16(d + pos), len = be16(d + pos + 8);
        const unsigned ttl = (static_cast<unsigned>(d[pos + 4]) << 24) | (static_cast<unsigned>(d[pos + 5]) << 16) |
                             (static_cast<unsigned>(d[pos + 6]) << 8) | d[pos + 7];
        pos += 10;
        if (pos + len > n)
            return out->error = "truncated rdata", false;
        if (type == 1 && len == 4)
        {
            std::uint32_t ip;
            std::memcpy(&ip, d + pos, 4); // already network byte order
            out->ipv4.push_back(ip);
            min_ttl = std::min(min_ttl, ttl);
        }
        pos += len; // CNAMEs are followed by the server; only A records are collected
    }
    if (out->ipv4.empty())
        return out->error = "no A record", false;
    out->ttl_seconds = static_cast<int>(std::max(30u, min_ttl));
    return true;
}

DohProvider::DohProvider(std::shared_ptr<tls::Shared> shared, std::vector<std::string> endpoints,
                         tls::Limits limits)
    : client_(std::move(shared), limits)
{
    for (std::string &e : endpoints)
        (endpoint_ok(e) ? endpoints_ : rejected_).push_back(std::move(e));
}

bool DohProvider::resolve(const std::string &host, DnsResult *out)
{
    Lock lock(mutex_);
    ++lookups_;
    const std::string wire = base64url(build_query(host));
    tls::GetOptions opt;
    opt.accept = "application/dns-message";
    opt.any_content_type = true;
    opt.max_body = 16u << 10;
    std::string last = "no usable DoH endpoint configured";
    for (std::size_t k = 0; k < endpoints_.size(); ++k)
    {
        const std::size_t idx = (preferred_ + k) % endpoints_.size();
        const std::string &endpoint = endpoints_[idx];
        tls::Response r = client_.get(endpoint + "?dns=" + wire, opt);
        if (!r.ok())
        {
            last = endpoint + ": " + tls::name(r.error) + " " + r.detail;
            continue;
        }
        if (r.content_type != "application/dns-message")
        {
            last = endpoint + ": unexpected content-type '" + r.content_type + "'";
            continue;
        }
        DnsResult parsed;
        if (parse_response(r.body.data(), r.body.size(), &parsed))
        {
            preferred_ = idx;
            failed_streak_ = 0;
            *out = std::move(parsed);
            return true;
        }
        if (parsed.nxdomain)
        {
            // Authoritative answer: do not ask another resolver. The resolver
            // itself is healthy, so this does not count as a failure.
            failed_streak_ = 0;
            out->nxdomain = true;
            out->error = "NXDOMAIN";
            return false;
        }
        last = endpoint + ": " + parsed.error;
    }
    ++failed_streak_;
    out->error = "DoH: " + last;
    return false;
}

} // namespace wings::art
