// Wings TV app - The Store's games: one tile per game, the game page, Download all parts, bug F.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The view model (game_view.*) on the fake backend's "Starfall Odyssey" (three
// listings, part sets, wrappers, regions) and on the whole recorded catalogue
// (recon/samples/library.json); the screen and the app driven by controller
// input; favourites written by the per-package grid; the browser-capture
// panel's D-pad (bug F).

#include "core/save_file.hpp"
#include "fake_backend.hpp"
#include "orbit/game_view.hpp"
#include "orbit/pegasus_backend.hpp"
#include "orbit/screens.hpp"
#include "orbit/store.hpp"
#include "support.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace orbit
{
namespace
{

// ---- helpers ----

struct Rig
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    std::unique_ptr<App> app;
    Feedback feedback;

    explicit Rig(const std::string &catalogue_path = {})
    {
        context.app_version = "00.001.000";
        if (!catalogue_path.empty())
            context.catalogue_paths = {catalogue_path};
        app = std::make_unique<App>(context);
        context.folder_confirmed = [] { return true; }; // these tests are not about the first-download question
        store.poll_once();
        store.load_catalog_once();
        frame();
        store.poll_once(); // the Store screen asked for the game list
        frame();
        work();
    }
    void frame(Direction nav = Direction::none, std::uint32_t press = 0)
    {
        hui::InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = press;
        feedback.clear();
        app->update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app->draw(drawn);
    }
    // Runs whatever the screens queued until nothing is left, frame by frame.
    void work()
    {
        for (int round = 0; round < 60; ++round)
        {
            bool any = false;
            while (store.run_next_action())
                any = true;
            frame();
            if (!any)
                break;
        }
        frame();
    }
    void press(hui::Action action)
    {
        frame(Direction::none, hui::action_bit(action));
        frame();
    }
    StoreScreen &screen()
    {
        return app->store_screen();
    }
    int tile_of(const std::string &title)
    {
        const std::vector<GameTile> &tiles = screen().tiles();
        for (std::size_t i = 0; i < tiles.size(); ++i)
            if (screen().games().games[static_cast<std::size_t>(tiles[i].game)].title == title)
                return static_cast<int>(i);
        return -1;
    }
    // Moves the grid's focus from the first tile to a position.
    void focus_tile(int position)
    {
        for (int i = 0; i < position % 5; ++i)
            frame(Direction::right);
        for (int i = 0; i < position / 5; ++i)
            frame(Direction::down);
        frame();
    }
};

std::vector<PackageDetail> starfall_details(host::FakeBackend &backend)
{
    std::vector<PackageDetail> out;
    for (const char *id : {"pkg25", "pkg26", "pkg27"})
        out.push_back(backend.package(id).value);
    return out;
}

int find_mirror(const GameView &v, const std::string &host, int parts)
{
    for (std::size_t m = 0; m < v.page.mirrors.size(); ++m)
        if (v.page.mirrors[m].host == host && v.page.mirrors[m].parts() == parts)
            return static_cast<int>(m);
    return -1;
}

int find_mirror_named(const GameView &v, const std::string &first_link_name)
{
    for (std::size_t m = 0; m < v.page.mirrors.size(); ++m)
        if (v.page.links[static_cast<std::size_t>(v.page.mirrors[m].links.front())].name == first_link_name)
            return static_cast<int>(m);
    return -1;
}

// Rows sit one under the other; every shown mirror has exactly one card.
void expect_rows_consistent(const GameView &v)
{
    float bottom = 0.0f;
    std::multiset<int> cards;
    for (const PageRow &row : v.rows)
    {
        EXPECT_GE(row.y + 0.01f, bottom);
        EXPECT_GT(row.h, 0.0f);
        bottom = row.y + row.h;
        if (row.kind == PageRowKind::mirror)
            cards.insert(row.mirror);
    }
    EXPECT_LE(bottom, v.height + 0.01f);
    EXPECT_EQ(cards.size(), v.shown.mirrors.size());
    for (const int m : v.shown.mirrors)
        EXPECT_EQ(cards.count(m), 1u);
}

// ---- the grid ----

TEST(GameGrid, OneTilePerGameWithItsRegionsAndSources)
{
    Rig r;
    StoreScreen &s = r.screen();
    EXPECT_TRUE(s.facts_ready());
    EXPECT_EQ(s.shown_count(), 25); // 27 packages, the three Starfall listings are one game
    const int at = r.tile_of("Starfall Odyssey");
    ASSERT_GE(at, 0);
    const GameTile &tile = s.tiles()[static_cast<std::size_t>(at)];
    const GameGroup &game = s.games().games[static_cast<std::size_t>(tile.game)];
    EXPECT_EQ(game.packages.size(), 3u);
    EXPECT_EQ(game.sources.size(), 3u);
    EXPECT_EQ(tile.regions, (std::vector<std::string>{"USA", "EUR"}));
    EXPECT_EQ(s.shown_ids()[static_cast<std::size_t>(at)], "pkg25");
    // The search matches a game through any of its packages.
    s.set_query("PPSA90026");
    r.frame();
    ASSERT_EQ(s.shown_count(), 1);
    EXPECT_EQ(s.shown_ids().front(), "pkg25");
    s.set_query("exfat");
    r.frame();
    EXPECT_EQ(s.shown_count(), 1);
    s.set_query("");
    // Region: only the game whose links or descriptions say EUR.
    CatalogueState state = s.catalogue_state();
    state.region = "EUR";
    s.set_catalogue_state(state);
    r.frame();
    ASSERT_EQ(s.shown_count(), 1);
    EXPECT_EQ(s.shown_ids().front(), "pkg25");
    // No browser needed keeps games with a .pkg, automated or debrid link.
    state.region.clear();
    state.no_browser = true;
    s.set_catalogue_state(state);
    r.frame();
    EXPECT_LT(s.shown_count(), 25);
    EXPECT_GE(r.tile_of("Starfall Odyssey"), 0);
    state.reset_filters();
    s.set_catalogue_state(state);
    r.frame();
    EXPECT_EQ(s.shown_count(), 25);
}

TEST(GameGrid, RunsOnMyFirmwareKeepsLinksThatDoNotSay)
{
    std::vector<Package> p(3);
    const char *titles[] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; ++i)
    {
        p[static_cast<std::size_t>(i)].id = std::to_string(i);
        p[static_cast<std::size_t>(i)].title = titles[i];
        p[static_cast<std::size_t>(i)].title_id = "PPSA0000" + std::to_string(i + 1);
        p[static_cast<std::size_t>(i)].link_count = 1;
    }
    const GameIndex index = group_games(p);
    ASSERT_EQ(index.games.size(), 3u);
    std::vector<PackageFacts> facts(3);
    for (PackageFacts &f : facts)
        f.known = true;
    facts[0].lowest_fw = fw_need("12.xx"); // only a 12.xx build
    facts[1].lowest_fw = fw_need("4.xx");  // a backport
    facts[2].fw_unknown = true;            // does not say
    const auto game = [&](int package) { return index.games[static_cast<std::size_t>(index.game_of[static_cast<std::size_t>(package)])]; };
    EXPECT_EQ(game_runs_on(game(0), facts, "10.xx"), 0);
    EXPECT_EQ(game_runs_on(game(1), facts, "10.xx"), 1);
    EXPECT_EQ(game_runs_on(game(2), facts, "10.xx"), 1);
    EXPECT_EQ(game_runs_on(game(0), facts, "13.xx"), 1);
    EXPECT_EQ(game_runs_on(game(0), facts, ""), -1);        // no firmware set
    EXPECT_EQ(game_runs_on(game(0), {}, "10.xx"), -1);      // links not read yet
    CatalogueState state;
    state.runs_on_my_fw = true;
    InstalledIndex installed;
    LinkPrefs prefs;
    prefs.firmware = "10.xx";
    std::vector<GameTile> tiles = select_games(GridQuery{p, {}, index, facts, state, "", installed, prefs});
    ASSERT_EQ(tiles.size(), 2u);
    EXPECT_EQ(index.games[static_cast<std::size_t>(tiles[0].game)].title, "Beta");
    EXPECT_EQ(index.games[static_cast<std::size_t>(tiles[1].game)].title, "Gamma");
    // Without a firmware the filter cannot say anything: nothing is hidden.
    prefs.firmware.clear();
    tiles = select_games(GridQuery{p, {}, index, facts, state, "", installed, prefs});
    EXPECT_EQ(tiles.size(), 3u);
    // Every sort order gives each game once.
    for (int mode = 0; mode < kSortModeCount; ++mode)
    {
        state.sort = static_cast<SortMode>(mode);
        EXPECT_EQ(select_games(GridQuery{p, {}, index, facts, state, "", installed, prefs}).size(), 3u);
    }
}

TEST(GameGrid, FavouritesFromThePerPackageGridStillWorkAndStayInTheirFormat)
{
    const std::string path = std::string(::testing::TempDir()) + "wings_games_fav.v1";
    std::remove(path.c_str());
    // catalogue.v1 as the per-package grid wrote it: one key per package, no game keys.
    const std::string old_payload = "sort=0\ninstalled=0\nfav_only=0\ntier=0\nhas_links=0\nsource=\n"
                                    "fav=PPSA90026\nfav=PPSA90004\n";
    ASSERT_TRUE(hui::save::write_atomic(path, hui::save::encode(hui::save::Kind::prefs, 1, old_payload)).empty());
    {
        Rig r(path);
        StoreScreen &s = r.screen();
        const int starfall = r.tile_of("Starfall Odyssey");
        const int harbour = r.tile_of("Quiet Harbour");
        ASSERT_GE(starfall, 0);
        EXPECT_TRUE(s.tile_favourite(starfall)); // through its EUR listing
        EXPECT_TRUE(s.tile_favourite(harbour));
        // Square on the game: off removes every listing's key.
        r.focus_tile(starfall);
        ASSERT_EQ(s.focus_index(), starfall);
        r.press(hui::Action::west);
        EXPECT_FALSE(s.tile_favourite(starfall));
        EXPECT_EQ(s.catalogue_state().favourites, (std::vector<std::string>{"PPSA90004"}));
        // On again: the first listing's key, the same scheme as before.
        r.press(hui::Action::west);
        EXPECT_TRUE(s.tile_favourite(starfall));
        EXPECT_EQ(s.catalogue_state().favourites, (std::vector<std::string>{"PPSA90004", "PPSA90025"}));
    }
    CatalogueState loaded;
    ASSERT_TRUE(load_catalogue_state({path}, &loaded));
    EXPECT_EQ(loaded.favourites, (std::vector<std::string>{"PPSA90004", "PPSA90025"}));
    const std::string written = encode_catalogue_state(loaded);
    EXPECT_EQ(written.find("fav=g:"), std::string::npos);
    EXPECT_NE(written.find("fav=PPSA90025\n"), std::string::npos);
    // The new filter keys round-trip; a file without them reads as "off".
    loaded.region = "EUR";
    loaded.runs_on_my_fw = true;
    loaded.no_browser = true;
    CatalogueState back;
    ASSERT_TRUE(decode_catalogue_state(encode_catalogue_state(loaded), &back));
    EXPECT_EQ(back.region, "EUR");
    EXPECT_TRUE(back.runs_on_my_fw);
    EXPECT_TRUE(back.no_browser);
    CatalogueState old;
    ASSERT_TRUE(decode_catalogue_state(old_payload, &old));
    EXPECT_FALSE(old.filtered());
    std::remove(path.c_str());
}

// ---- the game page ----

TEST(GamePageView, SectionsSetsWrappersAndBestOnTheFakeGame)
{
    host::FakeBackend backend;
    LinkPrefs prefs;
    prefs.firmware = "10.xx";
    GameView v;
    v.title = "Starfall Odyssey";
    load_game_view(&v, starfall_details(backend), {}, prefs);
    ASSERT_TRUE(v.loaded);
    for (int s = 0; s < kSectionCount; ++s)
        EXPECT_FALSE(v.page.sections[s].empty()) << section_label(static_cast<Section>(s));
    EXPECT_EQ(v.page.duplicate_links, 1); // the 1fichier file both sources list
    EXPECT_EQ(v.sources.size(), 3u);
    EXPECT_NE(listed_size_text(v).find("Listed size"), std::string::npos);
    EXPECT_NE(listed_size_text(v).find("Community Mirror"), std::string::npos);

    const StartedLinks none;
    const int viking = find_mirror(v, "VikingFile", 22);
    ASSERT_GE(viking, 0);
    EXPECT_EQ(v.page.mirrors[static_cast<std::size_t>(viking)].status, PartStatus::complete);
    MirrorCard card = mirror_card(v, viking, prefs, none);
    EXPECT_TRUE(card.multipart);
    EXPECT_NE(card.parts.find("22 parts"), std::string::npos);
    EXPECT_EQ(card.fit, FwFit::too_new);
    EXPECT_EQ(card.fw_note, "needs 12.xx");
    const int rootz = find_mirror(v, "Rootz", 3);
    ASSERT_GE(rootz, 0);
    EXPECT_EQ(mirror_card(v, rootz, prefs, none).warning, "part 3 missing");
    const int restart = find_mirror(v, "Akirabox", 5);
    ASSERT_GE(restart, 0);
    EXPECT_NE(mirror_card(v, restart, prefs, none).warning.find("restarts"), std::string::npos);
    // A decrypt-page URL: the host the name gives, and where the URL goes.
    const int akia = find_mirror_named(v, "EUR exFAT 01.005 Base - AKIA (exFAT)");
    ASSERT_GE(akia, 0);
    card = mirror_card(v, akia, prefs, none);
    EXPECT_EQ(card.host, "Akirabox");
    EXPECT_TRUE(card.wrapped);
    EXPECT_EQ(card.via, "pippo26442999.github.io");
    EXPECT_EQ(card.format, "exFAT");
    EXPECT_EQ(card.version, "v01.005");
    // Best: the debrid backport that runs on 10.xx, one file.
    ASSERT_GE(v.page.best, 0);
    card = mirror_card(v, v.page.best, prefs, none);
    EXPECT_EQ(card.host, "1fichier");
    EXPECT_TRUE(card.best);
    EXPECT_EQ(card.fw_note, "runs on 4.xx+");
    EXPECT_NE(best_summary(v).find("Debrid"), std::string::npos);
    EXPECT_NE(best_summary(v).find("1 file"), std::string::npos);
    EXPECT_FALSE(v.page.best_why.empty());

    // With a firmware set, links too new for it are hidden by default and counted.
    EXPECT_TRUE(v.filter.runs_on_my_fw);
    EXPECT_GT(v.shown.hidden_too_new, 0);
    EXPECT_EQ(v.shown.lowest_needed, "12.xx");
    ASSERT_GE(v.rows.size(), 3u);
    EXPECT_EQ(v.rows[0].kind, PageRowKind::best);
    EXPECT_EQ(v.rows[1].kind, PageRowKind::filters);
    EXPECT_EQ(v.rows[2].kind, PageRowKind::hidden);
    expect_rows_consistent(v);

    // The filter panel: only facets with two or more values, Reset last.
    std::vector<LinkFilterRow> rows = link_filter_rows(v.facets, v.filter, prefs);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.front(), LinkFilterRow::runs_on_my_fw);
    EXPECT_EQ(rows.back(), LinkFilterRow::reset);
    EXPECT_NE(std::find(rows.begin(), rows.end(), LinkFilterRow::region), rows.end());
    EXPECT_NE(std::find(rows.begin(), rows.end(), LinkFilterRow::host), rows.end());
    // Everything back, then a region nobody has: the empty state, and Reset.
    MirrorFilter all;
    set_game_filter(&v, all, prefs);
    EXPECT_EQ(static_cast<int>(v.shown.mirrors.size()), v.facets.total);
    expect_rows_consistent(v);
    MirrorFilter eur;
    cycle_link_filter(&eur, LinkFilterRow::region, 1, v.facets);
    EXPECT_EQ(eur.region, "USA");
    cycle_link_filter(&eur, LinkFilterRow::region, 1, v.facets);
    EXPECT_EQ(eur.region, "EUR");
    set_game_filter(&v, eur, prefs);
    EXPECT_LT(static_cast<int>(v.shown.mirrors.size()), v.facets.total);
    EXPECT_EQ(link_filter_summary(v.filter, prefs), "EUR");
    MirrorFilter nothing;
    nothing.region = "JPN";
    set_game_filter(&v, nothing, prefs);
    EXPECT_TRUE(v.shown.mirrors.empty());
    EXPECT_NE(std::find_if(v.rows.begin(), v.rows.end(),
                           [](const PageRow &r) { return r.kind == PageRowKind::empty; }),
              v.rows.end());
    cycle_link_filter(&nothing, LinkFilterRow::reset, 1, v.facets);
    EXPECT_FALSE(nothing.active());

    // No firmware set: nothing hidden, no firmware colour.
    GameView plain;
    load_game_view(&plain, starfall_details(backend), {}, LinkPrefs{});
    EXPECT_FALSE(plain.filter.runs_on_my_fw);
    EXPECT_EQ(plain.shown.hidden_too_new, 0);
    EXPECT_FALSE(mirror_card(plain, viking, LinkPrefs{}, none).fit_known);
}

TEST(GamePageView, DownloadAllPartsPlansInCatalogueOrderAndSkipsWhatThisAppQueued)
{
    host::FakeBackend backend;
    GameView v;
    load_game_view(&v, starfall_details(backend), {}, LinkPrefs{});
    const int viking = find_mirror(v, "VikingFile", 22);
    ASSERT_GE(viking, 0);
    StartedLinks started;
    PartsPlan plan = plan_parts(v, viking, started);
    ASSERT_EQ(plan.refs.size(), 22u);
    EXPECT_EQ(plan.total, 22);
    EXPECT_FALSE(plan.one_at_a_time);
    EXPECT_EQ(plan.action, "Download all 22 parts");
    EXPECT_EQ(plan.title, "Download 22 parts from VikingFile?");
    EXPECT_NE(plan.message.find("22 parts from VikingFile"), std::string::npos);
    for (int i = 0; i < 22; ++i)
    {
        EXPECT_EQ(plan.refs[static_cast<std::size_t>(i)].package_id, "pkg25");
        EXPECT_EQ(plan.refs[static_cast<std::size_t>(i)].link_index, i);
    }
    started.add(plan.refs[0]);
    started.add(plan.refs[3]);
    plan = plan_parts(v, viking, started);
    EXPECT_EQ(plan.refs.size(), 20u);
    EXPECT_EQ(plan.skipped, 2);
    EXPECT_EQ(plan.title, "Download 20 parts from VikingFile?");
    EXPECT_NE(plan.message.find("2 parts this app already queued"), std::string::npos);
    EXPECT_EQ(mirror_card(v, viking, LinkPrefs{}, started).started, 2);
    // Parts that open the PS5 browser go one at a time: the next one only.
    const int akira = find_mirror(v, "Akirabox", 4);
    ASSERT_GE(akira, 0);
    plan = plan_parts(v, akira, started);
    EXPECT_TRUE(plan.one_at_a_time);
    ASSERT_EQ(plan.refs.size(), 1u);
    EXPECT_EQ(plan.action, "Download the next part (part 1)");
    started.add(plan.refs.front());
    EXPECT_EQ(plan_parts(v, akira, started).action, "Download the next part (part 2)");
}

TEST(GamePageScreen, DownloadAllPartsConfirmsThenQueuesEachPartAndReportsFailures)
{
    Rig r;
    StoreScreen &s = r.screen();
    s.open_package("pkg26"); // any listing opens the whole game
    r.work();
    ASSERT_TRUE(s.detail_open());
    ASSERT_TRUE(s.game_page().loaded);
    EXPECT_EQ(s.game_page().package_ids, (std::vector<std::string>{"pkg25", "pkg26", "pkg27"}));
    const int viking = find_mirror(s.game_page(), "VikingFile", 22);
    ASSERT_GE(viking, 0);
    r.backend.set_failing_link("pkg25", 5);
    ASSERT_TRUE(s.focus_page_mirror(viking));
    r.press(hui::Action::confirm);
    ASSERT_TRUE(s.list_dialog().is_open());
    ASSERT_EQ(s.list_dialog().items().size(), 23u);
    EXPECT_EQ(s.list_dialog().items()[0].label, "Download all 22 parts");
    EXPECT_EQ(s.list_dialog().items()[1].label, "Part 1");
    r.press(hui::Action::confirm); // Download all
    ASSERT_TRUE(s.confirm_open());
    EXPECT_EQ(r.backend.started_links(), 0); // nothing before the confirmation
    r.press(hui::Action::confirm);
    EXPECT_TRUE(s.batch_running());
    r.work();
    EXPECT_FALSE(s.batch_running());
    EXPECT_EQ(r.backend.started_links(), 5); // one at a time: it stops at part 6
    EXPECT_EQ(s.started_links().keys.size(), 5u);
    EXPECT_TRUE(s.confirm_open()); // "5 of 22 parts queued", part 6 named
    r.press(hui::Action::confirm);
    EXPECT_FALSE(s.confirm_open());
    // Again: only the parts from the one that failed.
    r.backend.set_failing_link("none", -1);
    r.press(hui::Action::confirm);
    ASSERT_TRUE(s.list_dialog().is_open());
    EXPECT_EQ(s.list_dialog().items()[1].right, "Queued");
    EXPECT_EQ(s.list_dialog().items()[6].right, "");
    r.press(hui::Action::confirm);
    r.press(hui::Action::confirm);
    r.work();
    EXPECT_EQ(r.backend.started_links(), 22);
    EXPECT_EQ(s.started_links().keys.size(), 22u);
    EXPECT_FALSE(s.confirm_open());
    // Everything queued: the action says so and does nothing.
    r.press(hui::Action::confirm);
    ASSERT_TRUE(s.list_dialog().is_open());
    EXPECT_EQ(s.list_dialog().items()[0].label, "Every part is already queued");
    EXPECT_FALSE(s.list_dialog().items()[0].enabled);
    r.press(hui::Action::back);
    EXPECT_FALSE(s.list_dialog().is_open());
}

// The first-download folder question, reached through a multi-part set that opens the PS5 browser: it must go
// away once answered and let the download carry on.
TEST(GamePageScreen, TheFirstDownloadQuestionClosesAfterABrowserPartAndTheDownloadGoesOn)
{
    Rig r;
    bool confirmed = false;
    r.context.folder_confirmed = [&confirmed] { return confirmed; };
    r.context.confirm_folder = [&confirmed] { confirmed = true; };
    StoreScreen &s = r.screen();
    s.open_package("pkg26");
    r.work();
    ASSERT_TRUE(s.detail_open());
    const int akira = find_mirror(s.game_page(), "Akirabox", 4);
    ASSERT_GE(akira, 0);
    ASSERT_TRUE(s.focus_page_mirror(akira));
    // Walk the dialogs (parts list, confirmations) until the folder question shows up.
    int guard = 0;
    while (!s.folder_ask_open() && guard++ < 8)
    {
        r.press(hui::Action::confirm);
        r.work();
    }
    ASSERT_TRUE(s.folder_ask_open());
    EXPECT_EQ(r.backend.started_links(), 0); // nothing before the answer
    r.press(hui::Action::confirm);           // "Keep this folder"
    r.work();
    EXPECT_TRUE(confirmed);
    EXPECT_FALSE(s.folder_ask_open()); // gone
    // ... and it did not come back by itself.
    for (int i = 0; i < 5; ++i)
        r.frame();
    EXPECT_FALSE(s.folder_ask_open());
    r.work();
    EXPECT_GE(r.backend.started_links(), 1);
}

TEST(GamePageScreen, TheBestCardRunsTheExistingLinkFlowAndOptionsOpensTheFilters)
{
    Rig r;
    StoreScreen &s = r.screen();
    WingsLocalPrefs prefs;
    prefs.firmware = "10.xx";
    s.set_prefs(prefs);
    s.open_package("pkg25");
    r.work();
    ASSERT_TRUE(s.game_page().loaded);
    const PageRow *row = s.focused_page_row();
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->kind, PageRowKind::best);
    EXPECT_TRUE(s.game_page().filter.runs_on_my_fw);
    EXPECT_TRUE(s.header_focused()); // the page opens on the header's Install button
    r.frame(Direction::down);        // ... and Down goes to the downloads
    EXPECT_FALSE(s.header_focused());
    const std::size_t jobs = r.store.state().jobs.size();
    r.press(hui::Action::confirm); // a debrid link: straight to the queue
    r.work();
    EXPECT_EQ(r.store.state().jobs.size(), jobs + 1);
    EXPECT_TRUE(s.started_links().has(s.game_page().first_ref(s.game_page().page.best)));
    // Down goes to Filters, Cross (or Options) opens the panel there.
    r.frame(Direction::down);
    ASSERT_EQ(s.focused_page_row()->kind, PageRowKind::filters);
    r.press(hui::Action::confirm);
    EXPECT_TRUE(s.link_filters_open());
    r.frame(Direction::right); // Runs on my firmware: off
    r.frame();
    EXPECT_FALSE(s.game_page().filter.runs_on_my_fw);
    EXPECT_EQ(s.game_page().shown.hidden_too_new, 0);
    r.press(hui::Action::back);
    EXPECT_FALSE(s.link_filters_open());
    r.press(hui::Action::menu);
    EXPECT_TRUE(s.link_filters_open());
    r.press(hui::Action::back);
    // Preferences change while the page is open: ranked again.
    prefs.firmware.clear();
    s.set_prefs(prefs);
    r.frame();
    EXPECT_FALSE(mirror_card(s.game_page(), s.game_page().page.best, LinkPrefs{}, {}).fit_known);
    r.press(hui::Action::back);
    EXPECT_FALSE(s.detail_open());
}

TEST(GamePageScreen, TheHeaderHasInstallFavouriteAndMoreAndInstallSlidesToTheDownloads)
{
    Rig r;
    StoreScreen &s = r.screen();
    s.open_package("pkg25");
    r.work();
    ASSERT_TRUE(s.game_page().loaded);
    EXPECT_TRUE(s.header_focused());
    EXPECT_EQ(s.header_button(), static_cast<int>(HeaderButton::install));
    const std::size_t jobs = r.store.state().jobs.size();
    // Install does not queue anything: it moves to the list of downloads.
    r.press(hui::Action::confirm);
    EXPECT_FALSE(s.header_focused());
    EXPECT_EQ(r.store.state().jobs.size(), jobs);
    EXPECT_FALSE(s.modal());
    ASSERT_NE(s.focused_page_row(), nullptr);
    EXPECT_EQ(s.focused_page_row()->kind, PageRowKind::best);
    // Up from the first card goes back to the header.
    r.frame(Direction::up);
    EXPECT_TRUE(s.header_focused());
    // Right: Favourite toggles (the existing Square shortcut does the same).
    r.frame(Direction::right);
    EXPECT_EQ(s.header_button(), static_cast<int>(HeaderButton::favourite));
    r.press(hui::Action::confirm);
    EXPECT_TRUE(s.catalogue_state().favourites.size() > 0u);
    r.press(hui::Action::west);
    EXPECT_TRUE(s.catalogue_state().favourites.empty());
    // Right again: the three dots open a menu with the page's other actions.
    r.frame(Direction::right);
    EXPECT_EQ(s.header_button(), static_cast<int>(HeaderButton::more));
    r.frame(Direction::right); // the last button: nowhere further
    EXPECT_EQ(s.header_button(), static_cast<int>(HeaderButton::more));
    r.press(hui::Action::confirm);
    EXPECT_TRUE(s.list_dialog().is_open());
    r.press(hui::Action::confirm); // the first: the link filters
    EXPECT_TRUE(s.link_filters_open());
    r.press(hui::Action::back);
    EXPECT_FALSE(s.link_filters_open());
    EXPECT_TRUE(s.header_focused());
    // Circle closes the page from the header too.
    r.press(hui::Action::back);
    EXPECT_FALSE(s.detail_open());
}

// ---- bug F: the browser-capture panel's D-pad ----

TEST(CapturePanel, LeftAndRightFocusTheButtonDrawnOnThatSide)
{
    Rig r;
    r.backend.set_capture_steps(1000); // stays "watching"
    r.store.start_link("pkg1", 2, ""); // "Mirror (needs a check)": a browser capture
    r.work();
    ASSERT_TRUE(r.app->capture_panel_open());
    // draw_capture(): Hide (0) is the right-hand button, Cancel capture (1) left of it.
    EXPECT_EQ(r.app->capture_button(), 0);
    r.frame(Direction::left);
    EXPECT_EQ(r.app->capture_button(), 1);
    r.frame(Direction::left); // already at the left edge
    EXPECT_EQ(r.app->capture_button(), 1);
    r.frame(Direction::right);
    EXPECT_EQ(r.app->capture_button(), 0);
    r.frame(Direction::right); // already at the right edge
    EXPECT_EQ(r.app->capture_button(), 0);
    // Cross on the left-hand button cancels; on the right-hand one it only hides.
    r.frame(Direction::left);
    r.press(hui::Action::confirm);
    r.work();
    EXPECT_EQ(r.backend.cancelled_captures(), 1);
}

// ---- the whole recorded catalogue ----

std::string samples_dir()
{
    if (const char *env = std::getenv("WINGS_SAMPLES"))
        return env;
    return std::string(HUI_SOURCE_DIR) + "/../../recon/samples";
}

TEST(GameViewLibrary, EveryGameOfTheRecordedCatalogueBuildsThroughTheViewModel)
{
    std::ifstream file(samples_dir() + "/library.json", std::ios::binary);
    if (!file)
        GTEST_SKIP() << "recon/samples/library.json is not reachable from here";
    std::ostringstream text;
    text << file.rdbuf();
    auto replay = std::make_shared<wings::http::ReplayTransport>();
    replay->add("GET", "/api/health", 200,
                R"({"status":"running","port":6970,"downloadOnly":false,"nativePkgHandoff":true,"version":"1.11.0"})");
    replay->add("GET", "/api/library", 200, text.str());
    PegasusBackend backend(replay, "127.0.0.1:6970");
    const Result<std::vector<Package>> all = backend.library();
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all.value.size(), 2962u);
    const auto start = std::chrono::steady_clock::now();
    std::vector<PackageDetail> details;
    std::vector<PackageFacts> facts;
    for (const Package &p : all.value)
    {
        const Result<PackageDetail> d = backend.package(p.id);
        ASSERT_TRUE(d.ok());
        facts.push_back(package_facts(d.value));
        details.push_back(d.value);
    }
    const double facts_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // The grid: every game once in every order; the filters narrow it.
    const auto t_grid = std::chrono::steady_clock::now();
    const GameIndex index = group_games(all.value);
    const std::vector<std::uint8_t> tiers = backend.package_tiers().value;
    LinkPrefs prefs;
    prefs.firmware = "10.xx";
    prefs.region = "USA";
    CatalogueState state;
    InstalledIndex installed;
    for (int mode = 0; mode < kSortModeCount; ++mode)
    {
        state.sort = static_cast<SortMode>(mode);
        EXPECT_EQ(select_games(GridQuery{all.value, tiers, index, facts, state, "", installed, prefs}).size(),
                  index.games.size());
    }
    state.sort = SortMode::az;
    state.runs_on_my_fw = true;
    const std::size_t runs = select_games(GridQuery{all.value, tiers, index, facts, state, "", installed, prefs}).size();
    state.runs_on_my_fw = false;
    state.region = "JPN";
    const std::size_t jpn = select_games(GridQuery{all.value, tiers, index, facts, state, "", installed, prefs}).size();
    state.region.clear();
    state.no_browser = true;
    const std::size_t no_browser =
        select_games(GridQuery{all.value, tiers, index, facts, state, "", installed, prefs}).size();
    const std::vector<std::string> regions = catalogue_regions(index, facts);
    const double grid_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_grid).count();
    EXPECT_GT(runs, 0u);
    EXPECT_LT(runs, index.games.size());
    EXPECT_GT(jpn, 0u);
    EXPECT_LT(jpn, index.games.size());
    EXPECT_GT(no_browser, 0u);
    EXPECT_GE(regions.size(), 6u);
    EXPECT_EQ(regions.front(), "USA");

    // Every game page: built, laid out, every card and every filter value.
    const auto t_pages = std::chrono::steady_clock::now();
    long pages = 0, cards = 0, sets = 0, best = 0, filter_steps = 0, empty_pages = 0;
    for (const GameGroup &g : index.games)
    {
        std::vector<PackageDetail> d;
        for (const int p : g.packages)
            d.push_back(details[static_cast<std::size_t>(p)]);
        GameView v;
        v.title = g.title;
        load_game_view(&v, std::move(d), {}, prefs);
        ++pages;
        expect_rows_consistent(v);
        if (v.page.mirrors.empty())
        {
            ++empty_pages;
            ASSERT_EQ(v.rows.size(), 1u);
            EXPECT_EQ(v.rows[0].kind, PageRowKind::empty);
            continue;
        }
        EXPECT_EQ(v.rows[0].kind == PageRowKind::best, v.page.best >= 0) << g.title;
        best += v.page.best >= 0 ? 1 : 0;
        const StartedLinks none;
        for (int m = 0; m < static_cast<int>(v.page.mirrors.size()); ++m)
        {
            const MirrorCard c = mirror_card(v, m, prefs, none);
            EXPECT_FALSE(c.host.empty());
            EXPECT_FALSE(c.parts.empty());
            ASSERT_NE(v.link(v.first_ref(m)), nullptr) << g.title;
            ++cards;
            if (c.multipart)
            {
                ++sets;
                const PartsPlan plan = plan_parts(v, m, none);
                EXPECT_EQ(plan.total, c.part_count);
                EXPECT_EQ(plan.refs.size(), plan.one_at_a_time ? 1u : static_cast<std::size_t>(c.part_count));
                EXPECT_FALSE(plan.action.empty());
            }
        }
        // Every value of every filter row the panel would show.
        for (const LinkFilterRow row : link_filter_rows(v.facets, MirrorFilter{}, prefs))
        {
            if (row == LinkFilterRow::reset || row == LinkFilterRow::sort)
                continue;
            MirrorFilter f;
            for (int step = 0; step < 12; ++step)
            {
                cycle_link_filter(&f, row, 1, v.facets);
                set_game_filter(&v, f, prefs);
                EXPECT_LE(static_cast<int>(v.shown.mirrors.size()), v.facets.total);
                EXPECT_FALSE(link_filter_value(row, f, v.facets, prefs).empty());
                ++filter_steps;
                if (!f.active())
                    break;
            }
        }
        expect_rows_consistent(v);
    }
    const double page_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_pages).count();
    std::printf("[game view] facts for %zu packages in %.2f s; grid of %zu games (runs on 10.xx %zu, JPN %zu, "
                "no browser %zu, %zu regions) in %.2f s; %ld pages (%ld without links), %ld cards, %ld part sets, "
                "%ld with Best, %ld filter steps in %.2f s\n",
                all.value.size(), facts_seconds, index.games.size(), runs, jpn, no_browser, regions.size(),
                grid_seconds, pages, empty_pages, cards, sets, best, filter_steps, page_seconds);
    EXPECT_EQ(pages, static_cast<long>(index.games.size()));
    EXPECT_GT(sets, 1000);
    EXPECT_LT(grid_seconds, 10.0);
}

} // namespace
} // namespace orbit
