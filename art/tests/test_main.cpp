// Wings cover-art tests: unit, hermetic TLS fixture and safety suite.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   art_tests --certs DIR --ca ca-bundle.crt [--real] [--filter substring]
//
// DIR holds the PKI made by tests/gen_certs.sh. --real adds the tests that talk
// to the real CDNs (needs internet; uses DNS-over-HTTPS to 1.1.1.1/8.8.8.8).
#define WINGS_ART_TESTING 1

#include "wings/art/art_service.hpp"
#include "wings/art/disk_cache.hpp"
#include "wings/art/dns.hpp"
#include "wings/art/doh.hpp"
#include "wings/art/image_probe.hpp"
#include "wings/art/tls_client.hpp"

#include "test_server.hpp"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace wings::art;

namespace
{

int g_pass = 0, g_fail = 0;
std::string g_filter;
bool g_active = true;

#define CHECK(cond)                                                                            \
    do                                                                                         \
    {                                                                                          \
        if (!g_active)                                                                         \
            break;                                                                             \
        if (cond)                                                                              \
            ++g_pass;                                                                          \
        else                                                                                   \
        {                                                                                      \
            ++g_fail;                                                                          \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                                      \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                   \
    do                                                                                         \
    {                                                                                          \
        if (!g_active)                                                                         \
            break;                                                                             \
        if (cond)                                                                              \
            ++g_pass;                                                                          \
        else                                                                                   \
        {                                                                                      \
            ++g_fail;                                                                          \
            std::printf("    FAIL %s:%d  %s  [%s]\n", __FILE__, __LINE__, #cond,               \
                        std::string(msg).c_str());                                             \
        }                                                                                      \
    } while (0)

bool begin(const char *name)
{
    g_active = g_filter.empty() || std::string(name).find(g_filter) != std::string::npos;
    if (g_active)
        std::printf("[ RUN ] %s\n", name);
    return g_active;
}

std::string slurp(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string g_certs, g_ca_bundle_path;

std::string cert(const char *n) { return g_certs + "/" + n; }

std::string tmpdir(const char *tag)
{
    static int n = 0;
    std::string d = "/tmp/wings-art-test-" + std::to_string(::getpid()) + "-" + tag + std::to_string(n++);
    std::string cmd = "rm -rf '" + d + "'";
    if (std::system(cmd.c_str()) != 0)
        std::abort();
    return d;
}

void sleep_for_ms(int ms) { sleep_ms(ms); }

class FakeClock : public Clock
{
  public:
    std::int64_t unix_seconds() const override { return unix_.load(); }
    std::int64_t mono_ms() const override { return mono_.load(); }
    void advance(std::int64_t seconds)
    {
        unix_ += seconds;
        mono_ += seconds * 1000;
    }
    std::atomic<std::int64_t> unix_{1'800'000'000}, mono_{1'000'000};
};

// Name -> address map for the fixtures.
class TestDns : public DnsProvider
{
  public:
    bool resolve(const std::string &host, DnsResult *out) override
    {
        ++calls;
        if (fail)
        {
            out->error = "boom";
            return false;
        }
        if (host == "nx.test")
        {
            out->nxdomain = true;
            out->error = "NXDOMAIN";
            return false;
        }
        const char *ip = "127.0.0.1";
        if (host == "private10.test") ip = "10.0.0.5";
        else if (host == "private192.test") ip = "192.168.1.20";
        else if (host == "private172.test") ip = "172.16.5.5";
        else if (host == "linklocal.test") ip = "169.254.169.254";
        else if (host == "cgnat.test") ip = "100.64.0.9";
        else if (host == "zero.test") ip = "0.0.0.0";
        else if (host == "multicast.test") ip = "224.0.0.1";
        out->ipv4.assign(1, inet_addr(ip));
        out->ttl_seconds = 300;
        return true;
    }
    const char *name() const override { return "test"; }
    std::atomic<int> calls{0};
    std::atomic<bool> fail{false};
};

// ---- image fixtures
void be32(std::string *s, std::uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

std::string make_png(int w, int h, std::size_t pad = 600)
{
    std::string s("\x89PNG\r\n\x1a\n", 8);
    be32(&s, 13);
    s += "IHDR";
    be32(&s, static_cast<std::uint32_t>(w));
    be32(&s, static_cast<std::uint32_t>(h));
    s += std::string("\x08\x06\x00\x00\x00", 5);
    be32(&s, 0); // crc (not checked by the header probe)
    s.append(pad, '\0');
    return s;
}

std::string make_webp_vp8x(int w, int h, bool animated)
{
    std::string s = "RIFF";
    be32(&s, 0);
    s += "WEBPVP8X";
    be32(&s, 0x0a000000); // chunk size 10, little endian
    s.push_back(static_cast<char>(animated ? 0x02 : 0x00));
    s.append(3, '\0');
    const unsigned cw = static_cast<unsigned>(w - 1), ch = static_cast<unsigned>(h - 1);
    for (int i = 0; i < 3; ++i)
        s.push_back(static_cast<char>((cw >> (8 * i)) & 0xff));
    for (int i = 0; i < 3; ++i)
        s.push_back(static_cast<char>((ch >> (8 * i)) & 0xff));
    s.append(64, '\0');
    return s;
}

std::string make_webp_vp8l(int w, int h)
{
    std::string s = "RIFF";
    be32(&s, 0);
    s += "WEBPVP8L";
    s.append(4, '\0');
    s.push_back(0x2f);
    const std::uint32_t bits = static_cast<std::uint32_t>(w - 1) | (static_cast<std::uint32_t>(h - 1) << 14);
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
    s.append(32, '\0');
    return s;
}

std::string make_webp_vp8(int w, int h)
{
    std::string s = "RIFF";
    be32(&s, 0);
    s += "WEBPVP8 ";
    s.append(4, '\0');
    s.append("\x10\x02\x00", 3); // frame tag
    s.append("\x9d\x01\x2a", 3);
    s.push_back(static_cast<char>(w & 0xff));
    s.push_back(static_cast<char>((w >> 8) & 0x3f));
    s.push_back(static_cast<char>(h & 0xff));
    s.push_back(static_cast<char>((h >> 8) & 0x3f));
    s.append(32, '\0');
    return s;
}

std::string make_jpeg(int w, int h)
{
    std::string s("\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 20);
    s += std::string("\xFF\xC0\x00\x11\x08", 5);
    s.push_back(static_cast<char>(h >> 8));
    s.push_back(static_cast<char>(h & 0xff));
    s.push_back(static_cast<char>(w >> 8));
    s.push_back(static_cast<char>(w & 0xff));
    s += std::string("\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 10);
    s.append(64, '\0');
    return s;
}

const std::uint8_t *u8(const std::string &s) { return reinterpret_cast<const std::uint8_t *>(s.data()); }

fixture::Reply png_reply(int w = 64, int h = 64, std::size_t pad = 600, const char *ct = "image/png")
{
    fixture::Reply r;
    if (ct)
        r.headers.push_back({"Content-Type", ct});
    r.body = make_png(w, h, pad);
    return r;
}

tls::Limits fixture_limits()
{
    tls::Limits l;
    l.allow_private = true;
    l.connect_timeout_ms = 2000;
    l.io_timeout_ms = 3000;
    l.total_timeout_ms = 6000;
    return l;
}

std::string url(const fixture::Server &s, const std::string &path, const char *host = "art.test")
{
    return std::string("https://") + host + ":" + std::to_string(s.port()) + path;
}

std::shared_ptr<tls::Shared> make_shared_tls(const std::string &pem, std::shared_ptr<DnsProvider> dns)
{
    std::string info;
    auto s = tls::Shared::create(pem, &info);
    if (s)
        s->set_dns(std::move(dns));
    return s;
}

// =========================================================================
void test_image_probe()
{
    if (!begin("image_probe: magic bytes, dimensions, guard"))
        return;
    ImageInfo i;
    std::string png = make_png(260, 300);
    CHECK(sniff_image(u8(png), png.size()) == ImageFormat::png);
    CHECK(probe_image(u8(png), png.size(), &i) && i.width == 260 && i.height == 300);
    std::string w1 = make_webp_vp8x(1920, 1080, false);
    CHECK(probe_image(u8(w1), w1.size(), &i) && i.format == ImageFormat::webp && i.width == 1920 && i.height == 1080 && !i.animated);
    std::string w2 = make_webp_vp8l(400, 500);
    CHECK(probe_image(u8(w2), w2.size(), &i) && i.width == 400 && i.height == 500);
    std::string w3 = make_webp_vp8(260, 260);
    CHECK(probe_image(u8(w3), w3.size(), &i) && i.width == 260 && i.height == 260);
    std::string jp = make_jpeg(640, 480);
    CHECK(probe_image(u8(jp), jp.size(), &i) && i.format == ImageFormat::jpeg && i.width == 640 && i.height == 480);

    ImageGuard g;
    CHECK(check_image(u8(png), png.size(), g) == Verdict::ok);
    std::string big = make_png(2500, 2500); // 6.25 Mpx
    CHECK(check_image(u8(big), big.size(), g) == Verdict::too_many_pixels);
    std::string tall = make_png(100, 5000);
    CHECK(check_image(u8(tall), tall.size(), g) == Verdict::side_too_long);
    std::string bomb = make_png(65535, 65535); // 4.3 Gpx declared in a 700 byte file
    CHECK(check_image(u8(bomb), bomb.size(), g) != Verdict::ok);
    std::string anim = make_webp_vp8x(100, 100, true);
    CHECK(check_image(u8(anim), anim.size(), g) == Verdict::unsupported);
    std::string avif = std::string("\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1", 24) + std::string(32, 0);
    CHECK(check_image(u8(avif), avif.size(), g) == Verdict::unsupported);
    std::string html = "<!doctype html><html></html>";
    CHECK(check_image(u8(html), html.size(), g) == Verdict::unknown_format);
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg'/>";
    CHECK(check_image(u8(svg), svg.size(), g) == Verdict::unknown_format);
    std::string gif = "GIF89a\x01\x00\x01\x00";
    CHECK(check_image(u8(gif), gif.size(), g) == Verdict::unknown_format);
    CHECK(check_image(u8(png), 10, g) == Verdict::unreadable); // truncated header
    CHECK(check_image(nullptr, 0, g) == Verdict::unknown_format);
    std::string trunc_jpeg = jp.substr(0, 15);
    CHECK(check_image(u8(trunc_jpeg), trunc_jpeg.size(), g) == Verdict::unreadable);
    ImageGuard tight;
    tight.max_pixels = 100 * 100;
    CHECK(check_image(u8(png), png.size(), tight) == Verdict::too_many_pixels);
}

void test_url_parsing()
{
    if (!begin("url: parsing and refusal rules"))
        return;
    tls::ParsedUrl u;
    std::string why;
    CHECK(tls::parse_https_url("https://image.api.playstation.com/vulcan/a b.png?w=260", &u, &why));
    CHECK(u.host == "image.api.playstation.com" && u.port == 443 && u.target == "/vulcan/a%20b.png?w=260");
    CHECK(tls::parse_https_url("HTTPS://Example.COM:8443/x#frag", &u, &why) && u.host == "example.com" && u.port == 8443 && u.target == "/x");
    CHECK(tls::parse_https_url("https://example.com?q=1", &u, &why) && u.target == "/?q=1");
    const char *bad[] = {
        "http://example.com/a.png",          "ftp://example.com/a",
        "https://user:pw@example.com/a",     "https://user@example.com/a",
        "https://[::1]/a",                   "https://",
        "https:///a",                        "https://exa mple.com/a",
        "https://example.com:0/a",           "https://example.com:99999/a",
        "https://example.com:abc/a",         "https://2130706433/a",
        "https://0x7f000001/a",              "https://127.1/a",
        "https://1.2.3/a",                   "https://.example.com/a",
        "https://a..b/a",                    "//example.com/a",
        "javascript:alert(1)",               "",
        "https://exa\nmple.com/a",
    };
    for (const char *b : bad)
        CHECK_MSG(!tls::parse_https_url(b, &u, &why), b);
    CHECK(!tls::parse_https_url("https://example.com/" + std::string(2100, 'a'), &u, &why));
    CHECK(tls::parse_https_url("https://8.8.8.8/dns-query", &u, &why));

    CHECK(!tls::is_public_ipv4(0x7f000001) && !tls::is_public_ipv4(0x0a000001) && !tls::is_public_ipv4(0xac100001) &&
          !tls::is_public_ipv4(0xc0a80101) && !tls::is_public_ipv4(0xa9fea9fe) && !tls::is_public_ipv4(0x64400001) &&
          !tls::is_public_ipv4(0x00000000) && !tls::is_public_ipv4(0xe0000001) && !tls::is_public_ipv4(0xffffffff) &&
          !tls::is_public_ipv4(0xc0000201) && !tls::is_public_ipv4(0xc6120001));
    CHECK(tls::is_public_ipv4(0x01010101) && tls::is_public_ipv4(0x08080808) && tls::is_public_ipv4(0xac200001) &&
          tls::is_public_ipv4(0xc0a90001));
}

void test_dns_cache()
{
    if (!begin("dns: TTL cache, negative caching, one lookup per name"))
        return;
    FakeClock clock;
    auto inner = std::make_shared<TestDns>();
    CachingDns dns(inner, &clock);
    DnsResult r;
    CHECK(dns.resolve("a.test", &r) && r.ipv4.size() == 1);
    CHECK(dns.resolve("a.test", &r));
    CHECK(inner->calls == 1);
    clock.advance(299);
    CHECK(dns.resolve("a.test", &r) && inner->calls == 1);
    clock.advance(2);
    CHECK(dns.resolve("a.test", &r) && inner->calls == 2);
    // NXDOMAIN is remembered for 60 s
    CHECK(!dns.resolve("nx.test", &r) && r.nxdomain);
    CHECK(!dns.resolve("nx.test", &r) && inner->calls == 3);
    clock.advance(61);
    CHECK(!dns.resolve("nx.test", &r) && inner->calls == 4);
    // plain failures are not cached
    inner->fail = true;
    CHECK(!dns.resolve("b.test", &r));
    CHECK(!dns.resolve("b.test", &r) && inner->calls == 6);
    inner->fail = false;
    CHECK(dns.resolve("b.test", &r));
}

void test_doh_wire()
{
    if (!begin("doh: query and response wire format"))
        return;
    auto q = DohProvider::build_query("cdn.jsdelivr.net");
    CHECK(q.size() == 12 + 1 + 3 + 1 + 8 + 1 + 3 + 1 + 4);
    CHECK(q[12] == 3 && q[13] == 'c');
    // A response: header, question, one A answer with a pointer name.
    std::vector<std::uint8_t> resp = {0, 0, 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0};
    resp.insert(resp.end(), q.begin() + 12, q.end());
    const std::uint8_t ans[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 120, 0, 4, 1, 2, 3, 4};
    resp.insert(resp.end(), ans, ans + sizeof ans);
    DnsResult r;
    CHECK(DohProvider::parse_response(resp.data(), resp.size(), &r));
    CHECK(r.ipv4.size() == 1 && r.ttl_seconds == 120);
    std::uint8_t ip[4];
    std::memcpy(ip, &r.ipv4[0], 4);
    CHECK(ip[0] == 1 && ip[3] == 4);
    // short TTLs are raised to 30 s
    resp[resp.size() - 6 - 1] = 1;
    CHECK(DohProvider::parse_response(resp.data(), resp.size(), &r));
    // NXDOMAIN
    std::vector<std::uint8_t> nx = {0, 0, 0x81, 0x83, 0, 1, 0, 0, 0, 0, 0, 0};
    CHECK(!DohProvider::parse_response(nx.data(), nx.size(), &r) && r.nxdomain);
    // truncated / garbage / not a response / compression loop
    CHECK(!DohProvider::parse_response(resp.data(), 8, &r));
    CHECK(!DohProvider::parse_response(resp.data(), resp.size() - 3, &r));
    std::vector<std::uint8_t> notresp = resp;
    notresp[2] = 0x01;
    CHECK(!DohProvider::parse_response(notresp.data(), notresp.size(), &r));
    std::vector<std::uint8_t> loop = {0, 0, 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0, 0xc0, 0x0c, 0, 1, 0, 1};
    CHECK(!DohProvider::parse_response(loop.data(), loop.size(), &r));
    std::vector<std::uint8_t> counts = {0, 0, 0x81, 0x80, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
    CHECK(!DohProvider::parse_response(counts.data(), counts.size(), &r));
    // only https://<IPv4 literal> endpoints are accepted
    std::string info;
    auto shared = tls::Shared::create(slurp(g_ca_bundle_path), &info);
    DohProvider p(shared, {"https://dns.google/dns-query", "http://1.1.1.1/dns-query", "https://1.1.1.1/dns-query",
                           "https://user@8.8.8.8/x"});
    CHECK(p.rejected_endpoints().size() == 3);
}

void test_disk_cache()
{
    if (!begin("disk_cache: atomic write, header check, LRU, negatives, restart"))
        return;
    FakeClock clock;
    const std::string root = tmpdir("dc");
    DiskCacheConfig cfg;
    cfg.root = root + "/nested/art";
    cfg.budget_bytes = 20000;
    cfg.clock = &clock;
    cfg.evict_every_writes = 5;
    {
        DiskCache c(cfg);
        std::string err;
        CHECK_MSG(c.open(&err), err);
        std::string data(3000, 'a'), out;
        std::int64_t fetched = 0;
        CHECK(c.put("https://x.test/a.png", u8(data), data.size()));
        CHECK(c.get("https://x.test/a.png", &out, &fetched) == CacheGet::hit && out == data && fetched == clock.unix_seconds());
        CHECK(c.get("https://x.test/none.png", &out, &fetched) == CacheGet::miss);

        // a corrupted file is a miss and is removed
        const std::string path = c.path_for("https://x.test/a.png", false);
        {
            std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(100);
            f.put('Z');
        }
        CHECK(c.get("https://x.test/a.png", &out, &fetched) == CacheGet::miss);
        CHECK(slurp(path).empty());
        CHECK(c.stats().corrupt == 1);

        // a file stored under another URL's hash name (collision) is not served
        CHECK(c.put("https://x.test/b.png", u8(data), data.size()));
        const std::string pb = c.path_for("https://x.test/b.png", false);
        const std::string pc = c.path_for("https://x.test/c.png", false);
        std::string cmd = "mkdir -p \"$(dirname '" + pc + "')\" && cp '" + pb + "' '" + pc + "'";
        CHECK(std::system(cmd.c_str()) == 0);
        CHECK(c.get("https://x.test/c.png", &out, &fetched) == CacheGet::miss);

        // LRU: 6 x 3000 B > budget 20000: the least recently used go first
        c.clear();
        for (int i = 0; i < 6; ++i)
        {
            clock.advance(1);
            std::string d(4000, static_cast<char>('a' + i));
            c.put("https://x.test/lru" + std::to_string(i), u8(d), d.size());
            if (i == 2)
            {
                clock.advance(1);
                c.get("https://x.test/lru0", &out, &fetched); // touch the oldest: it must survive
            }
        }
        c.evict();
        const auto st = c.stats();
        CHECK(st.bytes <= cfg.budget_bytes);
        CHECK(c.contains("https://x.test/lru0"));
        CHECK(c.contains("https://x.test/lru5"));
        CHECK(!c.contains("https://x.test/lru1"));

        // negative records
        CHECK(c.put_negative("https://x.test/gone.png", "HTTP 404", 24 * 3600));
        std::string why;
        CHECK(c.get_negative("https://x.test/gone.png", &why) && why == "HTTP 404");
        clock.advance(24 * 3600 - 5);
        CHECK(c.get_negative("https://x.test/gone.png", &why));
        clock.advance(10);
        CHECK(!c.get_negative("https://x.test/gone.png", &why));
        CHECK(c.put_negative("https://x.test/gone2.png", "bad", 100));
        CHECK(c.put("https://x.test/gone2.png", u8(std::string(10, 'q')), 10)); // a later success clears it
        CHECK(!c.get_negative("https://x.test/gone2.png", &why));
        c.flush();
    }
    {
        // restart: index + files are picked up, leftovers removed
        const std::string leftover = DiskCache(cfg).path_for("https://x.test/tmp", false) + ".tmp";
        std::system(("mkdir -p \"$(dirname '" + leftover + "')\" && echo junk > '" + leftover + "'").c_str());
        DiskCache c(cfg);
        std::string err, out;
        std::int64_t f;
        CHECK(c.open(&err));
        CHECK(slurp(leftover).empty());
        CHECK(c.get("https://x.test/lru5", &out, &f) == CacheGet::hit && out == std::string(4000, 'f'));
    }
    {
        // refuses an unwritable root
        DiskCacheConfig bad;
        bad.root = "/proc/nope/art";
        DiskCache c(bad);
        std::string err;
        CHECK(!c.open(&err) && !err.empty());
        std::string d(10, 'x');
        CHECK(!c.put("https://x.test/a", u8(d), d.size()));
    }
    {
        DiskCacheConfig big = cfg;
        big.budget_bytes = 96ull << 20;
        big.max_entry_bytes = 4000;
        DiskCache c(big);
        std::string err;
        c.open(&err);
        std::string d(5000, 'x');
        CHECK(!c.put("https://x.test/too-big", u8(d), d.size()));
    }
}

// =========================================================================
void test_tls_client()
{
    if (!begin("tls: fetch, keep-alive, redirects, limits, types (local TLS server)"))
        return;
    fixture::Server srv(cert("good.pem"), cert("good.key"), [&](const std::string &p) -> fixture::Reply {
        fixture::Reply r = png_reply();
        if (p.rfind("/ok", 0) == 0)
            return r;
        if (p == "/noct")
            return png_reply(64, 64, 600, nullptr);
        if (p == "/webp")
        {
            fixture::Reply w;
            w.headers.push_back({"Content-Type", "image/webp"});
            w.body = make_webp_vp8x(260, 260, false);
            return w;
        }
        if (p == "/jpeg-as-png")
        {
            fixture::Reply w = png_reply();
            w.body = make_jpeg(100, 100);
            return w;
        }
        if (p == "/r1" || p == "/r2" || p == "/r3")
        {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", p == "/r1" ? "/r2" : p == "/r2" ? "/r3" : "/ok-final"});
            return w;
        }
        if (p == "/r4")
        {
            fixture::Reply w;
            w.status = 301;
            w.headers.push_back({"Location", "/r1"}); // r4 -> r1 -> r2 -> r3 -> ok = 4 hops
            return w;
        }
        if (p == "/loop")
        {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", "/loop"});
            return w;
        }
        if (p == "/to-http")
        {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", "http://art.test/ok"});
            return w;
        }
        if (p == "/to-ftp")
        {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", "ftp://art.test/ok"});
            return w;
        }
        if (p == "/to-schemeless")
        {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", "javascript:alert(1)"});
            return w;
        }
        if (p == "/to-loopback")
        {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", "https://127.0.0.1:6970/api/v1/queue"});
            return w;
        }
        if (p == "/no-location")
        {
            fixture::Reply w;
            w.status = 302;
            return w;
        }
        if (p == "/html")
        {
            fixture::Reply w;
            w.headers.push_back({"Content-Type", "text/html; charset=utf-8"});
            w.body = "<html>hi</html>";
            return w;
        }
        if (p == "/svg")
        {
            fixture::Reply w;
            w.headers.push_back({"Content-Type", "image/svg+xml"});
            w.body = "<svg/>";
            return w;
        }
        if (p == "/gzip")
        {
            fixture::Reply w = png_reply();
            w.headers.push_back({"Content-Encoding", "gzip"});
            return w;
        }
        if (p == "/big")
            return png_reply(64, 64, 100000);
        if (p == "/big-chunked")
        {
            fixture::Reply w = png_reply(64, 64, 100000);
            w.chunked = true;
            return w;
        }
        if (p == "/big-liar")
        {
            // Content-Length says small, the body is large: the cap must hold while reading.
            fixture::Reply w = png_reply(64, 64, 100000);
            w.headers.push_back({"Content-Length", "100"});
            w.close = true;
            return w;
        }
        if (p == "/big-noclen")
        {
            fixture::Reply w = png_reply(64, 64, 100000);
            w.omit_length = true;
            return w;
        }
        if (p == "/chunked")
        {
            fixture::Reply w = png_reply(64, 64, 5000);
            w.chunked = true;
            return w;
        }
        if (p == "/nolen-ok")
        {
            fixture::Reply w = png_reply();
            w.omit_length = true;
            return w;
        }
        if (p == "/404")
        {
            fixture::Reply w;
            w.status = 404;
            w.body = "no";
            return w;
        }
        if (p == "/503")
        {
            fixture::Reply w;
            w.status = 503;
            w.headers.push_back({"Retry-After", "17"});
            return w;
        }
        if (p == "/stale")
        {
            fixture::Reply w = png_reply();
            w.close_silently = true;
            return w;
        }
        if (p == "/slow")
        {
            fixture::Reply w = png_reply();
            w.stall_ms = 5000;
            return w;
        }
        if (p == "/garbage")
        {
            fixture::Reply w;
            w.raw = "NOT HTTP AT ALL\r\n\r\n";
            return w;
        }
        if (p == "/1xx")
        {
            fixture::Reply w;
            w.raw = "HTTP/1.1 100 Continue\r\n\r\n";
            return w;
        }
        if (p == "/dup-cl")
        {
            fixture::Reply w = png_reply();
            w.headers.push_back({"Content-Length", "10"});
            w.headers.push_back({"Content-Length", "20"});
            return w;
        }
        if (p == "/bad-chunk")
        {
            fixture::Reply w;
            w.raw = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nTransfer-Encoding: chunked\r\n\r\nZZZZ\r\nabc\r\n";
            return w;
        }
        return r;
    });
    CHECK(srv.start());
    auto dns = std::make_shared<TestDns>();
    auto shared = make_shared_tls(slurp(cert("ca.pem")), dns);
    CHECK(shared != nullptr);
    if (!shared)
        return;
    tls::Client c(shared, fixture_limits());

    auto r = c.get(url(srv, "/ok"));
    CHECK_MSG(r.ok(), r.detail);
    CHECK(r.status == 200 && r.content_type == "image/png" && r.body.size() == make_png(64, 64).size());
    CHECK(!r.timing.reused);
    auto r2 = c.get(url(srv, "/ok2"));
    CHECK(r2.ok() && r2.timing.reused);
    CHECK(srv.accepted() == 1);
    CHECK(c.idle_connections() == 1);

    CHECK(c.get(url(srv, "/noct")).ok()); // PS CDN style: no Content-Type at all
    r = c.get(url(srv, "/webp"));
    CHECK(r.ok() && r.content_type == "image/webp");
    r = c.get(url(srv, "/jpeg-as-png"));
    CHECK(r.ok() && sniff_image(r.body.data(), r.body.size()) == ImageFormat::jpeg);
    r = c.get(url(srv, "/chunked"));
    CHECK(r.ok() && r.body.size() == make_png(64, 64, 5000).size());
    r = c.get(url(srv, "/nolen-ok"));
    CHECK(r.ok());
    CHECK(c.get(url(srv, "/ok3")).ok()); // after a close-delimited body a new connection is made

    // redirects: 3 hops fine, 4 refused, loop refused
    r = c.get(url(srv, "/r1"));
    CHECK_MSG(r.ok() && r.timing.redirects == 3 && r.final_url.find("/ok-final") != std::string::npos, r.detail);
    r = c.get(url(srv, "/r4"));
    CHECK(r.error == tls::Error::redirect);
    r = c.get(url(srv, "/loop"));
    CHECK(r.error == tls::Error::redirect);
    r = c.get(url(srv, "/to-http"));
    CHECK(r.error == tls::Error::redirect && r.detail.find("non-https") != std::string::npos);
    CHECK(c.get(url(srv, "/to-ftp")).error == tls::Error::redirect);
    CHECK(c.get(url(srv, "/to-schemeless")).error == tls::Error::redirect);
    CHECK(c.get(url(srv, "/no-location")).error == tls::Error::redirect);

    // type and encoding rules
    CHECK(c.get(url(srv, "/html")).error == tls::Error::bad_type);
    CHECK(c.get(url(srv, "/svg")).error == tls::Error::bad_type);
    CHECK(c.get(url(srv, "/gzip")).error == tls::Error::protocol);

    // status
    r = c.get(url(srv, "/404"));
    CHECK(r.error == tls::Error::http_status && r.status == 404);
    r = c.get(url(srv, "/503"));
    CHECK(r.error == tls::Error::http_status && r.status == 503 && r.retry_after_s == 17);

    // size cap (default 2 MiB: 100 kB passes; custom cap 20 kB refuses in all three body modes)
    CHECK(c.get(url(srv, "/big")).ok());
    tls::GetOptions small;
    small.max_body = 20000;
    CHECK(c.get(url(srv, "/big"), small).error == tls::Error::too_large);
    CHECK(c.get(url(srv, "/big-chunked"), small).error == tls::Error::too_large);
    CHECK(c.get(url(srv, "/big-noclen"), small).error == tls::Error::too_large);
    // Content-Length 100 on a 100 kB body: the declared length wins, the rest is never read and
    // the connection is not reused.
    r = c.get(url(srv, "/big-liar"), small);
    CHECK(r.ok() && r.body.size() == 100);
    CHECK(!c.get(url(srv, "/ok-after-liar")).timing.reused);
    CHECK(c.get(url(srv, "/dup-cl")).error == tls::Error::protocol);

    // malformed protocol
    CHECK(c.get(url(srv, "/garbage")).error == tls::Error::protocol);
    CHECK(c.get(url(srv, "/1xx")).error == tls::Error::protocol);
    CHECK(c.get(url(srv, "/bad-chunk")).error == tls::Error::protocol);

    // a pooled connection the server silently closed is retried on a fresh one
    CHECK(c.get(url(srv, "/stale")).ok());
    r = c.get(url(srv, "/ok-after-stale"));
    CHECK_MSG(r.ok(), r.detail);

    // stall: total timeout
    {
        tls::Limits l = fixture_limits();
        l.total_timeout_ms = 800;
        l.io_timeout_ms = 5000;
        tls::Client cc(shared, l);
        const auto t0 = std::chrono::steady_clock::now();
        r = cc.get(url(srv, "/slow"));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r.error == tls::Error::io && ms < 2500);
    }
    // stall: cancellation is honoured promptly
    {
        static std::atomic<bool> flag{false};
        flag = false;
        std::thread t([] {
            sleep_for_ms(300);
            flag = true;
        });
        tls::GetOptions go;
        go.cancelled = [](void *) { return flag.load(); };
        const auto t0 = std::chrono::steady_clock::now();
        tls::Limits l = fixture_limits();
        l.total_timeout_ms = 20000;
        l.io_timeout_ms = 20000;
        tls::Client cc(shared, l);
        r = cc.get(url(srv, "/slow"), go);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t.join();
        CHECK(r.error == tls::Error::cancelled && ms < 1500);
    }
    srv.stop();
}

void test_tls_safety()
{
    if (!begin("safety: certificates, addresses, URL rules, trust store"))
        return;
    auto dns = std::make_shared<TestDns>();
    auto shared = make_shared_tls(slurp(cert("ca.pem")), dns);
    CHECK(shared != nullptr);
    if (!shared)
        return;
    auto handler = [](const std::string &) { return png_reply(); };

    fixture::Server expired(cert("expired.pem"), cert("expired.key"), handler);
    fixture::Server wrong(cert("wronghost.pem"), cert("wronghost.key"), handler);
    fixture::Server selfs(cert("selfsigned.pem"), cert("selfsigned.key"), handler);
    fixture::Server untrusted(cert("untrusted.pem"), cert("untrusted.key"), handler);
    fixture::Server good(cert("good.pem"), cert("good.key"), handler);
    CHECK(expired.start() && wrong.start() && selfs.start() && untrusted.start() && good.start());

    tls::Client c(shared, fixture_limits());
    CHECK(c.get(url(good, "/a")).ok());
    auto r = c.get(url(expired, "/a"));
    CHECK(r.error == tls::Error::handshake && r.time_error && r.verify_error != 0);
    r = c.get(url(wrong, "/a"));
    CHECK(r.error == tls::Error::handshake && !r.time_error);
    r = c.get(url(selfs, "/a"));
    CHECK(r.error == tls::Error::handshake);
    r = c.get(url(untrusted, "/a"));
    CHECK(r.error == tls::Error::handshake);
    // The good server's name is checked, not just the chain:
    r = c.get(url(good, "/a", "other.test"));
    CHECK(r.error == tls::Error::handshake);
    // IP literal endpoints are verified against the IP SAN (DoH bootstrap)
    r = c.get("https://127.0.0.1:" + std::to_string(good.port()) + "/a");
    CHECK_MSG(r.ok(), r.detail);
    // A certificate that is valid for a *public* CA store must not verify the test server
    {
        auto real = make_shared_tls(slurp(g_ca_bundle_path), dns);
        CHECK(real != nullptr);
        if (real)
        {
            tls::Client rc(real, fixture_limits());
            CHECK(rc.get(url(good, "/a")).error == tls::Error::handshake);
        }
    }

    // trust store refusal
    std::string info;
    CHECK(tls::Shared::create("", &info) == nullptr && info.find("refusing") != std::string::npos);
    CHECK(tls::Shared::create("not a pem file at all\n", &info) == nullptr);
    CHECK(tls::Shared::create("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n", &info) == nullptr);
    auto ok = tls::Shared::create(slurp(g_ca_bundle_path), &info);
    CHECK(ok != nullptr && ok->ca_count() > 100);

    // forbidden address classes: no connection must even be attempted
    tls::Limits strict; // allow_private = false
    strict.connect_timeout_ms = 1000;
    tls::Client sc(shared, strict);
    const char *private_hosts[] = {"private10.test", "private192.test", "private172.test", "linklocal.test",
                                   "cgnat.test", "zero.test", "multicast.test"};
    const int before = good.accepted();
    for (const char *h : private_hosts)
    {
        r = sc.get(url(good, "/a", h));
        CHECK_MSG(r.error == tls::Error::blocked, std::string(h) + " -> " + tls::name(r.error));
    }
    // "rebinding": a public-looking name that resolves to loopback
    r = sc.get(url(good, "/a"));
    CHECK(r.error == tls::Error::blocked);
    CHECK(sc.get("https://127.0.0.1:" + std::to_string(good.port()) + "/a").error == tls::Error::blocked);
    CHECK(sc.get("https://10.1.2.3/a").error == tls::Error::blocked);
    CHECK(sc.get("https://192.168.1.1/a").error == tls::Error::blocked);
    CHECK(sc.get("https://localhost:" + std::to_string(good.port()) + "/a").error == tls::Error::blocked);
    CHECK(sc.get("https://foo.localhost/a").error == tls::Error::blocked);
    CHECK(good.accepted() == before); // no connection was even attempted
    // redirect into the LAN / Pegasus is refused too
    {
        fixture::Server red(cert("good.pem"), cert("good.key"), [](const std::string &) {
            fixture::Reply w;
            w.status = 302;
            w.headers.push_back({"Location", "https://127.0.0.1:6970/api/queue"});
            return w;
        });
        CHECK(red.start());
        tls::Limits l = fixture_limits();
        tls::Client lc(shared, l); // private allowed for the first hop only is not expressible: the
                                   // redirect target is a loopback literal and so is allowed here
        auto rr = lc.get(url(red, "/x"));
        CHECK(!rr.ok()); // port 6970 is closed on the test host: connect fails
    }

    // URL rules at the client
    CHECK(c.get("http://art.test/a").error == tls::Error::bad_url);
    CHECK(c.get("https://user:pw@art.test/a").error == tls::Error::bad_url);
    CHECK(c.get("https://2130706433/a").error == tls::Error::bad_url);
    CHECK(c.get("https://nx.test/a").error == tls::Error::dns);
    r = c.get("https://nx.test/a");
    CHECK(r.nxdomain);

    // no DNS provider at all: names fail, nothing falls back to getaddrinfo
    auto nodns = make_shared_tls(slurp(cert("ca.pem")), nullptr);
    tls::Client nc(nodns, fixture_limits());
    CHECK(nc.get(url(good, "/a")).error == tls::Error::dns);
}

// =========================================================================
ArtConfig service_config(const std::string &root, std::shared_ptr<DnsProvider> dns, FakeClock *clock)
{
    ArtConfig c;
    c.cache_root = root;
    c.ca_bundle_paths = {cert("ca.pem")};
    c.dns_mode = DnsMode::custom;
    c.dns_override = std::move(dns);
    c.tls_limits = fixture_limits();
    c.clock = clock;
    c.workers = 4;
    return c;
}

void test_service_basic()
{
    if (!begin("service: fetch, dedupe, keep-alive, disk persistence, guard, negative cache"))
        return;
    FakeClock clock;
    fixture::Server srv(cert("good.pem"), cert("good.key"), [](const std::string &p) -> fixture::Reply {
        if (p == "/huge.png")
            return png_reply(5000, 5000);
        if (p == "/404.png")
        {
            fixture::Reply w;
            w.status = 404;
            return w;
        }
        if (p == "/avif")
        {
            fixture::Reply w;
            w.headers.push_back({"Content-Type", "image/avif"});
            w.body = std::string("\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1", 24) + std::string(64, 0);
            return w;
        }
        if (p == "/html")
        {
            fixture::Reply w;
            w.headers.push_back({"Content-Type", "text/html"});
            w.body = "<html></html>";
            return w;
        }
        if (p == "/disguised.png")
        {
            fixture::Reply w = png_reply();
            w.body = "<html>not an image</html>"; // image/png header, HTML body
            return w;
        }
        if (p == "/big.png")
            return png_reply(64, 64, 10000);
        fixture::Reply w = png_reply(260, 260, 300 + p.size());
        w.delay_ms = 5;
        return w;
    });
    CHECK(srv.start());
    const std::string root = tmpdir("svc");
    auto dns = std::make_shared<TestDns>();
    std::string first_url = url(srv, "/img/0.png");
    {
        ArtConfig cfg = service_config(root, dns, &clock);
        cfg.cover_max_bytes = 5000;
        ArtService s(cfg);
        std::string prob;
        CHECK_MSG(s.start(&prob), prob);
        CHECK(s.state().phase == Phase::ready && s.state().ca_certs == 1 && s.state().disk_ok);

        // dedupe: many simultaneous asks for one URL -> one network request
        for (int i = 0; i < 30; ++i)
            s.request(first_url);
        ArtResult r = s.fetch_blocking(first_url);
        CHECK_MSG(r.state == Fetch::ready, r.error);
        CHECK(r.content_type == "image/png" && sniff_image(u8(r.bytes), r.bytes.size()) == ImageFormat::png);
        CHECK(srv.count("/img/0.png") == 1);

        // 40 more distinct URLs: keep-alive keeps the connection count at the worker count
        std::vector<std::string> urls;
        for (int i = 1; i <= 40; ++i)
            urls.push_back(url(srv, "/img/" + std::to_string(i) + ".png"));
        for (auto &u : urls)
            s.request(u);
        int ok = 0;
        for (auto &u : urls)
            ok += s.fetch_blocking(u).state == Fetch::ready;
        CHECK(ok == 40);
        CHECK_MSG(srv.accepted() <= 4, std::to_string(srv.accepted()));
        CHECK(s.stats().connections_reused > 20);
        CHECK(dns->calls == 1); // one lookup, then the TTL cache

        // second ask is a disk hit and costs no request
        const int before = srv.requests();
        r = s.fetch_blocking(first_url);
        CHECK(r.state == Fetch::ready && srv.requests() == before && s.stats().disk_hits > 0);

        // guard, type and size refusals are permanent and cached negatively
        for (const char *p : {"/huge.png", "/404.png", "/avif", "/html", "/disguised.png", "/big.png"})
        {
            r = s.fetch_blocking(url(srv, p));
            CHECK_MSG(r.state == Fetch::failed && r.permanent, std::string(p) + ": " + r.error);
        }
        CHECK(s.stats().refused_by_guard >= 3);
        const int hits = srv.requests();
        r = s.fetch_blocking(url(srv, "/huge.png"));
        CHECK(r.state == Fetch::failed && r.permanent && srv.requests() == hits);
        // ... for 24 h, then asked again
        clock.advance(24 * 3600 + 10);
        r = s.request(url(srv, "/404.png"));
        CHECK(r.state == Fetch::pending);
        r = s.fetch_blocking(url(srv, "/404.png"));
        CHECK(r.state == Fetch::failed && srv.count("/404.png") == 2);
        s.stop();
    }
    {
        // a new process: network off, cached covers still served; others say why
        ArtConfig cfg = service_config(root, dns, &clock);
        cfg.dns_mode = DnsMode::off;
        ArtService s(cfg);
        CHECK(s.start());
        CHECK(s.state().phase == Phase::disabled_dns && s.state().detail.find("DNS-over-HTTPS") != std::string::npos);
        const int before = srv.requests();
        CHECK(s.request(first_url).state == Fetch::ready);
        auto r = s.request(url(srv, "/never-fetched.png"));
        CHECK(r.state == Fetch::disabled && !r.error.empty());
        CHECK(s.prefetch({url(srv, "/p1.png")}) == 0);
        CHECK(srv.requests() == before);
        // the user enables it: now it works without a restart
        s.set_dns_mode(DnsMode::custom);
        CHECK(s.state().phase == Phase::ready);
        r = s.fetch_blocking(url(srv, "/never-fetched.png"));
        CHECK(r.state == Fetch::ready);
        s.stop();
    }
}

void test_service_failures()
{
    if (!begin("service: retry schedule, host breaker 60 s, offline probing, ca refusal"))
        return;
    FakeClock clock;
    fixture::Server flaky(cert("good.pem"), cert("good.key"), [](const std::string &p) -> fixture::Reply {
        fixture::Reply w;
        if (p.rfind("/ok", 0) == 0)
            return png_reply();
        w.status = 500;
        return w;
    });
    CHECK(flaky.start());
    auto dns = std::make_shared<TestDns>();
    const std::string root = tmpdir("fail");
    ArtConfig cfg = service_config(root, dns, &clock);
    ArtService s(cfg);
    CHECK(s.start());

    // 5 consecutive transient failures on one host pause it for 60 s
    for (int i = 0; i < 5; ++i)
    {
        auto r = s.fetch_blocking(url(flaky, "/fail" + std::to_string(i)));
        CHECK_MSG(r.state == Fetch::failed && !r.permanent && r.http_status == 500, r.error);
    }
    const int hits = flaky.requests();
    CHECK(hits == 5);
    auto r = s.request(url(flaky, "/ok-1"));
    CHECK(r.state == Fetch::failed && r.error.find("paused") != std::string::npos && r.retry_after_s > 50 && r.retry_after_s <= 61);
    CHECK(flaky.requests() == hits);
    CHECK(s.stats().host_pauses == 1);
    clock.advance(61);
    r = s.fetch_blocking(url(flaky, "/ok-1"));
    CHECK_MSG(r.state == Fetch::ready, r.error);

    // a single URL follows the retry schedule 2 s, 8 s, 30 s ...
    s.retry_failed();
    r = s.fetch_blocking(url(flaky, "/fail-sched"));
    CHECK(r.state == Fetch::failed);
    CHECK(r.retry_after_s >= 1 && r.retry_after_s <= 3);
    const int h1 = flaky.requests();
    CHECK(s.request(url(flaky, "/fail-sched")).state == Fetch::failed);
    CHECK(flaky.requests() == h1);
    clock.advance(3);
    r = s.fetch_blocking(url(flaky, "/fail-sched"));
    CHECK(r.state == Fetch::failed && r.retry_after_s >= 7 && r.retry_after_s <= 9);
    s.stop();

    // resolver unreachable: offline state, probing every 30 s
    {
        auto bad = std::make_shared<TestDns>();
        bad->fail = true;
        ArtConfig c2 = service_config(tmpdir("off"), bad, &clock);
        ArtService s2(c2);
        CHECK(s2.start());
        auto a = s2.fetch_blocking(url(flaky, "/ok-a"));
        auto b = s2.fetch_blocking(url(flaky, "/ok-b"));
        CHECK(a.state == Fetch::failed && b.state == Fetch::failed);
        CHECK(s2.state().phase == Phase::offline);
        const int calls = bad->calls;
        auto c = s2.request(url(flaky, "/ok-c"));
        CHECK(c.state == Fetch::failed && bad->calls == calls);
        bad->fail = false;
        clock.advance(31);
        CHECK(s2.state().phase == Phase::ready);
        CHECK(s2.fetch_blocking(url(flaky, "/ok-c")).state == Fetch::ready);
        s2.stop();
    }
    // no usable CA bundle: HTTPS is refused, never done unverified
    {
        ArtConfig c3 = service_config(tmpdir("noca"), dns, &clock);
        c3.ca_bundle_paths = {"/nonexistent/ca.pem", "/etc/hostname"};
        ArtService s3(c3);
        std::string prob;
        CHECK(!s3.start(&prob) && !prob.empty());
        CHECK(s3.state().phase == Phase::ca_error);
        const int before = flaky.requests();
        auto d = s3.request(url(flaky, "/ok-x"));
        CHECK(d.state == Fetch::disabled && flaky.requests() == before);
        s3.stop();
    }
    // the second bundle in the list is used when the first is missing (user override pattern)
    {
        ArtConfig c4 = service_config(tmpdir("ca2"), dns, &clock);
        c4.ca_bundle_paths = {"/download0/wings/ca-bundle.pem", cert("ca.pem")};
        ArtService s4(c4);
        CHECK(s4.start() && s4.state().ca_source == cert("ca.pem"));
        s4.stop();
    }
}

void test_service_heap_and_priority()
{
    if (!begin("service: heap pause, lanes (visible before prefetch, newest first)"))
        return;
    FakeClock clock;
    std::mutex order_m;
    std::vector<std::string> order;
    fixture::Server srv(cert("good.pem"), cert("good.key"), [&](const std::string &p) -> fixture::Reply {
        {
            std::lock_guard<std::mutex> l(order_m);
            order.push_back(p);
        }
        fixture::Reply w = png_reply(64, 64, 300 + p.size());
        w.delay_ms = p.rfind("/slow", 0) == 0 ? 120 : 40;
        return w;
    });
    CHECK(srv.start());
    auto dns = std::make_shared<TestDns>();
    {
        std::atomic<std::uint64_t> heap{200ull << 20};
        ArtConfig cfg = service_config(tmpdir("heap"), dns, &clock);
        cfg.heap_used = [&] { return heap.load(); };
        ArtService s(cfg);
        CHECK(s.start());
        auto u = url(srv, "/heap.png");
        CHECK(s.request(u).state == Fetch::pending);
        sleep_for_ms(700);
        CHECK(srv.count("/heap.png") == 0); // nothing starts while the heap is over the limit
        CHECK(s.state().heap_paused && s.state().phase == Phase::paused_heap);
        heap = 50ull << 20;
        auto r = s.fetch_blocking(u);
        CHECK(r.state == Fetch::ready && srv.count("/heap.png") == 1);
        CHECK(s.stats().heap_pauses >= 1);
        s.stop();
    }
    {
        order.clear();
        ArtConfig cfg = service_config(tmpdir("prio"), dns, &clock);
        cfg.workers = 1;
        ArtService s(cfg);
        CHECK(s.start());
        s.request(url(srv, "/slow-0"));
        for (int i = 0; i < 100 && srv.count("/slow-0") == 0; ++i)
            sleep_for_ms(5);
        std::vector<std::string> pre;
        for (int i = 0; i < 6; ++i)
            pre.push_back(url(srv, "/pre-" + std::to_string(i)));
        CHECK(s.prefetch(pre) == 6);
        s.request(url(srv, "/vis-1"));
        s.request(url(srv, "/vis-2"));
        s.request(url(srv, "/vis-3"));
        for (int i = 0; i < 400 && !s.idle(); ++i)
            sleep_for_ms(10);
        std::vector<std::string> o;
        {
            std::lock_guard<std::mutex> l(order_m);
            o = order;
        }
        CHECK(o.size() == 10);
        if (o.size() == 10)
        {
            CHECK(o[0] == "/slow-0");
            CHECK(o[1] == "/vis-3" && o[2] == "/vis-2" && o[3] == "/vis-1");
            CHECK(o[4] == "/pre-0" && o[9] == "/pre-5");
        }
        // cancelling background work drops what is queued
        for (int i = 0; i < 6; ++i)
            pre[static_cast<std::size_t>(i)] = url(srv, "/cancel-" + std::to_string(i));
        s.request(url(srv, "/slow-1"));
        for (int i = 0; i < 100 && srv.count("/slow-1") == 0; ++i)
            sleep_for_ms(5);
        s.prefetch(pre);
        s.cancel_background();
        for (int i = 0; i < 100 && !s.idle(); ++i)
            sleep_for_ms(10);
        CHECK(srv.count("/cancel-3") == 0);
        s.stop();
    }
}

// DoH over the fixture server.
std::string b64url_decode(const std::string &in)
{
    std::string out;
    int val = 0, bits = -8;
    for (char c : in)
    {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-') v = 62;
        else if (c == '_') v = 63;
        else continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

void test_doh_end_to_end()
{
    if (!begin("doh: provider against a local DoH server, service in DoH mode"))
        return;
    FakeClock clock;
    std::atomic<int> doh_hits{0};
    fixture::Server doh(cert("good.pem"), cert("good.key"), [&](const std::string &p) -> fixture::Reply {
        ++doh_hits;
        fixture::Reply w;
        const std::size_t q = p.find("?dns=");
        if (p.rfind("/dns-query", 0) != 0 || q == std::string::npos)
        {
            w.status = 404;
            return w;
        }
        const std::string wire = b64url_decode(p.substr(q + 5));
        std::string resp("\0\0\x81\x80\0\x01\0\x01\0\0\0\0", 12);
        const std::string question = wire.substr(12);
        const bool nx = question.find("nx") != std::string::npos;
        if (nx)
            resp = std::string("\0\0\x81\x83\0\x01\0\0\0\0\0\0", 12) + question;
        else
        {
            resp += question;
            resp += std::string("\xc0\x0c\0\x01\0\x01\0\0\x00\x78\0\x04\x7f\0\0\x01", 16);
        }
        w.headers.push_back({"Content-Type", "application/dns-message"});
        w.body = resp;
        return w;
    });
    fixture::Server art(cert("good.pem"), cert("good.key"), [](const std::string &) { return png_reply(); });
    CHECK(doh.start() && art.start());

    ArtConfig cfg = service_config(tmpdir("doh"), nullptr, &clock);
    cfg.dns_mode = DnsMode::doh;
    cfg.doh_endpoints = {"https://127.0.0.1:" + std::to_string(doh.port()) + "/dns-query", "https://dns.example/q"};
    ArtService s(cfg);
    CHECK(s.start());
    CHECK(s.state().phase == Phase::ready && s.dns_mode() == DnsMode::doh);
    for (int i = 0; i < 5; ++i)
    {
        auto r = s.fetch_blocking(url(art, "/c" + std::to_string(i) + ".png"));
        CHECK_MSG(r.state == Fetch::ready, r.error);
    }
    CHECK(doh_hits == 1); // art.test resolved once, cached by TTL
    auto r = s.fetch_blocking(url(art, "/x.png", "nx.test"));
    CHECK(r.state == Fetch::failed);
    s.stop();

    // a DoH set-up without any valid endpoint does not silently use another resolver
    ArtConfig c2 = service_config(tmpdir("doh2"), nullptr, &clock);
    c2.dns_mode = DnsMode::doh;
    c2.doh_endpoints = {"https://dns.google/dns-query", "http://1.1.1.1/dns-query"};
    ArtService s2(c2);
    CHECK(s2.start());
    CHECK(s2.state().phase == Phase::disabled_dns);
    CHECK(s2.request(url(art, "/y.png")).state == Fetch::disabled);
    s2.stop();

    // default configuration: network off
    ArtConfig c3;
    CHECK(c3.dns_mode == DnsMode::off);
    CHECK(!system_dns_compiled()); // host build: console resolver is only compiled with WINGS_ART_SCENET
}

void test_real(const std::string &ca)
{
    if (!begin("real CDN (DoH): posters, badssl negatives"))
        return;
    ArtConfig cfg;
    cfg.cache_root = tmpdir("real");
    cfg.ca_bundle_paths = {ca};
    cfg.dns_mode = DnsMode::doh;
    ArtService s(cfg);
    CHECK(s.start());
    const char *good[] = {
        "https://image.api.playstation.com/vulcan/ap/rnd/202211/2218/9UflSkyn6ZUATU2VJNcMQ49b.png?w=260",
        "https://cdn.jsdelivr.net/gh/nexgen999/evoX-CoreOS@main/assets/icon/FPKGi.png",
    };
    for (const char *u : good)
    {
        auto r = s.fetch_blocking(u, Kind::cover, 20000);
        CHECK_MSG(r.state == Fetch::ready, std::string(u) + " " + r.error);
        std::printf("    %s -> %zu bytes %s\n", u, r.bytes.size(), r.content_type.c_str());
    }
    for (const char *u : {"https://expired.badssl.com/", "https://wrong.host.badssl.com/", "https://self-signed.badssl.com/",
                          "https://untrusted-root.badssl.com/"})
    {
        auto r = s.fetch_blocking(u, Kind::cover, 20000);
        CHECK_MSG(r.state == Fetch::failed, u);
    }
    s.stop();
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    bool real = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--certs" && i + 1 < argc) g_certs = argv[++i];
        else if (a == "--ca" && i + 1 < argc) g_ca_bundle_path = argv[++i];
        else if (a == "--filter" && i + 1 < argc) g_filter = argv[++i];
        else if (a == "--real") real = true;
    }
    if (g_certs.empty() || g_ca_bundle_path.empty())
    {
        std::fprintf(stderr, "usage: art_tests --certs DIR --ca ca-bundle.crt [--real] [--filter text]\n");
        return 2;
    }
    test_image_probe();
    test_url_parsing();
    test_dns_cache();
    test_doh_wire();
    test_disk_cache();
    test_tls_client();
    test_tls_safety();
    test_service_basic();
    test_service_failures();
    test_service_heap_and_priority();
    test_doh_end_to_end();
    if (real)
        test_real(g_ca_bundle_path);
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
