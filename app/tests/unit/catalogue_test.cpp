// Wings TV app - The Store grid: sort, filters, favourites, installed status.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The logic (catalogue_view.*) is tested on synthetic packages and on the
// recorded Pegasus samples in recon/samples (appdb.json, library.json); the
// screen on the fake backend.

#include "fake_backend.hpp"
#include "orbit/catalogue_view.hpp"
#include "orbit/core_transport.hpp"
#include "orbit/pegasus_backend.hpp"
#include "orbit/screens.hpp"
#include "orbit/store.hpp"
#include "support.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

namespace orbit
{
namespace
{

// ---- synthetic data ----

Package pkg(const char *id, const char *title, const char *title_id, const char *source,
            int links = 1, bool has_pkg = false, bool automated = false, bool supported = false)
{
    Package p;
    p.id = id;
    p.title = title;
    p.title_id = title_id;
    p.source_name = source;
    p.link_count = links;
    p.has_pkg = has_pkg;
    p.has_automated = automated;
    p.has_supported = supported;
    return p;
}

InstalledGame game(const char *title_id)
{
    InstalledGame g;
    g.title_id = title_id;
    return g;
}

std::vector<Package> sample_packages()
{
    return {
        pkg("1", "Bravo", "PPSA00002", "Beta", 1, true),            // 0 pkg
        pkg("2", "alpha", "PPSA00001", "Alpha", 2, false, true),    // 1 automated
        pkg("3", "Charlie", "PPSA00003", "Beta", 1, false, false, true), // 2 supported
        pkg("4", "Delta", "PPSA00004", "Alpha", 3),                 // 3 unknown
        pkg("5", "Echo", "", "Gamma", 0),                           // 4 no links, no title id
        pkg("6", "Foxtrot", "PPSA00006", "Gamma", 2),               // 5 debrid via tiers
    };
}

// Packed tiers for sample_packages(): what the backend would say with a debrid
// service ready for Foxtrot's host (and Delta's second link).
std::vector<std::uint8_t> sample_tiers()
{
    return {pack_tier(LinkTier::native_pkg, false), pack_tier(LinkTier::automated, false),
            pack_tier(LinkTier::supported, false),  pack_tier(LinkTier::debrid, true),
            pack_tier(LinkTier::unsupported, false), pack_tier(LinkTier::debrid, true)};
}

std::vector<std::string> titles(const std::vector<Package> &packages, const std::vector<int> &order)
{
    std::vector<std::string> out;
    for (const int i : order)
        out.push_back(packages[static_cast<std::size_t>(i)].title);
    return out;
}

InstalledIndex installed_index(std::initializer_list<const char *> ids, bool known = true)
{
    std::vector<InstalledGame> games;
    for (const char *id : ids)
        games.push_back(game(id));
    InstalledIndex index;
    index.assign(games, known);
    return index;
}

using Strings = std::vector<std::string>;

// ---- sort ----

TEST(CatalogueSort, EverySortModeOrdersAsNamedAndTiesBreakByTitle)
{
    const std::vector<Package> p = sample_packages();
    const std::vector<std::uint8_t> tiers = sample_tiers();
    const InstalledIndex installed = installed_index({"PPSA00003", "PPSA00001"});
    CatalogueState state;
    state.toggle_favourite(favourite_key(p[3])); // Delta
    state.toggle_favourite(favourite_key(p[0])); // Bravo
    const auto run = [&](SortMode mode)
    {
        state.sort = mode;
        return titles(p, select_packages(p, tiers, state, "", installed));
    };
    EXPECT_EQ(run(SortMode::az), (Strings{"alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"}));
    EXPECT_EQ(run(SortMode::za), (Strings{"Foxtrot", "Echo", "Delta", "Charlie", "Bravo", "alpha"}));
    // installed (alpha, Charlie), then not installed, then unknown (Echo has no id)
    EXPECT_EQ(run(SortMode::installed_first),
              (Strings{"alpha", "Charlie", "Bravo", "Delta", "Foxtrot", "Echo"}));
    EXPECT_EQ(run(SortMode::not_installed_first),
              (Strings{"Bravo", "Delta", "Foxtrot", "alpha", "Charlie", "Echo"}));
    EXPECT_EQ(run(SortMode::favourites_first),
              (Strings{"Bravo", "Delta", "alpha", "Charlie", "Echo", "Foxtrot"}));
    // native, automated, supported, then debrid (Delta, Foxtrot), unsupported
    EXPECT_EQ(run(SortMode::tier_first),
              (Strings{"Bravo", "alpha", "Charlie", "Delta", "Foxtrot", "Echo"}));
    EXPECT_EQ(run(SortMode::source),
              (Strings{"alpha", "Delta", "Bravo", "Charlie", "Echo", "Foxtrot"}));
}

TEST(CatalogueSort, TheListIsDeterministicWhateverTheInputOrder)
{
    std::vector<Package> a = sample_packages();
    std::vector<Package> b = a;
    std::reverse(b.begin(), b.end());
    const CatalogueState state;
    const InstalledIndex none;
    EXPECT_EQ(titles(a, select_packages(a, {}, state, "", none)),
              titles(b, select_packages(b, {}, state, "", none)));
}

// ---- filters ----

TEST(CatalogueFilter, EachFilterNarrowsAndTheyCombine)
{
    const std::vector<Package> p = sample_packages();
    const std::vector<std::uint8_t> tiers = sample_tiers();
    const InstalledIndex installed = installed_index({"PPSA00003", "PPSA00001", "ppsa00006"});
    CatalogueState state;
    EXPECT_FALSE(state.filtered());
    state.source = "Beta";
    EXPECT_TRUE(state.filtered());
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "", installed)), (Strings{"Bravo", "Charlie"}));
    state.reset_filters();
    state.installed = InstalledFilter::installed;
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "", installed)),
              (Strings{"alpha", "Charlie", "Foxtrot"})); // case of the id does not matter
    state.installed = InstalledFilter::not_installed;
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "", installed)), (Strings{"Bravo", "Delta"}));
    state.reset_filters();
    state.favourites_only = true;
    EXPECT_TRUE(select_packages(p, tiers, state, "", installed).empty());
    state.toggle_favourite(favourite_key(p[2]));
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "", installed)), (Strings{"Charlie"}));
    state.reset_filters(); // favourites stay
    EXPECT_TRUE(state.is_favourite(favourite_key(p[2])));
    state.tier = TierFilter::supported;
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "", installed)),
              (Strings{"alpha", "Bravo", "Charlie"}));
    state.tier = TierFilter::debrid; // a link a ready debrid service takes
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "", installed)), (Strings{"Delta", "Foxtrot"}));
    state.reset_filters();
    state.has_links_only = true;
    EXPECT_EQ(select_packages(p, tiers, state, "", installed).size(), 5u);
    // Combined with the search, which is not part of the persisted state.
    state.source = "Gamma";
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "fox", installed)), (Strings{"Foxtrot"}));
    EXPECT_EQ(titles(p, select_packages(p, tiers, state, "PPSA00006", installed)), (Strings{"Foxtrot"}));
    EXPECT_TRUE(select_packages(p, tiers, state, "zzz", installed).empty());
}

TEST(CatalogueFilter, UnknownInstalledStatusIsNeverGuessed)
{
    const std::vector<Package> p = sample_packages();
    InstalledIndex unloaded; // the game database has not been read
    EXPECT_FALSE(unloaded.known);
    EXPECT_EQ(unloaded.status("PPSA00001"), -1);
    CatalogueState state;
    state.installed = InstalledFilter::installed;
    EXPECT_TRUE(select_packages(p, {}, state, "", unloaded).empty());
    state.installed = InstalledFilter::not_installed;
    EXPECT_TRUE(select_packages(p, {}, state, "", unloaded).empty());
    // Read, but this package has no title id: still unknown, never "not installed".
    const InstalledIndex loaded = installed_index({"PPSA00001"});
    EXPECT_EQ(loaded.status(""), -1);
    EXPECT_EQ(loaded.status("PPSA00001"), 1);
    EXPECT_EQ(loaded.status(" ppsa00001 "), 1);
    EXPECT_EQ(loaded.status("PPSA99999"), 0);
}

TEST(CatalogueTier, FallsBackToTheFlagsWhenTheBackendHasNoClassification)
{
    const std::vector<Package> p = sample_packages();
    EXPECT_EQ(packed_tier(tier_of(p, {}, 0)), LinkTier::native_pkg);
    EXPECT_EQ(packed_tier(tier_of(p, {}, 1)), LinkTier::automated);
    EXPECT_EQ(packed_tier(tier_of(p, {}, 2)), LinkTier::supported);
    EXPECT_EQ(packed_tier(tier_of(p, {}, 3)), LinkTier::unknown);
    EXPECT_EQ(packed_tier(tier_of(p, {}, 4)), LinkTier::unsupported);
    // A classification of another length (a stale one) is ignored.
    const std::vector<std::uint8_t> stale = {pack_tier(LinkTier::debrid, true)};
    EXPECT_EQ(packed_tier(tier_of(p, stale, 0)), LinkTier::native_pkg);
    const std::vector<std::uint8_t> tiers = sample_tiers();
    EXPECT_EQ(packed_tier(tier_of(p, tiers, 5)), LinkTier::debrid);
    EXPECT_TRUE(packed_via_debrid(tier_of(p, tiers, 5)));
    EXPECT_FALSE(packed_via_debrid(tier_of(p, tiers, 0)));
}

// ---- persistence ----

TEST(CataloguePersistence, StateRoundTripsThroughTheSaveContainer)
{
    CatalogueState state;
    state.sort = SortMode::tier_first;
    state.installed = InstalledFilter::not_installed;
    state.favourites_only = true;
    state.tier = TierFilter::debrid;
    state.has_links_only = true;
    state.source = "Community Mirror";
    state.toggle_favourite("PPSA00002");
    state.toggle_favourite("PPSA00001");
    state.toggle_favourite("title:no id game");
    const std::string path = std::string(::testing::TempDir()) + "wings_catalogue_test.v1";
    std::remove(path.c_str());
    ASSERT_EQ(save_catalogue_state({path}, state), 1);
    CatalogueState loaded;
    ASSERT_TRUE(load_catalogue_state({"/nonexistent/none.v1", path}, &loaded));
    EXPECT_EQ(loaded.sort, SortMode::tier_first);
    EXPECT_EQ(loaded.installed, InstalledFilter::not_installed);
    EXPECT_TRUE(loaded.favourites_only);
    EXPECT_EQ(loaded.tier, TierFilter::debrid);
    EXPECT_TRUE(loaded.has_links_only);
    EXPECT_EQ(loaded.source, "Community Mirror");
    EXPECT_EQ(loaded.favourites, state.favourites);
    EXPECT_TRUE(loaded.is_favourite("PPSA00001"));
    EXPECT_TRUE(std::is_sorted(loaded.favourites.begin(), loaded.favourites.end()));
    // A file that is damaged is not trusted (the checked container).
    {
        std::ifstream in(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ASSERT_GT(data.size(), 20u);
        data[data.size() - 3] ^= 0x55;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << data;
    }
    CatalogueState untouched;
    untouched.source = "kept";
    EXPECT_FALSE(load_catalogue_state({path}, &untouched));
    EXPECT_EQ(untouched.source, "kept");
    std::remove(path.c_str());
}

TEST(CataloguePersistence, OutOfRangeValuesDropToDefaultsAndFavouritesToggle)
{
    CatalogueState state;
    ASSERT_TRUE(decode_catalogue_state("sort=99\ninstalled=7\ntier=x\nfav_only=1\nfav=B\nfav=A\nfav=A\n"
                                       "junk line\nfav=\n",
                                       &state));
    EXPECT_EQ(state.sort, SortMode::az);
    EXPECT_EQ(state.installed, InstalledFilter::any);
    EXPECT_EQ(state.tier, TierFilter::any);
    EXPECT_TRUE(state.favourites_only);
    EXPECT_EQ(state.favourites, (Strings{"A", "B"}));
    EXPECT_FALSE(state.toggle_favourite("A"));
    EXPECT_TRUE(state.toggle_favourite("C"));
    EXPECT_EQ(state.favourites, (Strings{"B", "C"}));
}

TEST(CatalogueFavourite, KeyIsTheTitleIdElseTheTitleAndSurvivesReloads)
{
    Package a = pkg("17", "Some Game", " ppsa01234 ", "S");
    Package b = pkg("9001", "Some Game", "PPSA01234", "Other source");
    EXPECT_EQ(favourite_key(a), favourite_key(b)); // another package id, same game
    Package c = pkg("5", "No Id Game", "", "S");
    EXPECT_EQ(favourite_key(c), "title:no id game");
}

// ---- recorded samples ----

std::string samples_dir()
{
    if (const char *env = std::getenv("WINGS_SAMPLES"))
        return env;
    return std::string(HUI_SOURCE_DIR) + "/../../recon/samples";
}

bool read_sample(const std::string &name, std::string *out)
{
    std::ifstream file(samples_dir() + "/" + name, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    *out = text.str();
    return true;
}

#define NEED_SAMPLE(var, name)                                                                     \
    std::string var;                                                                               \
    if (!read_sample(name, &var))                                                                  \
        GTEST_SKIP() << "recon/samples/" name " is not reachable from here";

constexpr const char *kHealth =
    R"({"status":"running","port":6970,"downloadOnly":false,"nativePkgHandoff":true,"version":"1.11.0"})";

struct SampleRig
{
    std::shared_ptr<wings::http::ReplayTransport> replay =
        std::make_shared<wings::http::ReplayTransport>();
    PegasusBackend backend{replay, "127.0.0.1:6970"};
    SampleRig()
    {
        replay->add("GET", "/api/health", 200, kHealth);
    }
};

TEST(CatalogueInstalled, MatchesTheTitleIdsOfTheRecordedAppdb)
{
    NEED_SAMPLE(appdb, "appdb.json");
    SampleRig rig;
    rig.replay->add("GET", "/appdb", 200, appdb);
    const Result<std::vector<InstalledGame>> games = rig.backend.installed();
    ASSERT_TRUE(games.ok());
    ASSERT_FALSE(games.value.empty());
    // Every "titleId" in the JSON is an installed game.
    std::size_t in_json = 0;
    for (std::size_t at = appdb.find("\"titleId\":\""); at != std::string::npos;
         at = appdb.find("\"titleId\":\"", at + 1))
        ++in_json;
    EXPECT_EQ(games.value.size(), in_json);
    InstalledIndex index;
    index.assign(games.value, true);
    EXPECT_TRUE(index.known);
    for (const InstalledGame &g : games.value)
        EXPECT_EQ(index.status(g.title_id), 1) << g.title_id;
    // The first sample entry is PPSA27366 (AI LIMIT); an id that is not there is "not installed".
    EXPECT_EQ(index.status("PPSA27366"), 1);
    EXPECT_EQ(index.status("PPSA00000"), 0);

    // A catalogue made from some of those games and some that are not.
    std::vector<Package> packages;
    packages.push_back(pkg("a", "Installed one", games.value[0].title_id.c_str(), "S"));
    packages.push_back(pkg("b", "Missing one", "PPSA00000", "S"));
    CatalogueState state;
    state.installed = InstalledFilter::installed;
    EXPECT_EQ(select_packages(packages, {}, state, "", index), (std::vector<int>{0}));
    state.installed = InstalledFilter::not_installed;
    EXPECT_EQ(select_packages(packages, {}, state, "", index), (std::vector<int>{1}));
}

TEST(CatalogueTiersBackend, PackageTiersAgreeWithThePackagePageForTheWholeLibrary)
{
    NEED_SAMPLE(library, "library.json");
    SampleRig rig;
    rig.replay->add("GET", "/api/library", 200, library);
    const Result<std::vector<Package>> all = rig.backend.library();
    ASSERT_TRUE(all.ok());
    ASSERT_GT(all.value.size(), 1000u);
    const auto check = [&](const char *note)
    {
        const Result<std::vector<std::uint8_t>> tiers = rig.backend.package_tiers();
        ASSERT_TRUE(tiers.ok());
        ASSERT_EQ(tiers.value.size(), all.value.size()) << note;
        for (std::size_t i = 0; i < all.value.size(); ++i)
        {
            const Result<PackageDetail> detail = rig.backend.package(all.value[i].id);
            ASSERT_TRUE(detail.ok());
            ASSERT_EQ(packed_tier(tiers.value[i]), best_tier(detail.value.links))
                << note << " package " << all.value[i].id;
            bool via = false;
            for (const PackageLink &link : detail.value.links)
                via = via || link_tier(link) == LinkTier::debrid;
            ASSERT_EQ(packed_via_debrid(tiers.value[i]), via) << note << " " << all.value[i].id;
        }
    };
    check("no debrid account");
    // Package 27 has 1fichier.com links (see pegasus_backend_test): with TorBox ready
    // for that host the classification moves with the settings.
    rig.replay->set("GET", "/api/settings", 200,
                    R"({"downloadRoot":"/data/x","downloadConnections":8,"torBox":{"configured":true,"valid":true,"hosterCount":1,"domainCount":1,"domains":["1fichier.com"]}})");
    ASSERT_TRUE(rig.backend.settings().ok());
    check("torbox ready");
    const Result<std::vector<std::uint8_t>> tiers = rig.backend.package_tiers();
    long via = 0;
    for (const std::uint8_t t : tiers.value)
        via += packed_via_debrid(t) ? 1 : 0;
    EXPECT_GT(via, 0);

    // The whole catalogue sorts and filters in every mode well inside a frame budget
    // of seconds even under the sanitizers (it runs on change, not per frame).
    InstalledIndex none;
    CatalogueState state;
    const auto start = std::chrono::steady_clock::now();
    for (int mode = 0; mode < kSortModeCount; ++mode)
    {
        state.sort = static_cast<SortMode>(mode);
        const std::vector<int> order = select_packages(all.value, tiers.value, state, "", none);
        EXPECT_EQ(order.size(), all.value.size());
    }
    state.tier = TierFilter::debrid;
    state.has_links_only = true;
    EXPECT_FALSE(select_packages(all.value, tiers.value, state, "a", none).empty());
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(seconds, 5.0);
}

// ---- the screen on the fake backend ----

struct GridHarness
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    std::unique_ptr<App> app;
    Feedback feedback;

    explicit GridHarness(const std::string &catalogue_path = {}, bool load_installed = true)
    {
        context.app_version = "00.001.000";
        if (!catalogue_path.empty())
            context.catalogue_paths = {catalogue_path};
        app = std::make_unique<App>(context);
        store.poll_once();
        store.load_catalog_once();
        frame();
        if (load_installed)
        {
            store.poll_once(); // the Store screen asked for the game list
            frame();
        }
        settle(3);
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
    void settle(int n = 6)
    {
        for (int i = 0; i < n; ++i)
            frame();
    }
    void press(hui::Action action)
    {
        frame(Direction::none, hui::action_bit(action));
        settle(2);
    }
    void move(Direction d)
    {
        frame(d);
        settle(2);
    }
    StoreScreen &grid()
    {
        return app->store_screen();
    }
};

TEST(StoreGrid, DefaultIsAToZWithInstalledStatusFromTheGameList)
{
    GridHarness h;
    StoreScreen &grid = h.grid();
    ASSERT_EQ(grid.shown_count(), 25); // 27 packages, one game listed three times
    const std::vector<std::string> ids = grid.shown_ids();
    // A to Z by title: "Brass Meridian" (pkg18) first.
    EXPECT_EQ(ids.front(), "pkg18");
    EXPECT_EQ(grid.catalogue_state().sort, SortMode::az);
    // Lantern Road (pkg8) is an installed game in the fake database; Ember Hollow is not.
    const auto at = [&](const char *id)
    { return static_cast<int>(std::find(ids.begin(), ids.end(), id) - ids.begin()); };
    EXPECT_EQ(grid.tile_installed(at("pkg8")), 1);
    EXPECT_EQ(grid.tile_installed(at("pkg1")), 0);
}

TEST(StoreGrid, NothingIsClaimedWhileTheGameListHasNotLoaded)
{
    host::FakeBackend backend;
    Store store(backend);
    test::TestFonts fonts;
    test::FakeSink sink;
    ArtCache art(backend, sink);
    Context context(fonts.type, fonts.fonts, store, art);
    App app(context);
    store.poll_once(); // no screen has asked for /appdb yet
    store.load_catalog_once();
    hui::InputFrame idle;
    idle.connected = true;
    // One frame: the first update rebuilds with the catalogue but without the list.
    {
        Feedback feedback;
        // tick() delivers the poll's results; the Store screen then asks for /appdb.
        app.update(idle, 1.0f / 60.0f, feedback);
    }
    EXPECT_FALSE(store.state().have_installed);
    for (int i = 0; i < app.store_screen().shown_count(); ++i)
        EXPECT_EQ(app.store_screen().tile_installed(i), -1);
}

TEST(StoreGrid, SquareTogglesAFavouriteAndItIsPersisted)
{
    const std::string path = std::string(::testing::TempDir()) + "wings_grid_test.v1";
    std::remove(path.c_str());
    std::string first;
    {
        GridHarness h(path);
        first = h.grid().shown_ids().front();
        EXPECT_FALSE(h.grid().tile_favourite(0));
        h.press(hui::Action::west);
        EXPECT_TRUE(h.grid().tile_favourite(0));
        EXPECT_EQ(h.grid().catalogue_state().favourites.size(), 1u);
        h.press(hui::Action::west);
        EXPECT_FALSE(h.grid().tile_favourite(0));
        h.press(hui::Action::west);
        EXPECT_TRUE(h.grid().tile_favourite(0));
        CatalogueState state = h.grid().catalogue_state();
        state.sort = SortMode::za;
        state.installed = InstalledFilter::not_installed;
        h.grid().set_catalogue_state(state);
        h.settle();
        EXPECT_EQ(h.grid().shown_ids().front(), "pkg21"); // Z to A (Wren), not installed only
    }
    {
        // A new start reads the same state back.
        GridHarness h(path);
        EXPECT_EQ(h.grid().catalogue_state().sort, SortMode::za);
        EXPECT_EQ(h.grid().catalogue_state().installed, InstalledFilter::not_installed);
        EXPECT_EQ(h.grid().catalogue_state().favourites.size(), 1u);
        CatalogueState state = h.grid().catalogue_state();
        state.reset_filters();
        state.sort = SortMode::az;
        state.favourites_only = true;
        h.grid().set_catalogue_state(state);
        h.settle();
        ASSERT_EQ(h.grid().shown_count(), 1);
        EXPECT_EQ(h.grid().shown_ids().front(), first);
    }
    std::remove(path.c_str());
}

TEST(StoreGrid, TheRoundButtonIsReachableByDpadAndOpensTheUtilities)
{
    GridHarness h;
    StoreScreen &grid = h.grid();
    EXPECT_FALSE(grid.on_utility_button());
    h.move(Direction::left); // first column: the focus goes to the button
    EXPECT_TRUE(grid.on_utility_button());
    h.press(hui::Action::confirm);
    EXPECT_TRUE(grid.utilities_open());
    EXPECT_TRUE(grid.modal());
    // Tabs do not switch under the panel.
    h.press(hui::Action::page_next);
    EXPECT_EQ(h.app->current_tab(), tab::store);
    h.press(hui::Action::confirm); // "Sort by" page
    h.move(Direction::down);       // Z to A
    h.press(hui::Action::confirm);
    EXPECT_FALSE(grid.utilities_open());
    EXPECT_EQ(grid.catalogue_state().sort, SortMode::za);
    EXPECT_EQ(grid.focus_index(), 0);
    // Options opens the panel from the grid, Circle closes it.
    h.move(Direction::right);
    EXPECT_FALSE(grid.on_utility_button());
    h.press(hui::Action::menu);
    EXPECT_TRUE(grid.utilities_open());
    h.press(hui::Action::back);
    EXPECT_FALSE(grid.utilities_open());
}

TEST(StoreGrid, FilterPanelCyclesRowsAndResetClearsThem)
{
    GridHarness h;
    StoreScreen &grid = h.grid();
    h.press(hui::Action::menu);
    h.move(Direction::down);       // Filter by
    h.press(hui::Action::confirm); // the filter page
    // Row 0 (group Show) is Installation: right cycles to Installed.
    h.move(Direction::right);
    EXPECT_EQ(grid.catalogue_state().installed, InstalledFilter::installed);
    h.settle();
    EXPECT_LT(grid.shown_count(), 25);
    for (int i = 0; i < 8; ++i)
        h.move(Direction::down); // Favourites, has links, region, firmware, no browser, single file, tier, Source
    h.move(Direction::right);    // the first source
    EXPECT_FALSE(grid.catalogue_state().source.empty());
    for (int i = 0; i < 5; ++i)
        h.move(Direction::down); // to Reset filters (clamped at the last row)
    h.press(hui::Action::confirm);
    h.settle();
    EXPECT_FALSE(grid.catalogue_state().filtered());
    EXPECT_EQ(grid.shown_count(), 25);
}

TEST(StoreGrid, EmptyResultShowsTheMessageAndConfirmResets)
{
    GridHarness h;
    StoreScreen &grid = h.grid();
    grid.set_query("zzzzzz");
    h.settle();
    EXPECT_EQ(grid.shown_count(), 0);
    h.press(hui::Action::confirm); // reset from the empty state
    EXPECT_EQ(grid.query(), "");
    EXPECT_EQ(grid.shown_count(), 25);
}

TEST(StoreGrid, FocusStaysOnThePackageWhenTheInstalledListChanges)
{
    GridHarness h;
    StoreScreen &grid = h.grid();
    h.move(Direction::right);
    h.move(Direction::down);
    const std::string id = grid.shown_ids()[static_cast<std::size_t>(grid.focus_index())];
    CatalogueState state = grid.catalogue_state();
    state.sort = SortMode::installed_first; // the order depends on the installed list
    grid.set_catalogue_state(state);
    h.settle();
    // The player changed the order: the focus restarts at the first tile.
    EXPECT_EQ(grid.focus_index(), 0);
    // A refresh of the game list (a download finished) keeps the focused package.
    h.move(Direction::down);
    const std::string kept = grid.shown_ids()[static_cast<std::size_t>(grid.focus_index())];
    h.store.refresh_now();
    h.store.poll_once();
    h.settle();
    EXPECT_EQ(grid.shown_ids()[static_cast<std::size_t>(grid.focus_index())], kept);
    (void)id;
}

} // namespace
} // namespace orbit
