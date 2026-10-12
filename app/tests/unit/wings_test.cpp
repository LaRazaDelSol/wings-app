// Wings TV app - Image decoding, the mark, the Store on a fake backend and the app driven by input.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/image.hpp"
#include "orbit/mark.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include "../third_party/stb/stb_image_write.h"

#include <webp/encode.h>

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace orbit
{
namespace
{

void append(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}

std::vector<std::uint8_t> solid(int width, int height, std::uint8_t r, std::uint8_t g,
                                std::uint8_t b)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    return rgba;
}

TEST(Image, DecodesPngAndFitsItToTheDrawingSize)
{
    const std::vector<std::uint8_t> rgba = solid(800, 400, 200, 40, 10);
    std::string png;
    stbi_write_png_to_func(append, &png, 800, 400, 4, rgba.data(), 800 * 4);
    EXPECT_EQ(image::sniff(png), image::Format::png);
    image::Pixels pixels;
    ASSERT_TRUE(image::decode(png, 400, 400, &pixels));
    EXPECT_EQ(pixels.width, 400);
    EXPECT_EQ(pixels.height, 200);
    EXPECT_NEAR(pixels.rgba[0], 200, 1);
    EXPECT_NEAR(pixels.rgba[1], 40, 1);
    // Small images are never enlarged.
    ASSERT_TRUE(image::decode(png, 4000, 4000, &pixels));
    EXPECT_EQ(pixels.width, 800);
}

TEST(Image, DecodesWebpAtTheRequestedSize)
{
    const std::vector<std::uint8_t> rgba = solid(512, 512, 30, 120, 240);
    std::uint8_t *encoded = nullptr;
    const std::size_t size = WebPEncodeLosslessRGBA(rgba.data(), 512, 512, 512 * 4, &encoded);
    ASSERT_GT(size, 0u);
    const std::string webp(reinterpret_cast<const char *>(encoded), size);
    WebPFree(encoded);
    EXPECT_EQ(image::sniff(webp), image::Format::webp);
    image::Pixels pixels;
    ASSERT_TRUE(image::decode(webp, 400, 400, &pixels));
    EXPECT_EQ(pixels.width, 400);
    EXPECT_EQ(pixels.height, 400);
    EXPECT_NEAR(pixels.rgba[2], 240, 2);
}

TEST(Image, RefusesWhatIsNotAnImage)
{
    image::Pixels pixels;
    std::string error;
    EXPECT_FALSE(image::decode("<html>blocked</html>", 100, 100, &pixels, &error));
    EXPECT_EQ(error, "unsupported image format");
    EXPECT_FALSE(image::decode(std::string("RIFF\0\0\0\0WEBPjunk", 16), 100, 100, &pixels, &error));
    EXPECT_FALSE(
        image::decode(std::string("\x89PNG\r\n\x1a\nbroken", 14), 100, 100, &pixels, &error));
    EXPECT_TRUE(pixels.empty());
}

TEST(Image, ResizeAveragesAndAmbientIsASoftDarkenedWash)
{
    image::Pixels checker;
    checker.width = checker.height = 4;
    checker.rgba.resize(64);
    for (int i = 0; i < 16; ++i)
    {
        const std::uint8_t v = (i + i / 4) % 2 ? 255 : 0;
        checker.rgba[static_cast<std::size_t>(i) * 4] = v;
        checker.rgba[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    const image::Pixels one = image::resize(checker, 1, 1);
    EXPECT_NEAR(one.rgba[0], 128, 1);
    const image::Pixels wash = image::ambient(checker, 16);
    EXPECT_EQ(wash.width, 16);
    EXPECT_LT(wash.rgba[0], 128); // darkened
    EXPECT_EQ(wash.rgba[3], 255);
}

TEST(Mark, IsAWithOuterStrokesThatCanBeDimmed)
{
    const image::Pixels mark = mark::render(512);
    const auto alpha = [&](const image::Pixels &p, int x, int y)
    { return p.rgba[(static_cast<std::size_t>(y) * p.width + x) * 4 + 3]; };
    EXPECT_EQ(alpha(mark, 256, 214), 255); // the middle peak of the W
    EXPECT_EQ(alpha(mark, 10, 10), 0);     // the corner is transparent
    EXPECT_EQ(alpha(mark, 120, 260), 255); // on the left outer stroke
    const image::Pixels faint = mark::render(512, 0.3f);
    EXPECT_NEAR(alpha(faint, 120, 260), 77, 2); // outer strokes dimmed
    EXPECT_EQ(alpha(faint, 256, 214), 255);     // inner strokes untouched
}

// ---- the Store on the fake backend ----

TEST(Store, PollsHealthQueueAndTheSlowLists)
{
    host::FakeBackend backend;
    Store store(backend);
    EXPECT_EQ(store.state().link, Link::connecting);
    store.poll_once();
    // The catalogue is read by its own worker (here: by hand), after the sources.
    EXPECT_TRUE(store.load_catalog_once());
    EXPECT_FALSE(store.load_catalog_once()); // nothing changed since
    store.tick();
    const Snapshot &state = store.state();
    EXPECT_EQ(state.link, Link::online);
    EXPECT_EQ(state.info.version, "1.11.0");
    EXPECT_EQ(state.endpoint, "127.0.0.1:6970");
    EXPECT_FALSE(state.jobs.empty());
    EXPECT_TRUE(state.have_catalog);
    EXPECT_FALSE(state.packages.empty());
    EXPECT_FALSE(state.sources.empty());
    EXPECT_TRUE(state.have_settings);
    EXPECT_EQ(state.settings.connections, 16);
    EXPECT_TRUE(state.have_defaults);
    // The game database is read only when the Library asks.
    EXPECT_FALSE(state.have_installed);
    store.want_installed(true);
    store.poll_once();
    store.tick();
    EXPECT_FALSE(store.state().installed.empty());
    EXPECT_NE(store.state().package("pkg1"), nullptr);
}

TEST(Store, OfflineBackendGivesWaitingStateAndRecovers)
{
    host::FakeBackend backend(host::FakeMode::offline);
    Store store(backend);
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::offline);
    EXPECT_FALSE(store.state().link_error.empty());
    EXPECT_FALSE(store.state().have_catalog);
    EXPECT_FALSE(store.load_catalog_once()); // nothing to read before the sources are known
    backend.set_mode(host::FakeMode::normal);
    store.poll_once();
    store.load_catalog_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_TRUE(store.state().have_catalog);
}

TEST(Store, TheCatalogueIsReadAgainOnlyWhenTheSourcesChange)
{
    host::FakeBackend backend;
    Store store(backend);
    store.poll_once();
    ASSERT_TRUE(store.load_catalog_once());
    store.tick();
    const std::uint64_t first = store.state().catalog_serial;
    store.refresh_now();
    store.poll_once();
    EXPECT_FALSE(store.load_catalog_once());
    store.source_enable("src1", false); // a source changes: the catalogue is stale
    ASSERT_TRUE(store.run_next_action());
    EXPECT_TRUE(store.load_catalog_once());
    store.tick();
    EXPECT_GT(store.state().catalog_serial, first);
}

TEST(Store, JobActionsRunInOrderAndUpdateTheQueue)
{
    host::FakeBackend backend;
    Store store(backend);
    store.poll_once();
    store.tick();
    store.job_action("job1", "pause");
    EXPECT_TRUE(store.action().busy);
    EXPECT_TRUE(store.run_next_action());
    EXPECT_FALSE(store.run_next_action());
    store.tick();
    EXPECT_FALSE(store.action().busy);
    for (const Job &job : store.state().jobs)
    {
        if (job.id == "job1")
            EXPECT_EQ(job.status, "paused");
    }
    store.job_action("nope", "pause");
    store.run_next_action();
    store.tick();
    EXPECT_FALSE(store.action().error.empty());
}

TEST(Store, ClearFinishedPasswordAndChoiceGoThroughTheQueue)
{
    host::FakeBackend backend;
    Store store(backend);
    store.poll_once();
    store.tick();
    store.archive_password("job7", "wrong");
    store.run_next_action();
    store.tick();
    ASSERT_NE(store.state().job("job7"), nullptr);
    EXPECT_EQ(store.state().job("job7")->status, "waitingPassword");
    EXPECT_FALSE(store.state().job("job7")->error.empty());
    store.archive_password("job7", "right");
    store.archive_choice("job8", 1);
    store.run_next_action();
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().job("job7")->status, "extracting");
    EXPECT_EQ(store.state().job("job8")->status, "extracting");
    const std::size_t before = store.state().jobs.size();
    store.clear_finished();
    store.run_next_action();
    store.tick();
    EXPECT_LT(store.state().jobs.size(), before);
    for (const Job &job : store.state().jobs)
        EXPECT_FALSE(job_finished(job));
}

TEST(Store, TheCaptureSessionIsWatchedUntilItCompletes)
{
    host::FakeBackend backend;
    backend.set_capture_steps(2);
    Store store(backend);
    store.poll_once();
    store.tick();
    const std::size_t jobs = store.state().jobs.size();
    store.start_link("pkg1", 2, ""); // a browser-capture link
    ASSERT_TRUE(store.run_next_action());
    store.tick();
    EXPECT_EQ(store.state().capture.state, "watching");
    EXPECT_EQ(store.state().capture.session_id, "7");
    for (int i = 0; i < 6 && store.state().capture.state != "completed"; ++i)
    {
        store.poll_once();
        store.tick();
    }
    EXPECT_EQ(store.state().capture.state, "completed");
    EXPECT_GT(store.state().jobs.size(), jobs); // the captured download is queued
    store.dismiss_capture();
    EXPECT_TRUE(store.state().capture.state.empty());
}

TEST(Store, CancellingACaptureStopsItButNotWhileItResolves)
{
    host::FakeBackend backend;
    Store store(backend);
    store.poll_once();
    store.tick();
    store.start_link("pkg1", 2, "");
    store.run_next_action();
    store.tick();
    EXPECT_TRUE(store.cancel_capture());
    store.run_next_action();
    store.tick();
    EXPECT_EQ(backend.cancelled_captures(), 1);
    EXPECT_EQ(store.state().capture.state, "cancelled");
    store.dismiss_capture();
}

TEST(Store, ARunningCaptureIsPickedUpAfterARestart)
{
    host::FakeBackend backend;
    backend.start_link("pkg1", 2, ""); // started by an earlier run of the app
    Store store(backend);
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().capture.session_id, "7");
    EXPECT_FALSE(store.state().capture.terminal());
}

TEST(Store, DirectLinksAreCheckedThenQueued)
{
    host::FakeBackend backend;
    Store store(backend);
    store.poll_once();
    store.tick();
    store.probe_link("https://files.example/game.pkg");
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().probe.file_name, "game.pkg");
    EXPECT_TRUE(store.probe_action().error.empty());
    const std::size_t jobs = store.state().jobs.size();
    store.queue_direct(store.state().probe, "");
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().jobs.size(), jobs + 1);
    store.probe_link("https://site.example/page");
    store.run_next_action();
    store.tick();
    EXPECT_NE(store.probe_action().error.find("web page"), std::string::npos);
}

TEST(Store, SourceActionsUpdateTheListAndReportErrors)
{
    host::FakeBackend backend;
    Store store(backend);
    store.poll_once();
    store.tick();
    const std::size_t sources = store.state().sources.size();
    store.source_add_url("https://catalog.example/list.json");
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().sources.size(), sources + 1);
    EXPECT_EQ(store.source_action().message, "Source added");
    store.source_add_url("https://catalog.example/bad.json");
    store.run_next_action();
    store.tick();
    EXPECT_FALSE(store.source_action().error.empty());
    store.source_delete("src1");
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().sources.size(), sources);
}

TEST(Art, FetchesInstalledGameArtAndSkipsStorePosters)
{
    host::FakeBackend backend;
    test::FakeSink sink;
    ArtCache art(backend, sink);
    EXPECT_EQ(art.get("app:PPSA90101", ArtKind::cover), nullptr); // queued
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr);
    while (art.work_once())
    {
    }
    art.tick();
    art.tick();
    EXPECT_NE(art.get("app:PPSA90101", ArtKind::cover), nullptr);
    EXPECT_EQ(art.get("pkg:pkg1", ArtKind::cover), nullptr); // failed: placeholder stays
}

// ---- the whole app, driven by controller input against the fake backend ----

struct AppHarness
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;

    explicit AppHarness(host::FakeMode mode = host::FakeMode::normal) : backend(mode)
    {
        context.folder_confirmed = [] { return true; };
        context.app_version = "00.001.000";
        store.poll_once();
        store.load_catalog_once();
        frame();
    }
    // Runs whatever the screens queued, then lets the frame loop see the results.
    void work(int frames = 3)
    {
        while (store.run_next_action())
        {
        }
        for (int i = 0; i < frames; ++i)
            frame();
    }
    void down(int times = 1)
    {
        for (int i = 0; i < times; ++i)
            frame(Direction::down);
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
        for (const auto &instance : drawn.scene.instances())
            ASSERT_TRUE(std::isfinite(instance.rect[0]) && std::isfinite(instance.rect[1]));
    }
    void press(Action action)
    {
        frame(Direction::none, hui::action_bit(action));
    }
    void settle(int frames = 30)
    {
        for (int i = 0; i < frames; ++i)
            frame();
    }
};

int backend_cancelled(AppHarness &h)
{
    return h.backend.cancelled_captures();
}

TEST(App, OpensOnTheStoreTabAndL1R1CycleAllSevenTabs)
{
    AppHarness h;
    EXPECT_EQ(h.app.current_tab(), tab::store);
    for (int expected = 1; expected < tab::count; ++expected)
    {
        h.press(Action::page_next);
        EXPECT_EQ(h.app.current_tab(), expected);
    }
    h.press(Action::page_next); // past the last tab: refused, stays
    EXPECT_EQ(h.app.current_tab(), tab::logs);
    for (int i = 0; i < tab::count; ++i)
        h.press(Action::page_prev);
    EXPECT_EQ(h.app.current_tab(), tab::store);
}

TEST(App, EveryTabDrawsOnlineOfflineAndEmpty)
{
    for (const host::FakeMode mode :
         {host::FakeMode::normal, host::FakeMode::offline, host::FakeMode::empty})
    {
        AppHarness h(mode);
        for (int t = 0; t < tab::count; ++t)
        {
            h.app.show_tab(t);
            h.settle(5);
            Frame drawn;
            h.app.draw(drawn);
            EXPECT_GT(drawn.scene.instances().size(), 10u);
        }
    }
}

TEST(App, TheShellShowsBeforeTheCatalogueLoadsAndWhileOffline)
{
    host::FakeBackend backend(host::FakeMode::offline);
    test::FakeSink sink;
    test::TestFonts fonts;
    Store store(backend);
    ArtCache art(backend, sink);
    Context context(fonts.type, fonts.fonts, store, art);
    App app(context);
    context.folder_confirmed = [] { return true; };
    Feedback feedback;
    // Nothing polled yet: the shell (tabs, status pill) is already drawn.
    app.update({}, 1.0f / 60.0f, feedback);
    Frame before;
    app.draw(before);
    EXPECT_GT(before.scene.instances().size(), 20u);
    store.poll_once();
    app.update({}, 1.0f / 60.0f, feedback);
    EXPECT_EQ(store.state().link, Link::offline);
    Frame offline;
    app.draw(offline);
    EXPECT_GT(offline.scene.instances().size(), 20u);
}

TEST(App, UpFromContentReachesTheTopBarAndDownReturns)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(5);
    EXPECT_FALSE(h.app.top_bar_focused());
    h.frame(Direction::up);
    EXPECT_TRUE(h.app.top_bar_focused());
    h.frame(Direction::right);
    EXPECT_EQ(h.app.current_tab(), tab::library);
    h.frame(Direction::down);
    EXPECT_FALSE(h.app.top_bar_focused());
}

TEST(App, ConfirmOnADownloadAsksTheBackendToPauseIt)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(5);
    h.press(Action::confirm); // the first row is the downloading job: its menu
    h.press(Action::confirm); // the first entry is Pause
    while (h.store.run_next_action()) // the Store's background reads may be queued first
    {
    }
    h.store.tick();
    bool paused = false;
    for (const Job &job : h.store.state().jobs)
        paused = paused || (job.id == "job1" && job.status == "paused");
    EXPECT_TRUE(paused);
}

// ---- Store screen ----

TEST(StoreScreen, SearchFiltersByTitleIdAndSource)
{
    AppHarness h;
    h.settle(3);
    StoreScreen &store = h.app.store_screen();
    EXPECT_EQ(store.shown_count(), 25); // one tile per game: Starfall Odyssey is listed three times
    store.set_query("lantern");
    h.settle(2);
    EXPECT_EQ(store.shown_count(), 2); // Lantern Road, Moth & Lantern
    store.set_query("PPSA90021");
    h.settle(2);
    EXPECT_EQ(store.shown_count(), 1);
    store.set_query("zzzz");
    h.settle(2);
    EXPECT_EQ(store.shown_count(), 0);
    store.set_query("");
    store.set_source_filter("Community Mirror");
    h.settle(2);
    EXPECT_EQ(store.shown_count(), 8);
    store.set_source_filter("A source that is gone"); // no such source: the filter drops
    h.settle(2);
    EXPECT_EQ(store.shown_count(), 25);
}

TEST(StoreScreen, TheSearchKeyboardOpensOnTriangleAndBackClosesIt)
{
    AppHarness h;
    h.settle(3);
    EXPECT_FALSE(h.app.store_screen().modal());
    h.press(Action::north);
    EXPECT_TRUE(h.app.store_screen().modal());
    // Tabs do not switch under the keyboard.
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), tab::store);
    h.press(Action::back);
    EXPECT_FALSE(h.app.store_screen().modal());
}

TEST(StoreScreen, ConfirmOpensThePackagePageAndBackReturns)
{
    AppHarness h;
    h.settle(3);
    h.press(Action::confirm);
    EXPECT_TRUE(h.app.store_screen().detail_open());
    h.work();
    h.press(Action::back);
    EXPECT_FALSE(h.app.store_screen().detail_open());
}

TEST(StoreScreen, APkgLinkAsksThenHandsOverToThePs5)
{
    AppHarness h;
    h.settle(3);
    h.app.store_screen().open_package("pkg1");
    h.work();
    const GameView &page = h.app.store_screen().game_page();
    ASSERT_EQ(page.details.size(), 1u);
    ASSERT_EQ(page.details[0].links.size(), 5u);
    EXPECT_EQ(page.details[0].links[0].route, LinkRoute::native_pkg);
    // The Best card (focused first) is the .pkg.
    ASSERT_GE(page.page.best, 0);
    EXPECT_EQ(page.page.mirrors[static_cast<std::size_t>(page.page.best)].tier, LinkTier::native_pkg);
    const std::size_t jobs = h.store.state().jobs.size();
    h.down(1);                // the page opens on the header: Down goes to the downloads
    h.press(Action::confirm); // the Best card: a confirmation, nothing sent yet
    EXPECT_TRUE(h.app.store_screen().modal());
    EXPECT_EQ(h.store.state().link_serial, 0u);
    h.press(Action::confirm); // Send
    h.work();
    EXPECT_GT(h.store.state().link_serial, 0u);
    EXPECT_TRUE(h.store.state().link_result.dispatch.native);
    EXPECT_TRUE(h.app.store_screen().modal()); // the "sent" notice
    h.press(Action::confirm);
    EXPECT_FALSE(h.app.store_screen().modal());
    EXPECT_EQ(h.store.state().jobs.size(), jobs); // it went to PS5 Downloads, not Pegasus' queue
}

TEST(StoreScreen, LinksAreSortedByTierKeepingTheOriginalOrderInsideATier)
{
    AppHarness h;
    h.settle(3);
    // The tier order of the Store's own package detail (kept for other readers).
    h.store.open_package("pkg9"); // seven links of every kind
    h.work();
    const std::vector<PackageLink> &links = h.store.state().detail.links;
    ASSERT_EQ(links.size(), 7u);
    // native, automated, supported (browser), debrid x2, unknown, not supported.
    const LinkTier expected[] = {LinkTier::native_pkg, LinkTier::automated,  LinkTier::supported,
                                 LinkTier::debrid,     LinkTier::debrid,     LinkTier::unknown,
                                 LinkTier::unsupported};
    for (std::size_t i = 0; i < links.size(); ++i)
        EXPECT_EQ(static_cast<int>(link_tier(links[i])), static_cast<int>(expected[i]));
    // Inside the debrid tier the catalogue order (3 before 4) is kept.
    EXPECT_EQ(links[3].index, 3);
    EXPECT_EQ(links[4].index, 4);
    EXPECT_EQ(static_cast<int>(best_tier(links)), static_cast<int>(LinkTier::native_pkg));
    // On the game page a card still sends its link's own catalogue index.
    StoreScreen &screen = h.app.store_screen();
    screen.open_package("pkg9");
    h.work();
    const GameView &page = screen.game_page();
    int capture = -1;
    for (std::size_t m = 0; m < page.page.mirrors.size(); ++m)
        if (page.first_ref(static_cast<int>(m)).link_index == 2) // the manual-capture link
            capture = static_cast<int>(m);
    ASSERT_GE(capture, 0);
    ASSERT_TRUE(screen.focus_page_mirror(capture));
    h.press(Action::confirm);
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().capture.state, "watching");
}

TEST(LinkTiers, ClassificationLabelsAndStableSort)
{
    const auto link = [](int index, LinkRoute route, const char *provider = "")
    {
        PackageLink l;
        l.index = index;
        l.route = route;
        l.provider = provider;
        return l;
    };
    EXPECT_EQ(static_cast<int>(link_tier(link(0, LinkRoute::capture))),
              static_cast<int>(LinkTier::unknown));
    EXPECT_EQ(static_cast<int>(link_tier(link(0, LinkRoute::capture, "MEGA"))),
              static_cast<int>(LinkTier::supported));
    EXPECT_EQ(static_cast<int>(link_tier(link(0, LinkRoute::browser_only))),
              static_cast<int>(LinkTier::unsupported));
    EXPECT_EQ(static_cast<int>(link_tier(link(0, LinkRoute::missing))),
              static_cast<int>(LinkTier::unsupported));
    EXPECT_TRUE(tier_is_green(LinkTier::automated));
    EXPECT_FALSE(tier_is_green(LinkTier::debrid));
    EXPECT_EQ(std::string(tier_label(LinkTier::debrid)), "Debrid");
    EXPECT_EQ(std::string(tier_label(LinkTier::unsupported)), "Not supported");
    EXPECT_EQ(static_cast<int>(best_tier({})), static_cast<int>(LinkTier::unsupported));
    std::vector<PackageLink> links = {link(0, LinkRoute::blocked), link(1, LinkRoute::capture),
                                      link(2, LinkRoute::debrid), link(3, LinkRoute::capture, "A"),
                                      link(4, LinkRoute::capture, "B"), link(5, LinkRoute::automated),
                                      link(6, LinkRoute::native_pkg), link(7, LinkRoute::debrid)};
    sort_links_by_tier(links);
    const int order[] = {6, 5, 3, 4, 2, 7, 1, 0};
    for (std::size_t i = 0; i < links.size(); ++i)
        EXPECT_EQ(links[i].index, order[i]);
}

// The game page's card whose (first) link has this catalogue index gets the focus.
void focus_link(AppHarness &h, int link_index)
{
    StoreScreen &screen = h.app.store_screen();
    const GameView &page = screen.game_page();
    for (std::size_t m = 0; m < page.page.mirrors.size(); ++m)
    {
        if (page.first_ref(static_cast<int>(m)).link_index == link_index)
        {
            ASSERT_TRUE(screen.focus_page_mirror(static_cast<int>(m)));
            return;
        }
    }
    FAIL() << "no card for link " << link_index;
}

TEST(StoreScreen, ADebridLinkQueuesDirectlyAndAToastSaysSo)
{
    AppHarness h;
    h.settle(3);
    h.app.store_screen().open_package("pkg1");
    h.work();
    const std::size_t jobs = h.store.state().jobs.size();
    focus_link(h, 3); // "Premium mirror": one ready debrid service
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().jobs.size(), jobs + 1);
    EXPECT_NE(h.app.toast().find("Real-Debrid"), std::string::npos);
}

TEST(StoreScreen, SeveralDebridServicesAskWhichAndFilesAreSelected)
{
    AppHarness h;
    h.settle(3);
    h.app.store_screen().open_package("pkg1");
    h.work();
    const std::size_t jobs = h.store.state().jobs.size();
    focus_link(h, 4); // "Season pack": both services, several files
    h.press(Action::confirm);
    EXPECT_TRUE(h.app.store_screen().modal()); // the service choice
    h.press(Action::confirm);                  // the first one
    h.work();
    EXPECT_EQ(h.store.state().link_result.kind, LinkResult::Kind::select_files);
    EXPECT_EQ(h.store.state().link_result.files.size(), 3u);
    h.down(3); // past the three files, onto the Download button
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().jobs.size(), jobs + 1);
}

TEST(StoreScreen, ABrowserCheckOpensThePanelAndHidingKeepsItRunning)
{
    AppHarness h;
    h.backend.set_capture_steps(3);
    h.settle(3);
    h.app.store_screen().open_package("pkg1");
    h.work();
    focus_link(h, 2); // "Mirror (needs a check)": needs the PS5 browser
    h.press(Action::confirm);
    EXPECT_TRUE(h.app.store_screen().modal());
    h.press(Action::confirm); // Open browser
    h.work();
    EXPECT_TRUE(h.app.capture_panel_open());
    EXPECT_EQ(h.store.state().capture.state, "watching");
    h.press(Action::back); // hide: it does not cancel
    EXPECT_FALSE(h.app.capture_panel_open());
    EXPECT_EQ(backend_cancelled(h), 0);
    h.press(Action::menu); // Options brings it back
    EXPECT_TRUE(h.app.capture_panel_open());
    // Left then Cross: Cancel capture is the left-hand button (bug F).
    h.frame(Direction::left);
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(backend_cancelled(h), 1);
    EXPECT_EQ(h.store.state().capture.state, "cancelled");
    EXPECT_TRUE(h.app.capture_panel_open()); // stays until it is closed
    h.press(Action::confirm);
    EXPECT_FALSE(h.app.capture_panel_open());
}

TEST(StoreScreen, ACompletedCaptureToastsAndClosesThePanel)
{
    AppHarness h;
    h.backend.set_capture_steps(1);
    h.settle(3);
    h.app.store_screen().open_package("pkg1");
    h.work();
    focus_link(h, 2);
    h.press(Action::confirm);
    h.press(Action::confirm);
    h.work();
    for (int i = 0; i < 4; ++i)
    {
        h.store.poll_once();
        h.work(2);
    }
    EXPECT_FALSE(h.app.capture_panel_open());
    EXPECT_NE(h.app.toast().find("queued"), std::string::npos);
}

TEST(StoreScreen, AnErrorFromPegasusIsShownNotSwallowed)
{
    AppHarness h;
    h.settle(3);
    h.app.store_screen().open_package("pkg1");
    h.work();
    h.backend.set_mode(host::FakeMode::offline);
    focus_link(h, 3);
    h.press(Action::confirm);
    h.work();
    EXPECT_FALSE(h.store.link_action().error.empty());
    EXPECT_TRUE(h.app.store_screen().modal()); // the notice
    EXPECT_EQ(h.store.state().link, Link::offline);
}

// ---- Downloads screen ----

TEST(DownloadsScreen, ActiveJobsComeFirstAndTheMenuFitsTheState)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(3);
    DownloadsScreen &downloads = h.app.downloads_screen();
    EXPECT_EQ(downloads.row_count(), 8);
    h.press(Action::confirm); // job1: downloading
    EXPECT_EQ(downloads.menu_codes(), (std::vector<std::string>{"pause", "cancel"}));
    h.press(Action::back);
    h.down(3); // job4: paused
    h.press(Action::confirm);
    EXPECT_EQ(downloads.menu_codes(), (std::vector<std::string>{"resume", "cancel"}));
    h.press(Action::back);
    h.down(1); // job7: waiting for a password
    h.press(Action::confirm);
    EXPECT_EQ(downloads.menu_codes().front(), "password");
    h.press(Action::back);
    h.down(3); // job6 is the failed one: finished jobs come last
    h.press(Action::confirm);
    EXPECT_EQ(downloads.menu_codes().front(), "retry");
}

TEST(DownloadsScreen, CancelAsksFirstAndTheDefaultIsToKeepIt)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(3);
    h.press(Action::confirm);
    h.down(1);
    h.press(Action::confirm); // Cancel download: a destructive confirmation
    h.press(Action::confirm); // the focus starts on "Keep it"
    h.work();
    EXPECT_EQ(h.store.state().job("job1")->status, "downloading");
    h.press(Action::confirm);
    h.down(1);
    h.press(Action::confirm);
    h.frame(Direction::right); // onto Cancel download
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().job("job1")->status, "cancelled");
}

TEST(DownloadsScreen, ThePasswordIsTypedOnTheKeyboardAndSent)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(3);
    h.down(4);
    h.press(Action::confirm);
    h.press(Action::confirm); // Enter the archive password
    EXPECT_TRUE(h.app.downloads_screen().modal());
    h.app.downloads_screen().type_into_prompt("secret");
    h.down(10); // through the keys onto the buttons
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().job("job7")->status, "extracting");
}

TEST(DownloadsScreen, ClearFinishedConfirmsThenRemovesThem)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(3);
    h.press(Action::north);
    EXPECT_TRUE(h.app.downloads_screen().modal());
    h.frame(Direction::right);
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.app.downloads_screen().row_count(), 6);
}

TEST(DownloadsScreen, ADirectLinkIsCheckedConfirmedAndQueued)
{
    AppHarness h;
    h.app.show_tab(tab::downloads);
    h.settle(3);
    const std::size_t jobs = h.store.state().jobs.size();
    h.press(Action::west);
    EXPECT_TRUE(h.app.downloads_screen().modal());
    h.down(10);
    h.press(Action::confirm); // the prompt starts with "https://": Done checks it
    h.work();
    // The fake accepts any address; the answer is a confirmation of the file.
    EXPECT_TRUE(h.app.downloads_screen().modal());
    h.press(Action::confirm); // Add to queue
    h.work();
    EXPECT_EQ(h.store.state().jobs.size(), jobs + 1);
}

// ---- Sources screen ----

TEST(SourcesScreen, TheMenuTurnsOffRefreshesAndDeletesWithConfirmation)
{
    AppHarness h;
    h.app.show_tab(tab::sources);
    h.settle(3);
    h.press(Action::confirm);
    h.press(Action::confirm); // Turn off
    h.work();
    EXPECT_FALSE(h.store.state().sources[0].enabled);
    h.press(Action::confirm);
    h.down(2);
    h.press(Action::confirm); // Delete: asks first
    h.press(Action::confirm); // default focus is "Keep it"
    h.work();
    EXPECT_EQ(h.store.state().sources.size(), 3u);
    h.press(Action::confirm);
    h.down(2);
    h.press(Action::confirm);
    h.frame(Direction::right);
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().sources.size(), 2u);
}

TEST(SourcesScreen, AddingByAddressSendsTheTypedUrl)
{
    AppHarness h;
    h.app.show_tab(tab::sources);
    h.settle(3);
    h.press(Action::west);
    EXPECT_TRUE(h.app.sources_screen().modal());
    h.app.sources_screen().type_into_prompt("catalog.example/list.json");
    h.down(10);
    h.press(Action::confirm);
    h.work();
    ASSERT_EQ(h.store.state().sources.size(), 4u);
    EXPECT_EQ(h.store.state().sources.back().url, "https://catalog.example/list.json");
}

TEST(SourcesScreen, FirstRunOffersTheSuggestedSourcesAndAppliesTheTickedOnes)
{
    AppHarness h(host::FakeMode::onboarding);
    EXPECT_TRUE(h.store.state().sources.empty());
    h.app.show_tab(tab::sources);
    h.settle(3);
    EXPECT_TRUE(h.app.sources_screen().modal()); // the onboarding list opened by itself
    h.down(3);                                   // past the three, onto "Add selected"
    h.press(Action::confirm);
    h.work();
    EXPECT_EQ(h.store.state().sources.size(), 2u); // the two pre-ticked ones
    EXPECT_FALSE(h.store.state().defaults.onboarding_required);
    EXPECT_FALSE(h.app.sources_screen().modal());
}

TEST(SourcesScreen, NotNowDismissesTheOnboarding)
{
    AppHarness h(host::FakeMode::onboarding);
    h.app.show_tab(tab::sources);
    h.settle(3);
    ASSERT_TRUE(h.app.sources_screen().modal());
    h.press(Action::back);
    h.work();
    EXPECT_FALSE(h.store.state().defaults.onboarding_required);
    EXPECT_TRUE(h.store.state().sources.empty());
}

TEST(App, SurvivesRandomInputEverywhere)
{
    AppHarness h;
    h.backend.set_capture_steps(3);
    std::mt19937 random(7);
    const Direction directions[] = {Direction::none, Direction::up, Direction::down,
                                    Direction::left, Direction::right};
    const Action actions[] = {Action::confirm,   Action::back,      Action::north,
                              Action::west,      Action::page_prev, Action::page_next,
                              Action::jump_prev, Action::jump_next};
    for (int i = 0; i < 3000; ++i)
    {
        const Direction nav = directions[random() % 5];
        const std::uint32_t press = random() % 4 == 0 ? hui::action_bit(actions[random() % 8]) : 0;
        h.frame(nav, press);
        while (h.store.run_next_action())
        {
        }
        if (i % 100 == 0)
            h.store.poll_once();
    }
    SUCCEED();
}

} // namespace
} // namespace orbit
