// Wings TV app - streaming download to disk: limits, free-space rule, UFS2 magic, ChunkSink and the
// whole stream_download_to_file flow against a fake downloader. No test touches the network.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_manager.hpp"
#include "orbit/stream_download.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

std::vector<std::uint8_t> make_image(std::size_t size, bool magic = true)
{
    std::vector<std::uint8_t> v(size);
    for (std::size_t i = 0; i < size; ++i)
        v[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
    if (magic)
    {
        v[kUfs2MagicOffset + 0] = 0x19;
        v[kUfs2MagicOffset + 1] = 0x01;
        v[kUfs2MagicOffset + 2] = 0x54;
        v[kUfs2MagicOffset + 3] = 0x19;
    }
    else
    {
        v[kUfs2MagicOffset] = 0;
    }
    return v;
}

std::string hex_of(const std::vector<std::uint8_t> &v)
{
    return sha256_hex(std::string_view(reinterpret_cast<const char *>(v.data()), v.size()));
}

bool exists(const std::string &p)
{
    return std::filesystem::exists(p);
}

std::string slurp(const std::string &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

class StreamDir : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dir_ = (std::filesystem::temp_directory_path() / ("wings-stream-" + std::to_string(::getpid()))).string();
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        final_ = dir_ + "/PPSA99178.ffpkg";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::string dir_, final_;
};

// Hands out `body` in `piece`-byte chunks, like the TLS client does (never more than 16 KiB).
class FakeDownloader final : public StreamingDownloader
{
  public:
    bool ready(std::string *) override { return true; }
    std::string get_stream(const std::string &url, std::uint64_t max_bytes, const StreamHooks &hooks,
                           const std::function<bool(const std::uint8_t *, std::size_t)> &sink,
                           int *status) override
    {
        last_url = url;
        last_max = max_bytes;
        if (hooks.redirect_ok)
            redirect_gate = hooks.redirect_ok;
        *status = http_status;
        if (!error.empty() && fail_after == 0)
            return error;
        if (http_status < 200 || http_status > 299)
            return {};
        std::size_t off = 0;
        while (off < body.size())
        {
            const std::size_t k = std::min(piece, body.size() - off);
            if (fail_after != 0 && off >= fail_after)
                return error;
            if (!sink(body.data() + off, k))
                return "sink";
            off += k;
        }
        return {};
    }
    std::vector<std::uint8_t> body;
    std::size_t piece = 4093; // deliberately not a divisor of anything
    int http_status = 200;
    std::string error;
    std::size_t fail_after = 0;
    std::string last_url;
    std::uint64_t last_max = 0;
    std::function<bool(const std::string &)> redirect_gate;
};

StreamRequest request_for(const std::vector<std::uint8_t> &img, const std::string &path)
{
    StreamRequest r;
    r.url = "https://github.com/x/y/releases/download/v1/PPSA99178.ffpkg";
    r.final_path = path;
    r.size = img.size();
    r.sha256 = hex_of(img);
    return r;
}

TEST(StreamRules, UfsMagic)
{
    const std::uint8_t good[4] = {0x19, 0x01, 0x54, 0x19};
    const std::uint8_t reversed[4] = {0x19, 0x54, 0x01, 0x19};
    EXPECT_TRUE(ufs2_magic_ok(good));
    EXPECT_FALSE(ufs2_magic_ok(reversed));
    EXPECT_FALSE(ufs2_magic_ok(nullptr));
    EXPECT_EQ(kUfs2MagicOffset, 65536u + 1372u);
}

TEST(StreamRules, SizeAndSpace)
{
    EXPECT_FALSE(stream_size_ok(0));
    EXPECT_FALSE(stream_size_ok(kUfs2MagicOffset + 3));
    EXPECT_TRUE(stream_size_ok(kUfs2MagicOffset + 4));
    EXPECT_TRUE(stream_size_ok(54460416));
    EXPECT_TRUE(stream_size_ok(kStreamMaxImage));
    EXPECT_FALSE(stream_size_ok(kStreamMaxImage + 1));

    const std::uint64_t want = 54460416;
    const double need = static_cast<double>(want + kStreamSpare);
    EXPECT_TRUE(stream_space_ok(-1.0, want)); // unknown does not block
    EXPECT_TRUE(stream_space_ok(need, want));
    EXPECT_FALSE(stream_space_ok(need - 1.0, want));
    EXPECT_FALSE(stream_space_ok(0.0, want));
}

TEST(StreamRules, RedirectPin)
{
    EXPECT_TRUE(stream_redirect_allowed("https://github.com/a/b/releases/download/v1/f"));
    EXPECT_TRUE(stream_redirect_allowed("https://release-assets.githubusercontent.com/x?y=1"));
    EXPECT_FALSE(stream_redirect_allowed("http://github.com/a"));
    EXPECT_FALSE(stream_redirect_allowed("https://github.com.evil.example/a"));
    EXPECT_FALSE(stream_redirect_allowed("https://evil.example/github.com"));
    EXPECT_FALSE(stream_redirect_allowed("https://user@github.com/a"));
}

TEST_F(StreamDir, SinkWritesHashesAndSeesMagicAcrossChunks)
{
    const auto img = make_image(70000);
    ChunkSink::Config cfg;
    cfg.part_path = final_ + ".part";
    cfg.limit = img.size();
    std::uint64_t last = 0;
    bool monotonic = true;
    cfg.progress = [&](std::uint64_t b)
    {
        monotonic = monotonic && b > last;
        last = b;
    };
    ChunkSink sink(cfg);
    ASSERT_TRUE(sink.open());
    // 1 byte at a time around the magic, big pieces elsewhere.
    std::size_t off = 0;
    while (off < img.size())
    {
        const bool near = off + 8 >= kUfs2MagicOffset && off < kUfs2MagicOffset + 8;
        const std::size_t k = std::min<std::size_t>(near ? 1 : 3000, img.size() - off);
        ASSERT_TRUE(sink.write(img.data() + off, k));
        off += k;
    }
    std::string hex;
    bool ufs2 = false;
    ASSERT_TRUE(sink.finish(&hex, &ufs2));
    EXPECT_EQ(hex, hex_of(img));
    EXPECT_TRUE(ufs2);
    EXPECT_TRUE(monotonic);
    EXPECT_EQ(last, img.size());
    EXPECT_EQ(sink.bytes(), img.size());
    EXPECT_EQ(slurp(cfg.part_path), std::string(img.begin(), img.end()));
}

TEST_F(StreamDir, SinkLimitCancelAndAbortDeleteThePartFile)
{
    const auto img = make_image(70000);
    const std::string part = final_ + ".part";
    {
        ChunkSink::Config cfg;
        cfg.part_path = part;
        cfg.limit = 1000;
        ChunkSink sink(cfg);
        ASSERT_TRUE(sink.open());
        EXPECT_TRUE(exists(part));
        EXPECT_TRUE(sink.write(img.data(), 600));
        EXPECT_FALSE(sink.write(img.data(), 600)); // 1200 > 1000
        EXPECT_EQ(sink.failed(), StreamError::too_large);
        EXPECT_FALSE(exists(part));
    }
    {
        ChunkSink::Config cfg;
        cfg.part_path = part;
        cfg.limit = img.size();
        bool stop = false;
        cfg.cancel = [&] { return stop; };
        ChunkSink sink(cfg);
        ASSERT_TRUE(sink.open());
        EXPECT_TRUE(sink.write(img.data(), 100));
        stop = true;
        EXPECT_FALSE(sink.write(img.data(), 100));
        EXPECT_EQ(sink.failed(), StreamError::cancelled);
        EXPECT_FALSE(exists(part));
    }
    {
        ChunkSink::Config cfg;
        cfg.part_path = part;
        cfg.limit = img.size();
        {
            ChunkSink sink(cfg);
            ASSERT_TRUE(sink.open());
            EXPECT_TRUE(sink.write(img.data(), 100));
            EXPECT_TRUE(exists(part));
        } // destroyed without finish(): the work file goes
        EXPECT_FALSE(exists(part));
    }
    {
        ChunkSink::Config cfg;
        cfg.part_path = dir_ + "/no-such-dir/x.part";
        cfg.limit = 10;
        ChunkSink sink(cfg);
        EXPECT_FALSE(sink.open());
        EXPECT_EQ(sink.failed(), StreamError::disk);
    }
}

TEST_F(StreamDir, FlowSuccessRenamesAndReplaces)
{
    const auto img = make_image(70000);
    {
        std::ofstream old(final_, std::ios::binary);
        old << "old";
    }
    FakeDownloader dl;
    dl.body = img;
    std::uint64_t progress = 0;
    StreamRequest req = request_for(img, final_);
    req.hooks.progress = [&](std::uint64_t b) { progress = b; };
    req.free_bytes = [](const std::string &) { return 1e12; };
    const StreamResult r = stream_download_to_file(dl, req);
    ASSERT_TRUE(r.ok) << static_cast<int>(r.error);
    EXPECT_EQ(r.bytes, img.size());
    EXPECT_EQ(r.sha256, req.sha256);
    EXPECT_EQ(progress, img.size());
    EXPECT_EQ(slurp(final_), std::string(img.begin(), img.end()));
    EXPECT_FALSE(exists(final_ + ".part"));
    EXPECT_EQ(dl.last_max, img.size() + 1);
    // Redirects are pinned to GitHub by default.
    ASSERT_TRUE(static_cast<bool>(dl.redirect_gate));
    EXPECT_TRUE(dl.redirect_gate("https://release-assets.githubusercontent.com/a"));
    EXPECT_FALSE(dl.redirect_gate("https://example.com/a"));
}

TEST_F(StreamDir, FlowFailuresLeaveNothingBehind)
{
    const auto img = make_image(70000);
    auto expect_fail = [&](FakeDownloader &dl, StreamRequest req, StreamError want)
    {
        const StreamResult r = stream_download_to_file(dl, req);
        EXPECT_FALSE(r.ok);
        EXPECT_EQ(r.error, want);
        EXPECT_FALSE(exists(final_));
        EXPECT_FALSE(exists(final_ + ".part"));
    };
    {
        FakeDownloader dl;
        dl.body = img;
        StreamRequest req = request_for(img, final_);
        req.sha256 = std::string(64, 'a');
        expect_fail(dl, req, StreamError::checksum);
    }
    {
        FakeDownloader dl;
        dl.body = make_image(70000, false);
        expect_fail(dl, request_for(dl.body, final_), StreamError::not_image);
    }
    {
        FakeDownloader dl;
        dl.body = img;
        dl.body.resize(69000); // fewer bytes than listed
        expect_fail(dl, request_for(img, final_), StreamError::size_mismatch);
    }
    {
        FakeDownloader dl;
        dl.body = img;
        dl.body.resize(70500); // more bytes than listed
        expect_fail(dl, request_for(img, final_), StreamError::too_large);
    }
    {
        FakeDownloader dl;
        dl.http_status = 404;
        const StreamResult r = stream_download_to_file(dl, request_for(img, final_));
        EXPECT_EQ(r.error, StreamError::http_status);
        EXPECT_EQ(r.status, 404);
        EXPECT_FALSE(exists(final_ + ".part"));
    }
    {
        FakeDownloader dl;
        dl.body = img;
        dl.error = "io (truncated body)";
        dl.fail_after = 20000; // dies mid-stream
        const StreamResult r = stream_download_to_file(dl, request_for(img, final_));
        EXPECT_EQ(r.error, StreamError::network);
        EXPECT_NE(r.detail.find("truncated"), std::string::npos);
        EXPECT_GT(r.bytes, 0u);
        EXPECT_FALSE(exists(final_ + ".part"));
    }
    {
        FakeDownloader dl;
        dl.body = img;
        StreamRequest req = request_for(img, final_);
        req.free_bytes = [](const std::string &) { return 1000.0; };
        expect_fail(dl, req, StreamError::no_space);
        EXPECT_TRUE(dl.last_url.empty()); // refused before any connection
    }
    {
        FakeDownloader dl;
        dl.body = img;
        StreamRequest req = request_for(img, final_);
        int calls = 0;
        req.hooks.cancel = [&] { return ++calls > 5; };
        expect_fail(dl, req, StreamError::cancelled);
    }
    {
        FakeDownloader dl;
        StreamRequest req = request_for(img, final_);
        req.size = 100; // below the magic offset
        expect_fail(dl, req, StreamError::bad_request);
        req = request_for(img, final_);
        req.sha256 = "ABC";
        expect_fail(dl, req, StreamError::bad_request);
    }
}

TEST_F(StreamDir, FailureKeepsAnExistingFinalFile)
{
    const auto img = make_image(70000);
    {
        std::ofstream old(final_, std::ios::binary);
        old << "keep me";
    }
    FakeDownloader dl;
    dl.body = img;
    StreamRequest req = request_for(img, final_);
    req.sha256 = std::string(64, 'b');
    EXPECT_EQ(stream_download_to_file(dl, req).error, StreamError::checksum);
    EXPECT_EQ(slurp(final_), "keep me");
    EXPECT_FALSE(exists(final_ + ".part"));
}

} // namespace
} // namespace orbit
