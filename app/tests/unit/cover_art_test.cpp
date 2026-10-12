// Wings TV app - Cover art glue: poster index, preferences, diagnostics text,
// Settings > Cover art, ArtCache backpressure and the image dimension guard.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "fake_cover_art.hpp"
#include "orbit/cover_art.hpp"
#include "orbit/diagnostics.hpp"
#include "orbit/image.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include "../third_party/stb/stb_image_write.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace orbit
{
namespace
{

void append_png(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}

Package package(const std::string &id, const std::string &poster)
{
    Package p;
    p.id = id;
    p.title = "Title " + id;
    p.poster_url = poster;
    return p;
}

// ---- PosterIndex ----

TEST(PosterIndex, MapsPackageKeysToHttpsUrlsOnly)
{
    PosterIndex index;
    const std::vector<Package> packages = {
        package("1", "https://image.api.playstation.com/a.png?w=260"),
        package("2", "http://insecure.example/b.png"),
        package("3", ""),
        package("4", "https://cdn.jsdelivr.net/c.png"),
    };
    EXPECT_TRUE(index.sync(7, packages));
    EXPECT_EQ(index.size(), 2u);
    EXPECT_EQ(index.resolve("pkg:1", ArtKind::cover), "https://image.api.playstation.com/a.png?w=260");
    EXPECT_EQ(index.resolve("pkg:4", ArtKind::cover), "https://cdn.jsdelivr.net/c.png");
    // Unknown, not-https and empty: "" so the caller falls through to the backend.
    EXPECT_EQ(index.resolve("pkg:2", ArtKind::cover), "");
    EXPECT_EQ(index.resolve("pkg:3", ArtKind::cover), "");
    EXPECT_EQ(index.resolve("pkg:99", ArtKind::cover), "");
    // Installed games are not resolved here (the backend serves /appdb/icon).
    EXPECT_EQ(index.resolve("app:PPSA01234", ArtKind::cover), "");
    // Packages have no hero picture.
    EXPECT_EQ(index.resolve("pkg:1", ArtKind::hero), "");
}

TEST(PosterIndex, ReplacesOnlyWhenTheCatalogueSerialChanged)
{
    PosterIndex index;
    EXPECT_TRUE(index.sync(1, {package("1", "https://a.example/1.png")}));
    EXPECT_FALSE(index.sync(1, {package("1", "https://a.example/other.png")}));
    EXPECT_EQ(index.resolve("pkg:1", ArtKind::cover), "https://a.example/1.png");
    EXPECT_TRUE(index.sync(2, {package("5", "https://a.example/5.png")}));
    EXPECT_EQ(index.resolve("pkg:1", ArtKind::cover), "");
    EXPECT_EQ(index.resolve("pkg:5", ArtKind::cover), "https://a.example/5.png");
}

TEST(PosterIndex, IsSafeToReadFromAnotherThreadWhileItIsReplaced)
{
    PosterIndex index;
    std::vector<Package> packages;
    for (int i = 0; i < 500; ++i)
        packages.push_back(package(std::to_string(i), "https://a.example/" + std::to_string(i)));
    index.sync(1, packages);
    std::atomic<bool> stop{false};
    std::atomic<int> hits{0};
    std::thread reader(
        [&]
        {
            while (!stop.load())
            {
                if (!index.resolve("pkg:42", ArtKind::cover).empty())
                    hits.fetch_add(1);
            }
        });
    for (std::uint64_t serial = 2; serial < 60; ++serial)
        index.sync(serial, packages);
    stop.store(true);
    reader.join();
    EXPECT_GT(hits.load(), 0);
    EXPECT_EQ(index.resolve("pkg:42", ArtKind::cover), "https://a.example/42");
}

// ---- preferences ----

struct TempDir
{
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/wings-prefs-XXXXXX";
        const char *made = mkdtemp(name);
        path = made != nullptr ? made : "/tmp";
    }
    ~TempDir()
    {
        std::string command = "rm -rf '" + path + "'";
        if (std::system(command.c_str()) != 0)
        {
        }
    }
};

TEST(Prefs, DefaultIsOnAndTheChoiceSurvivesARestart)
{
    TempDir dir;
    const std::vector<std::string> paths = {dir.path + "/settings.v1"};
    WingsPrefs loaded;
    EXPECT_FALSE(load_prefs(paths, &loaded)); // no file: the default stays
    EXPECT_TRUE(loaded.art_doh);
    EXPECT_TRUE(WingsPrefs{}.art_doh);

    WingsPrefs on;
    on.art_doh = true;
    EXPECT_EQ(save_prefs(paths, on), 1);
    WingsPrefs back;
    ASSERT_TRUE(load_prefs(paths, &back));
    EXPECT_TRUE(back.art_doh);

    on.art_doh = false;
    EXPECT_EQ(save_prefs(paths, on), 1);
    ASSERT_TRUE(load_prefs(paths, &back));
    EXPECT_FALSE(back.art_doh);
}

TEST(Prefs, DamagedFileIsIgnoredAndTheNextPathIsUsed)
{
    TempDir dir;
    const std::string bad = dir.path + "/bad.v1";
    const std::string good = dir.path + "/good.v1";
    std::FILE *file = std::fopen(bad.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    std::fputs("HUIL garbage that fails the checksum", file);
    std::fclose(file);
    WingsPrefs on;
    on.art_doh = true;
    ASSERT_EQ(save_prefs({good}, on), 1);
    WingsPrefs out;
    ASSERT_TRUE(load_prefs({bad, good}, &out));
    EXPECT_TRUE(out.art_doh);
    WingsPrefs none;
    EXPECT_FALSE(load_prefs({bad}, &none));
    EXPECT_TRUE(none.art_doh);
}

TEST(Prefs, SaveSkipsFoldersThatCannotBeWritten)
{
    TempDir dir;
    WingsPrefs on;
    on.art_doh = true;
    EXPECT_EQ(save_prefs({"/proc/definitely/not/here/settings.v1", dir.path + "/ok.v1"}, on), 1);
}

// ---- diagnostics text ----

TEST(Diagnostics, FormatsUtcAndJudgesTheClock)
{
    EXPECT_EQ(format_utc(1760000000), "2025-10-09 08:53:20");
    EXPECT_EQ(format_utc(0), "unknown");
    EXPECT_EQ(format_utc(951782400), "2000-02-29 00:00:00"); // leap day
    EXPECT_FALSE(clock_plausible(0));
    EXPECT_FALSE(clock_plausible(946684800)); // 2000-01-01: a reset console
    EXPECT_TRUE(clock_plausible(1760000000));
    EXPECT_FALSE(clock_plausible(5000000000));
}

TEST(Diagnostics, SaysWhatIsOffAndWhatIsWrong)
{
    DiagnosticsInput in;
    in.app_version = "00.001.000";
    in.title_id = "PPSA99178";
    in.unix_time = 946684800; // wrong date
    in.link = "online";
    in.endpoint = "127.0.0.1:6970";
    in.catalogue_packages = 2962;
    in.poster_urls = 1084;
    in.art_present = true;
    in.art_phase = "off (DNS-over-HTTPS is not enabled)";
    in.art_detail = "Cover art is off.";
    in.dns_mode = "off";
    in.ca_certs = 121;
    in.ca_source = "/app0/assets/ca-bundle.crt";
    in.rand_status = 0;
    in.cache_dir = "/download0/wings/art";
    in.cache_dir_writable = false;
    in.written_to = {"/data/wings/diagnostics.txt"};
    in.recent_problems = {"[WINGS-ART] host x paused for 60 s"};
    const std::string text = format_diagnostics(in);
    EXPECT_NE(text.find("PPSA99178"), std::string::npos);
    EXPECT_NE(text.find("looks right:     NO"), std::string::npos);
    EXPECT_NE(text.find("DNS-over-HTTPS is OFF"), std::string::npos);
    EXPECT_NE(text.find("121 certificates from /app0/assets/ca-bundle.crt"), std::string::npos);
    EXPECT_NE(text.find("must be 1 for HTTPS"), std::string::npos);
    EXPECT_NE(text.find("folder writable: NO"), std::string::npos);
    EXPECT_NE(text.find("2962 packages, 1084 with an https cover URL"), std::string::npos);
    EXPECT_NE(text.find("/data/wings/diagnostics.txt"), std::string::npos);
    EXPECT_NE(text.find("paused for 60 s"), std::string::npos);
    // Plain text, line breaks only, nothing wider than a file manager preview.
    EXPECT_EQ(text.find('\r'), std::string::npos);
}

TEST(Diagnostics, ProblemLogKeepsTheLastTwelveAndTrimsLongLines)
{
    ProblemLog log;
    for (int i = 0; i < 30; ++i)
        log.add("problem " + std::to_string(i));
    log.add(std::string(1000, 'x'));
    const std::vector<std::string> lines = log.snapshot();
    ASSERT_EQ(lines.size(), 12u);
    EXPECT_EQ(lines.front(), "problem 19");
    EXPECT_EQ(lines.back().size(), 220u);
}

// ---- Settings > Cover art ----

struct SettingsHarness
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    host::FakeCoverArt cover{backend, false};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;

    explicit SettingsHarness(bool with_control = true)
    {
        context.app_version = "00.001.000";
        if (with_control)
            context.cover_art = &cover;
        store.poll_once();
        store.load_catalog_once();
        app.show_tab(tab::settings);
        frame();
    }
    void frame(Direction nav = Direction::none, std::uint32_t press = 0)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = press;
        feedback.clear();
        app.update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app.draw(drawn);
    }
    void confirm()
    {
        frame(Direction::none, hui::action_bit(Action::confirm));
    }
    int row_of(const std::string &title)
    {
        const auto rows = app.settings_screen().row_list();
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].title == title)
                return static_cast<int>(i);
        }
        return -1;
    }
    void go_to(int row)
    {
        for (int i = 0; i < row; ++i)
            frame(Direction::down);
    }
};

TEST(SettingsCoverArt, ShowsThePrivacySentenceAndTheOffStateByDefault)
{
    SettingsHarness h;
    const auto rows = h.app.settings_screen().row_list();
    const int toggle = h.row_of("Download store covers (DNS-over-HTTPS)");
    ASSERT_GE(toggle, 0);
    EXPECT_NE(rows[static_cast<std::size_t>(toggle)].detail.find("Cloudflare"), std::string::npos);
    EXPECT_NE(rows[static_cast<std::size_t>(toggle)].detail.find("Google"), std::string::npos);
    EXPECT_EQ(rows[static_cast<std::size_t>(toggle)].right, "Off");
    const int status = h.row_of("Cover art status");
    ASSERT_GE(status, 0);
    EXPECT_NE(rows[static_cast<std::size_t>(status)].detail.find("Cover art is off"),
              std::string::npos);
    EXPECT_GE(h.row_of("Clear art cache"), 0);
    EXPECT_GE(h.row_of("Retry failed covers"), 0);
    // Opt-in: nothing was switched on by drawing the screen.
    EXPECT_FALSE(h.cover.doh());
    EXPECT_FALSE(h.backend.store_art());
}

TEST(SettingsCoverArt, TurningItOnAsksFirstAndTurningItOffDoesNot)
{
    SettingsHarness h;
    h.go_to(h.row_of("Download store covers (DNS-over-HTTPS)"));
    h.confirm();
    EXPECT_TRUE(h.app.settings_screen().modal());
    EXPECT_FALSE(h.cover.doh()); // not yet
    // Back declines.
    h.frame(Direction::none, hui::action_bit(Action::back));
    EXPECT_FALSE(h.app.settings_screen().modal());
    EXPECT_FALSE(h.cover.doh());
    // Ask again and accept (the focus starts on "Turn on").
    h.confirm();
    ASSERT_TRUE(h.app.settings_screen().modal());
    h.confirm();
    EXPECT_FALSE(h.app.settings_screen().modal());
    EXPECT_TRUE(h.cover.doh());
    EXPECT_EQ(h.cover.changes, 1);
    EXPECT_EQ(h.app.toast(), "Cover downloads turned on");
    // The row now says On, and Cross turns it off at once.
    const auto rows = h.app.settings_screen().row_list();
    EXPECT_EQ(rows[static_cast<std::size_t>(h.row_of("Download store covers (DNS-over-HTTPS)"))].right,
              "On");
    h.confirm();
    EXPECT_FALSE(h.cover.doh());
    EXPECT_EQ(h.app.toast(), "Cover downloads turned off");
}

TEST(SettingsCoverArt, ClearAndRetryReachTheService)
{
    SettingsHarness h;
    h.go_to(h.row_of("Clear art cache"));
    h.confirm();
    EXPECT_EQ(h.cover.cleared, 1);
    h.frame(Direction::down);
    h.confirm();
    EXPECT_EQ(h.cover.retried, 1);
    EXPECT_EQ(h.cover.changes, 0);
}

TEST(SettingsCoverArt, WithoutTheModuleTheSectionSaysSoAndNothingIsActionable)
{
    SettingsHarness h(false);
    EXPECT_EQ(h.row_of("Download store covers (DNS-over-HTTPS)"), -1);
    const int row = h.row_of("Cover art");
    ASSERT_GE(row, 0);
    h.go_to(row);
    h.confirm();
    EXPECT_FALSE(h.app.settings_screen().modal());
}

// ---- ArtCache backpressure and pending polling ----

TEST(ArtCacheBackpressure, DecodesNoFurtherAheadThanTheUploadBound)
{
    host::FakeBackend backend;
    test::FakeSink sink;
    ArtLimits limits;
    limits.max_pending_uploads = 3;
    limits.uploads_per_frame = 2;
    ArtCache art(backend, sink, limits);
    for (int i = 0; i < 12; ++i)
        EXPECT_EQ(art.get("app:PPSA9010" + std::to_string(i), ArtKind::cover), nullptr);
    int decoded = 0;
    while (art.work_once())
        ++decoded;
    EXPECT_EQ(decoded, 3); // stops at the bound with nine requests still queued
    art.tick();            // uploads two
    int more = 0;
    while (art.work_once())
        ++more;
    EXPECT_EQ(more, 2); // two slots opened, no more
    EXPECT_EQ(sink.live.size(), 4u); // two images uploaded: cover + ambient wash each
}

TEST(ArtCacheBackpressure, AllRequestsFinishOnceTheFrameLoopKeepsUploading)
{
    host::FakeBackend backend;
    test::FakeSink sink;
    ArtLimits limits;
    limits.max_pending_uploads = 4;
    ArtCache art(backend, sink, limits);
    for (int i = 0; i < 20; ++i)
        art.get("app:G" + std::to_string(i), ArtKind::cover);
    for (int guard = 0; guard < 200; ++guard)
    {
        while (art.work_once())
        {
        }
        art.tick();
    }
    EXPECT_TRUE(art.idle());
    EXPECT_EQ(sink.live.size(), 20u * 2u); // cover + ambient wash each
}

TEST(ArtCachePending, AnswersPendingThenDeliversAndKeepsPlaceholdersForMissingCovers)
{
    host::FakeBackend backend;
    backend.set_store_art(true);
    test::FakeSink sink;
    ArtCache art(backend, sink);
    // pkg1 has a cover (after two "pending" answers); pkg16 never does.
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr);
    EXPECT_EQ(art.get("pkg:pkg16", ArtKind::cover), nullptr);
    const Artwork *ready = nullptr;
    for (int i = 0; i < 400 && ready == nullptr; ++i)
    {
        while (art.work_once())
        {
        }
        art.tick();
        ready = art.get("pkg:pkg1", ArtKind::cover);
        if (ready == nullptr)
            usleep(5000);
    }
    ASSERT_NE(ready, nullptr);
    EXPECT_GE(backend.art_asks("pkg:pkg1"), 3);
    EXPECT_EQ(art.get("pkg:pkg16", ArtKind::cover), nullptr);
}

TEST(ArtCachePending, RetryFailedAsksAgainAtOnceInsteadOfAfterTheTimer)
{
    host::FakeBackend backend;
    test::FakeSink sink;
    ArtCache art(backend, sink);
    const auto pump = [&]
    {
        while (art.work_once())
        {
        }
        art.tick();
    };
    // DNS-over-HTTPS off: no cover, the tile fails (placeholder).
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr);
    pump();
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr);
    const int before = backend.art_asks("pkg:pkg1");
    // The player turns it on: without retry_failed the 60 s timer still holds it back.
    backend.set_store_art(true);
    pump();
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr);
    pump();
    EXPECT_EQ(backend.art_asks("pkg:pkg1"), before);
    art.retry_failed();
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr); // queued again
    const Artwork *ready = nullptr;
    for (int i = 0; i < 400 && ready == nullptr; ++i)
    {
        pump();
        ready = art.get("pkg:pkg1", ArtKind::cover);
        if (ready == nullptr)
            usleep(5000);
    }
    EXPECT_NE(ready, nullptr);
}

// ---- image dimension guard ----

std::string png_header(std::uint32_t width, std::uint32_t height)
{
    std::string out("\x89PNG\r\n\x1a\n", 8);
    const auto be = [&](std::uint32_t v)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<char>((v >> shift) & 0xff));
    };
    be(13);
    out += "IHDR";
    be(width);
    be(height);
    out += std::string("\x08\x06\x00\x00\x00", 5);
    be(0); // CRC (stb does not check it while it only reads the header)
    // stb's header scan returns at the first IDAT chunk header (it looks for tRNS first).
    be(8);
    out += "IDAT";
    return out;
}

TEST(ImageGuard, RefusesLargeDimensionsBeforeAllocatingPixels)
{
    image::Pixels pixels;
    std::string error;
    EXPECT_FALSE(image::decode(png_header(5000, 100), 400, 400, &pixels, &error));
    EXPECT_NE(error.find("out of range"), std::string::npos) << error;
    EXPECT_FALSE(image::decode(png_header(2500, 2500), 400, 400, &pixels, &error)); // 6.25 Mpx
    EXPECT_NE(error.find("out of range"), std::string::npos) << error;
    // stb itself calls this "too large"; either way nothing is allocated.
    EXPECT_FALSE(image::decode(png_header(65535, 65535), 400, 400, &pixels, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(image::decode(png_header(8000, 8000), 400, 400, &pixels, &error));
    EXPECT_TRUE(pixels.empty());
}

TEST(ImageGuard, AcceptsExactlyFourMegapixelsAndOrdinaryCovers)
{
    // 2000 x 2000 = 4 Mpx exactly: accepted. A solid colour compresses to a few KB.
    std::vector<std::uint8_t> rgba(2000u * 2000u * 4u, 0x80);
    std::string png;
    stbi_write_png_to_func(append_png, &png, 2000, 2000, 4, rgba.data(), 2000 * 4);
    image::Pixels pixels;
    std::string error;
    ASSERT_TRUE(image::decode(png, 400, 400, &pixels, &error)) << error;
    EXPECT_EQ(pixels.width, 400);
    EXPECT_EQ(pixels.height, 400);
    // One pixel over is refused.
    EXPECT_FALSE(image::decode(png_header(2001, 2000), 400, 400, &pixels, &error));
    // A normal 260 px poster.
    std::vector<std::uint8_t> small(260u * 260u * 4u, 0x40);
    std::string poster;
    stbi_write_png_to_func(append_png, &poster, 260, 260, 4, small.data(), 260 * 4);
    ASSERT_TRUE(image::decode(poster, 400, 400, &pixels, &error)) << error;
    EXPECT_EQ(pixels.width, 260);
}

} // namespace
} // namespace orbit
