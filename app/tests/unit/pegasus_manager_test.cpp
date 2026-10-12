// Wings TV app - Pegasus DL manager: release JSON, URL rules, digest, versions, cache time, advice,
// the download state machine and the loopback hand-off. No test touches the real network.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/i18n.hpp"
#include "orbit/pegasus_manager.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace orbit
{
namespace
{

// The shape the official releases API returns (verified 2026-10-12), trimmed to the fields we read.
const char *kFixture = R"([
 {"tag_name":"v1.11.0","published_at":"2026-09-30T12:00:00Z","draft":false,"prerelease":false,
  "assets":[{"name":"pegasus_dl.elf","size":20971520,
   "digest":"sha256:00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF",
   "browser_download_url":"https://github.com/pegasus-ps5/pegasus-dl/releases/download/v1.11.0/pegasus_dl.elf"}]},
 {"tag_name":"v1.10.2","published_at":"2026-08-01T08:00:00Z","draft":false,"prerelease":false,
  "assets":[{"name":"pegasus_dl.elf","size":19000000,
   "digest":"sha256:ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100",
   "browser_download_url":"https://github.com/pegasus-ps5/pegasus-dl/releases/download/v1.10.2/pegasus_dl.elf"}]},
 {"tag_name":"v2.0.0","draft":false,"prerelease":true,"assets":[]},
 {"tag_name":"v1.12.0","draft":true,"assets":[]}
])";

std::string make_fixture(const std::string &tag, std::size_t size, const std::string &hex,
                         const std::string &url_override = {})
{
    const std::string url =
        url_override.empty()
            ? "https://github.com/pegasus-ps5/pegasus-dl/releases/download/" + tag + "/pegasus_dl.elf"
            : url_override;
    return "[{\"tag_name\":\"" + tag +
           "\",\"published_at\":\"2026-10-01T00:00:00Z\",\"draft\":false,"
           "\"prerelease\":false,\"assets\":[{\"name\":\"pegasus_dl.elf\",\"size\":" +
           std::to_string(size) + ",\"digest\":\"sha256:" + hex + "\",\"browser_download_url\":\"" + url +
           "\"}]}]";
}

TEST(PegasusRelease, ParsesTheOfficialShape)
{
    std::vector<PegasusRelease> list;
    std::string error;
    ASSERT_TRUE(parse_pegasus_releases(kFixture, &list, &error)) << error;
    ASSERT_EQ(list.size(), 3u); // the draft is skipped
    EXPECT_EQ(list[0].tag, "v1.11.0");
    EXPECT_EQ(list[0].size, 20971520u);
    EXPECT_EQ(list[0].sha256, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    EXPECT_TRUE(list[0].downloadable);
    EXPECT_TRUE(list[2].prerelease);
    EXPECT_FALSE(list[2].downloadable); // no asset
    const PegasusRelease *latest = pick_latest_pegasus_release(list);
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->tag, "v1.11.0"); // not the release candidate
}

TEST(PegasusRelease, RejectsDownloadsThatAreNotTheOfficialPattern)
{
    const std::string hex(64, 'a');
    std::vector<PegasusRelease> list;
    ASSERT_TRUE(parse_pegasus_releases(make_fixture("v1.11.0", 100, hex, "https://example.com/pegasus_dl.elf"),
                                       &list, nullptr));
    ASSERT_EQ(list.size(), 1u);
    EXPECT_FALSE(list[0].downloadable);
    ASSERT_TRUE(parse_pegasus_releases(make_fixture("v1.11.0", 100, "abc"), &list, nullptr));
    EXPECT_FALSE(list[0].downloadable); // short digest
    ASSERT_TRUE(parse_pegasus_releases(make_fixture("v1.11.0", 65u << 20, hex), &list, nullptr));
    EXPECT_FALSE(list[0].downloadable); // over the 64 MiB cap
    EXPECT_FALSE(parse_pegasus_releases("{\"a\":1}", &list, nullptr));
    EXPECT_FALSE(parse_pegasus_releases("not json", &list, nullptr));
}

TEST(PegasusUrl, FirstUrlIsExactlyTheOfficialOne)
{
    const std::string url = pegasus_download_url("v1.11.0");
    EXPECT_EQ(url, "https://github.com/pegasus-ps5/pegasus-dl/releases/download/v1.11.0/pegasus_dl.elf");
    EXPECT_TRUE(pegasus_url_allowed(url, PegasusUrlRole::first, "v1.11.0"));
    EXPECT_FALSE(pegasus_url_allowed(url, PegasusUrlRole::first, "v1.10.0"));
    EXPECT_FALSE(pegasus_url_allowed(url + "?x=1", PegasusUrlRole::first, "v1.11.0"));
    EXPECT_FALSE(pegasus_url_allowed(
        "http://github.com/pegasus-ps5/pegasus-dl/releases/download/v1.11.0/pegasus_dl.elf",
        PegasusUrlRole::first, "v1.11.0"));
    EXPECT_EQ(pegasus_download_url("v1.11"), "");
    EXPECT_EQ(pegasus_download_url("../etc"), "");
    EXPECT_EQ(pegasus_download_url("v1.11.0/../../x"), "");
}

TEST(PegasusUrl, RedirectsStayOnGithubHosts)
{
    const auto ok = [](const char *u) { return pegasus_url_allowed(u, PegasusUrlRole::redirect); };
    EXPECT_TRUE(ok("https://objects.githubusercontent.com/github-production-release-asset/1?X-Amz=1"));
    EXPECT_TRUE(ok("https://release-assets.githubusercontent.com/x/y"));
    EXPECT_TRUE(ok("https://github.com/a/b"));
    EXPECT_TRUE(ok("HTTPS://GitHub.com:443/a"));
    EXPECT_FALSE(ok("http://objects.githubusercontent.com/x"));
    EXPECT_FALSE(ok("https://evil.example/x"));
    EXPECT_FALSE(ok("https://github.com.evil.example/x"));
    EXPECT_FALSE(ok("https://evilgithub.com/x"));
    EXPECT_FALSE(ok("https://github.com@evil.example/x"));
    EXPECT_FALSE(ok("https://user:pw@github.com/x"));
    EXPECT_FALSE(ok("https://github.com:8443/x"));
    EXPECT_FALSE(ok("https://github.com\\@evil.example/x"));
    EXPECT_FALSE(ok("https://127.0.0.1/x"));
    EXPECT_FALSE(ok("https://github.com/a b"));
    EXPECT_FALSE(ok(""));
}

TEST(PegasusDigest, Sha256KnownVectorsAndCompare)
{
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Streaming in odd pieces equals one shot (block boundaries).
    const std::string big(1000, 'x');
    Sha256 h;
    h.update(big.data(), 3);
    h.update(big.data() + 3, 61);
    h.update(big.data() + 64, 936);
    EXPECT_EQ(h.finish_hex(), sha256_hex(big));
    std::string hex;
    EXPECT_TRUE(
        parse_sha256_digest("sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789", &hex));
    EXPECT_EQ(hex, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    EXPECT_FALSE(
        parse_sha256_digest("sha1:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", &hex));
    EXPECT_FALSE(parse_sha256_digest("sha256:zz", &hex));
    EXPECT_FALSE(parse_sha256_digest("sha256:" + std::string(64, 'g'), &hex));
}

TEST(PegasusVersion, CompareAndAdvice)
{
    bool ok = false;
    EXPECT_EQ(compare_pegasus_versions("v1.11.0", "1.10.9", &ok), 1);
    EXPECT_TRUE(ok);
    EXPECT_EQ(compare_pegasus_versions("1.9.0", "v1.11.0", &ok), -1);
    EXPECT_EQ(compare_pegasus_versions("1.11", "v1.11.0", &ok), 0);
    compare_pegasus_versions("dev", "v1.11.0", &ok);
    EXPECT_FALSE(ok);

    EXPECT_EQ(pegasus_advice(false, "", "v1.11.0"), PegasusAdvice::not_running);
    EXPECT_EQ(pegasus_advice(true, "1.11.0", ""), PegasusAdvice::not_checked);
    EXPECT_EQ(pegasus_advice(true, "1.11.0", "v1.11.0"), PegasusAdvice::up_to_date);
    EXPECT_EQ(pegasus_advice(true, "1.12.0", "v1.11.0"), PegasusAdvice::up_to_date);
    EXPECT_EQ(pegasus_advice(true, "1.10.0", "v1.11.0"), PegasusAdvice::newer_tested); // inside the range
    EXPECT_EQ(pegasus_advice(true, "1.11.0", "v1.12.0"), PegasusAdvice::newer_untested);
    EXPECT_EQ(pegasus_advice(true, "weird", "v1.11.0"), PegasusAdvice::unreadable);
    EXPECT_EQ(pegasus_advice_text(PegasusAdvice::up_to_date), "Up to date");
    EXPECT_EQ(pegasus_advice_text(PegasusAdvice::newer_untested), "Newer release available, untested with Wings");
    EXPECT_EQ(pegasus_advice_text(PegasusAdvice::not_running), "Not running");
}

TEST(PegasusCache, DueAfterSixHoursOrOnDemand)
{
    const std::int64_t t = 1'800'000'000;
    EXPECT_TRUE(pegasus_check_due(0, t, false));
    EXPECT_FALSE(pegasus_check_due(t, t + 60, false));
    EXPECT_FALSE(pegasus_check_due(t, t + 6 * 3600 - 1, false));
    EXPECT_TRUE(pegasus_check_due(t, t + 6 * 3600, false));
    EXPECT_TRUE(pegasus_check_due(t, t + 60, true)); // the player pressed Check
    EXPECT_TRUE(pegasus_check_due(t, t - 5, false)); // clock went backwards
}

TEST(PegasusCache, RoundTripsAndRevalidates)
{
    std::vector<PegasusRelease> list;
    ASSERT_TRUE(parse_pegasus_releases(kFixture, &list, nullptr));
    const std::string payload = encode_pegasus_cache(1234567, list);
    std::int64_t at = 0;
    std::vector<PegasusRelease> back;
    ASSERT_TRUE(decode_pegasus_cache(payload, &at, &back));
    EXPECT_EQ(at, 1234567);
    ASSERT_EQ(back.size(), list.size());
    EXPECT_EQ(back[0].tag, "v1.11.0");
    EXPECT_TRUE(back[0].downloadable);
    EXPECT_EQ(back[0].sha256, list[0].sha256);
    // A damaged line is dropped, and a tag with a path in it never loads.
    ASSERT_TRUE(decode_pegasus_cache(
        "checked=5\nr=../x|a|10|" + std::string(64, 'a') + "|0\nr=v1.0.0|a|zz|q|0\n", &at, &back));
    ASSERT_EQ(back.size(), 1u);
    EXPECT_FALSE(back[0].downloadable);
    EXPECT_FALSE(decode_pegasus_cache("r=v1.0.0|a|10|" + std::string(64, 'a') + "|0\n", &at, &back)); // no time
}

// ---- state machine ----

class FakeDocs final : public DocFetcher
{
  public:
    DocResult get(const std::string &url, std::size_t) override
    {
        urls.push_back(url);
        return result;
    }
    DocResult result;
    std::vector<std::string> urls;
};

class FakeDownloader final : public PegasusDownloader
{
  public:
    bool ready(std::string *why) override
    {
        if (!open)
            *why = "off";
        return open;
    }
    PegasusDownloadResult get(const std::string &url, std::size_t max_bytes,
                              const PegasusDownloadHooks &hooks) override
    {
        urls.push_back(url);
        caps.push_back(max_bytes);
        PegasusDownloadResult r;
        if (hooks.redirect_ok)
        {
            first_redirect_ok = hooks.redirect_ok("https://objects.githubusercontent.com/x");
            bad_redirect_ok = hooks.redirect_ok("https://evil.example/x");
        }
        if (hooks.progress)
            hooks.progress(body.size());
        r.status = 200;
        r.body.assign(body.begin(), body.end());
        return r;
    }
    bool open = true;
    std::string body;
    std::vector<std::string> urls;
    std::vector<std::size_t> caps;
    bool first_redirect_ok = false, bad_redirect_ok = true;
};

struct Rig
{
    Rig()
    {
        char tmpl[] = "/tmp/wings_pm_XXXXXX";
        const char *made = mkdtemp(tmpl);
        root = made != nullptr ? made : "/tmp";
        elf = std::string("\x7f") + "ELF" + std::string(200, 'z');
    }
    ~Rig()
    {
        manager.wait_idle();
        const std::string cmd = "rm -rf " + root;
        if (std::system(cmd.c_str()) != 0)
            std::fprintf(stderr, "cleanup failed\n");
    }
    void attach(const std::string &json)
    {
        docs.result.status = 200;
        docs.result.body = json;
        net.body = elf;
        PegasusManagerConfig c;
        c.docs = &docs;
        c.downloader = &net;
        c.root = root + "/pegasus";
        c.cache_paths = {root + "/releases.v1"};
        c.now = [this] { return clock; };
        manager.attach(std::move(c));
    }
    std::string root, elf;
    FakeDocs docs;
    FakeDownloader net;
    PegasusManager manager;
    std::int64_t clock = 1'800'000'000;
};

bool exists(const std::string &p)
{
    FILE *f = std::fopen(p.c_str(), "rb");
    if (f != nullptr)
        std::fclose(f);
    return f != nullptr;
}

TEST(PegasusManager, CheckThenDownloadVerifiesAndRenames)
{
    Rig rig;
    rig.attach(make_fixture("v1.11.0", rig.elf.size(), sha256_hex(rig.elf)));
    EXPECT_FALSE(rig.manager.status().have_latest);
    EXPECT_FALSE(rig.manager.download_latest()); // nothing known yet

    ASSERT_TRUE(rig.manager.check_now());
    rig.manager.wait_idle();
    PegasusStatus st = rig.manager.status();
    ASSERT_TRUE(st.have_latest) << st.check_error;
    EXPECT_EQ(st.latest.tag, "v1.11.0");
    EXPECT_EQ(st.checked_at, rig.clock);
    ASSERT_EQ(rig.docs.urls.size(), 1u);
    EXPECT_EQ(rig.docs.urls[0], kPegasusReleasesUrl);
    EXPECT_FALSE(st.downloaded);

    ASSERT_TRUE(rig.manager.download_latest());
    rig.manager.wait_idle();
    st = rig.manager.status();
    EXPECT_TRUE(st.downloaded) << st.message;
    EXPECT_FALSE(st.message_is_error);
    const std::string path = rig.root + "/pegasus/v1.11.0/pegasus_dl.elf";
    EXPECT_EQ(st.path, path);
    EXPECT_TRUE(exists(path));
    EXPECT_FALSE(exists(path + ".part"));
    ASSERT_EQ(rig.net.urls.size(), 1u);
    EXPECT_EQ(rig.net.urls[0], pegasus_download_url("v1.11.0"));
    EXPECT_EQ(rig.net.caps[0], kPegasusDownloadCap);
    EXPECT_TRUE(rig.net.first_redirect_ok);
    EXPECT_FALSE(rig.net.bad_redirect_ok);

    // A new run finds the saved list and the verified file by itself.
    PegasusManager again;
    PegasusManagerConfig c;
    c.docs = &rig.docs;
    c.downloader = &rig.net;
    c.root = rig.root + "/pegasus";
    c.cache_paths = {rig.root + "/releases.v1"};
    c.now = [&rig] { return rig.clock + 60; };
    again.attach(std::move(c));
    const PegasusStatus st2 = again.status();
    EXPECT_TRUE(st2.have_latest);
    EXPECT_TRUE(st2.downloaded);
    EXPECT_EQ(st2.checked_at, rig.clock);
    again.auto_check(); // fresh: no second request
    again.wait_idle();
    EXPECT_EQ(rig.docs.urls.size(), 1u);
}

TEST(PegasusManager, DigestMismatchSavesNothing)
{
    Rig rig;
    rig.attach(make_fixture("v1.11.0", rig.elf.size(), std::string(64, 'a')));
    ASSERT_TRUE(rig.manager.check_now());
    rig.manager.wait_idle();
    ASSERT_TRUE(rig.manager.download_latest());
    rig.manager.wait_idle();
    const PegasusStatus st = rig.manager.status();
    EXPECT_FALSE(st.downloaded);
    EXPECT_TRUE(st.message_is_error);
    EXPECT_FALSE(st.message.empty());
    const std::string path = rig.root + "/pegasus/v1.11.0/pegasus_dl.elf";
    EXPECT_FALSE(exists(path));
    EXPECT_FALSE(exists(path + ".part"));
}

TEST(PegasusManager, WrongSizeOrNotElfIsRefused)
{
    Rig rig;
    rig.attach(make_fixture("v1.11.0", rig.elf.size() + 1, sha256_hex(rig.elf)));
    ASSERT_TRUE(rig.manager.check_now());
    rig.manager.wait_idle();
    ASSERT_TRUE(rig.manager.download_latest());
    rig.manager.wait_idle();
    EXPECT_FALSE(rig.manager.status().downloaded);

    Rig other;
    const std::string html(200, 'h');
    other.attach(make_fixture("v1.11.0", html.size(), sha256_hex(html)));
    other.net.body = html; // right size and digest, but not an ELF
    ASSERT_TRUE(other.manager.check_now());
    other.manager.wait_idle();
    ASSERT_TRUE(other.manager.download_latest());
    other.manager.wait_idle();
    EXPECT_FALSE(other.manager.status().downloaded);
    EXPECT_TRUE(other.manager.status().message_is_error);
}

TEST(PegasusManager, ClosedNetworkGateBlocksEverything)
{
    Rig rig;
    rig.attach(make_fixture("v1.11.0", rig.elf.size(), sha256_hex(rig.elf)));
    rig.net.open = false;
    EXPECT_FALSE(rig.manager.status().net_ok);
    rig.manager.auto_check();
    rig.manager.wait_idle();
    EXPECT_TRUE(rig.docs.urls.empty());
    EXPECT_FALSE(rig.manager.check_now());
    EXPECT_TRUE(rig.docs.urls.empty());
    EXPECT_FALSE(rig.manager.status().check_error.empty());
}

TEST(PegasusManager, FailedCheckIsNotRetriedEveryFrameAndKeepsTheOldList)
{
    Rig rig;
    rig.attach(make_fixture("v1.11.0", rig.elf.size(), sha256_hex(rig.elf)));
    rig.manager.auto_check();
    rig.manager.wait_idle();
    ASSERT_TRUE(rig.manager.status().have_latest);
    rig.clock += 7 * 3600; // stale again
    rig.docs.result.status = 500;
    rig.manager.auto_check();
    rig.manager.wait_idle();
    EXPECT_EQ(rig.docs.urls.size(), 2u);
    EXPECT_FALSE(rig.manager.status().check_error.empty());
    EXPECT_TRUE(rig.manager.status().have_latest); // the saved list stays
    rig.clock += 30;
    rig.manager.auto_check();
    rig.manager.wait_idle();
    EXPECT_EQ(rig.docs.urls.size(), 2u); // not within ten minutes
    rig.manager.check_now();             // the player may always ask
    rig.manager.wait_idle();
    EXPECT_EQ(rig.docs.urls.size(), 3u);
}

TEST(PegasusManager, SendIsRefusedWithoutADownloadOrWhilePegasusAnswers)
{
    Rig rig;
    rig.attach(make_fixture("v1.11.0", rig.elf.size(), sha256_hex(rig.elf)));
    EXPECT_FALSE(rig.manager.send_to_loader(false)); // nothing downloaded
    ASSERT_TRUE(rig.manager.check_now());
    rig.manager.wait_idle();
    ASSERT_TRUE(rig.manager.download_latest());
    rig.manager.wait_idle();
    EXPECT_FALSE(rig.manager.send_to_loader(true)); // Pegasus answers: nothing to send
}

// ---- hand-off ----

int listen_loopback(std::uint16_t *port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001u);
    a.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(fd, 1) != 0)
        return -1;
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
    *port = ntohs(a.sin_port);
    return fd;
}

TEST(PegasusSend, StreamsTheFileToALoopbackListener)
{
    Rig rig;
    const std::string path = rig.root + "/payload.elf";
    std::string content = std::string("\x7f") + "ELF";
    for (int i = 0; i < 5000; ++i)
        content.push_back(static_cast<char>(i * 7));
    FILE *f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);

    std::uint16_t port = 0;
    const int server = listen_loopback(&port);
    ASSERT_GE(server, 0);
    std::string error;
    EXPECT_TRUE(send_file_to_loopback("127.0.0.1", port, path, nullptr, &error)) << error;
    const int conn = accept(server, nullptr, nullptr);
    ASSERT_GE(conn, 0);
    std::string got;
    char buf[4096];
    for (;;)
    {
        const ssize_t n = recv(conn, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        got.append(buf, static_cast<std::size_t>(n));
    }
    close(conn);
    close(server);
    EXPECT_EQ(got, content);
}

TEST(PegasusSend, SaysSoWhenTheLoaderIsNotThereAndRefusesOtherHosts)
{
    Rig rig;
    const std::string path = rig.root + "/payload.elf";
    FILE *f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("x", f);
    std::fclose(f);
    std::uint16_t port = 0;
    const int server = listen_loopback(&port);
    ASSERT_GE(server, 0);
    close(server); // nothing listens on it now
    std::string error;
    EXPECT_FALSE(send_file_to_loopback("127.0.0.1", port, path, nullptr, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(send_file_to_loopback("192.168.1.5", 9021, path, nullptr, &error));
    EXPECT_FALSE(send_file_to_loopback("127.0.0.1", port, rig.root + "/missing", nullptr, &error));
}

} // namespace
} // namespace orbit
