// Wings TV app - The game page's Filters panel: facets, seeding from the preferences, sort, empty state, focus.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/game_group.hpp"
#include "orbit/game_view.hpp"
#include "orbit/pegasus_backend.hpp"
#include "support.hpp"
#include "wings/replay_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

namespace orbit
{
namespace
{

std::vector<PackageDetail> starfall(host::FakeBackend &backend)
{
    std::vector<PackageDetail> out;
    for (const char *id : {"pkg25", "pkg26", "pkg27"})
        out.push_back(backend.package(id).value);
    return out;
}

bool has_row(const std::vector<LinkFilterRow> &rows, LinkFilterRow row)
{
    return std::find(rows.begin(), rows.end(), row) != rows.end();
}

// One Filters row, one card per shown mirror, the empty row only when nothing is shown.
void expect_sane(const GameView &v)
{
    if (v.page.mirrors.empty())
        return;
    int filters = 0, empty = 0, mirrors = 0;
    for (const PageRow &r : v.rows)
    {
        filters += r.kind == PageRowKind::filters;
        empty += r.kind == PageRowKind::empty;
        mirrors += r.kind == PageRowKind::mirror;
    }
    EXPECT_EQ(filters, 1);
    EXPECT_EQ(mirrors, static_cast<int>(v.shown.mirrors.size()));
    EXPECT_EQ(empty, v.shown.mirrors.empty() ? 1 : 0);
}

std::string low(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<int> release_runs(const GameView &v, const std::vector<int> &mirrors)
{
    std::vector<int> out;
    for (const int m : mirrors)
    {
        const int r = v.page.mirrors[static_cast<std::size_t>(m)].release;
        if (out.empty() || out.back() != r)
            out.push_back(r);
    }
    return out;
}

TEST(GameFilters, RowsOnlyForFacetsWithTwoOrMoreValues)
{
    host::FakeBackend backend;
    LinkPrefs prefs;
    prefs.firmware = "10.xx";
    GameView v;
    load_game_view(&v, starfall(backend), {}, prefs);
    const Facets &f = v.facets;
    const auto rows = link_filter_rows(f, MirrorFilter{}, prefs);
    EXPECT_EQ(has_row(rows, LinkFilterRow::region), f.show_regions());
    EXPECT_EQ(has_row(rows, LinkFilterRow::format), f.show_formats());
    EXPECT_EQ(has_row(rows, LinkFilterRow::host), f.hosts.size() >= 2);
    EXPECT_EQ(has_row(rows, LinkFilterRow::source), f.sources.size() >= 2);
    EXPECT_EQ(has_row(rows, LinkFilterRow::kind), f.sections.size() >= 2);
    EXPECT_EQ(has_row(rows, LinkFilterRow::single_file), f.show_single_file());
    EXPECT_EQ(has_row(rows, LinkFilterRow::no_browser), f.show_no_browser());
    EXPECT_EQ(rows.back(), LinkFilterRow::reset);
    EXPECT_TRUE(has_row(rows, LinkFilterRow::sort));
    EXPECT_GE(f.hosts.size(), 2u);
    // A facet with one value is left out.
    Facets one = f;
    one.hosts.resize(1);
    one.sources.resize(1);
    const auto r1 = link_filter_rows(one, MirrorFilter{}, prefs);
    EXPECT_FALSE(has_row(r1, LinkFilterRow::host));
    EXPECT_FALSE(has_row(r1, LinkFilterRow::source));
}

TEST(GameFilters, ShownEqualsFilterMirrorsAndEveryStepIsSane)
{
    host::FakeBackend backend;
    LinkPrefs prefs;
    GameView v;
    load_game_view(&v, starfall(backend), {}, prefs);
    for (const LinkFilterRow row : link_filter_rows(v.facets, MirrorFilter{}, prefs))
    {
        if (row == LinkFilterRow::reset || row == LinkFilterRow::sort)
            continue;
        MirrorFilter f;
        for (int step = 0; step < 10; ++step)
        {
            cycle_link_filter(&f, row, 1, v.facets);
            set_game_filter(&v, f, prefs);
            std::vector<int> want = filter_mirrors(v.page, f, prefs).mirrors;
            std::vector<int> got = v.shown.mirrors;
            std::sort(want.begin(), want.end());
            std::sort(got.begin(), got.end());
            EXPECT_EQ(got, want);
            expect_sane(v);
            if (!f.active())
                break;
        }
    }
}

TEST(GameFilters, EmptyStateThenClear)
{
    host::FakeBackend backend;
    LinkPrefs prefs;
    GameView v;
    load_game_view(&v, starfall(backend), {}, prefs);
    MirrorFilter f;
    f.region = "NOPE";
    set_game_filter(&v, f, prefs);
    EXPECT_TRUE(v.shown.mirrors.empty());
    expect_sane(v);
    EXPECT_NE(link_filter_summary(v.filter, prefs).find("NOPE"), std::string::npos);
    cycle_link_filter(&f, LinkFilterRow::reset, 1, v.facets);
    EXPECT_FALSE(f.active());
    set_game_filter(&v, f, prefs);
    EXPECT_FALSE(v.shown.mirrors.empty());
    expect_sane(v);
}

TEST(GameFilters, SortKeepsReleasesAndTheSameMirrors)
{
    host::FakeBackend backend;
    LinkPrefs prefs;
    GameView v;
    load_game_view(&v, starfall(backend), {}, prefs);
    const std::vector<int> best = v.shown.mirrors;
    for (int s = 1; s < kMirrorSortCount; ++s)
    {
        v.sort = static_cast<MirrorSort>(s);
        set_game_filter(&v, v.filter, prefs);
        std::vector<int> a = best, b = v.shown.mirrors;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        EXPECT_EQ(a, b) << mirror_sort_label(v.sort);
        EXPECT_EQ(release_runs(v, best), release_runs(v, v.shown.mirrors));
        expect_sane(v);
    }
    v.sort = MirrorSort::host;
    set_game_filter(&v, v.filter, prefs);
    for (std::size_t i = 1; i < v.shown.mirrors.size(); ++i)
    {
        const Mirror &a = v.page.mirrors[static_cast<std::size_t>(v.shown.mirrors[i - 1])];
        const Mirror &b = v.page.mirrors[static_cast<std::size_t>(v.shown.mirrors[i])];
        if (a.release == b.release)
            EXPECT_LE(low(a.host), low(b.host));
    }
    v.sort = MirrorSort::fewest_parts;
    set_game_filter(&v, v.filter, prefs);
    for (std::size_t i = 1; i < v.shown.mirrors.size(); ++i)
    {
        const Mirror &a = v.page.mirrors[static_cast<std::size_t>(v.shown.mirrors[i - 1])];
        const Mirror &b = v.page.mirrors[static_cast<std::size_t>(v.shown.mirrors[i])];
        if (a.release == b.release)
            EXPECT_LE(a.multipart() ? a.links.size() : 1u, b.multipart() ? b.links.size() : 1u);
    }
    EXPECT_EQ(cycle_mirror_sort(MirrorSort::best, -1), MirrorSort::host);
    EXPECT_EQ(cycle_mirror_sort(MirrorSort::host, 1), MirrorSort::best);
}

TEST(GameFilters, SeedFromPrefsDropsWhatWouldHideEverything)
{
    host::FakeBackend backend;
    LinkPrefs none;
    GameView v;
    load_game_view(&v, starfall(backend), {}, none);
    ASSERT_TRUE(v.facets.show_regions());
    LinkPrefs prefs;
    prefs.region = v.facets.regions.front().value;
    FilterSeed seed = seed_filter(v.page, v.facets, prefs);
    EXPECT_EQ(seed.filter.region, prefs.region);
    EXPECT_FALSE(seed.dropped());
    EXPECT_FALSE(filter_mirrors(v.page, seed.filter, prefs).mirrors.empty());
    // A region the page does not have is not applied.
    prefs.region = "KOR";
    if (!std::any_of(v.facets.regions.begin(), v.facets.regions.end(),
                     [](const FacetValue &f) { return f.value == "KOR"; }))
    {
        seed = seed_filter(v.page, v.facets, prefs);
        EXPECT_TRUE(seed.filter.region.empty());
    }
    // Synthetic: a facet table claiming a region/format that hides everything is dropped with a note.
    Facets fake = v.facets;
    fake.regions = {{"XXX", 1}, {"YYY", 1}};
    fake.formats = {{"PKG", 1}, {"Dump", 1}};
    LinkPrefs bad;
    bad.region = "XXX";
    bad.format = LinkFormat::pkg;
    seed = seed_filter(v.page, fake, bad);
    EXPECT_TRUE(seed.dropped_region || seed.dropped_format);
    EXPECT_FALSE(seed.note.empty());
    EXPECT_FALSE(filter_mirrors(v.page, seed.filter, bad).mirrors.empty());
    // An old firmware: whatever is seeded still shows something.
    LinkPrefs old;
    old.firmware = "1.xx";
    GameView w;
    load_game_view(&w, starfall(backend), {}, old);
    EXPECT_FALSE(w.shown.mirrors.empty());
    if (w.facets.fw_runs == 0 && w.facets.fw_unknown == 0 && w.facets.fw_too_new > 0)
    {
        EXPECT_FALSE(w.filter.runs_on_my_fw);
        EXPECT_FALSE(w.seed_note.empty());
    }
    expect_sane(w);
}

std::string samples_dir()
{
    if (const char *env = std::getenv("WINGS_SAMPLES"))
        return env;
    return std::string(HUI_SOURCE_DIR) + "/../../recon/samples";
}

TEST(GameFilters, RecordedLibraryNeverSeedsAnEmptyPage)
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
    const GameIndex index = group_games(all.value);
    LinkPrefs prefs;
    prefs.firmware = "10.xx";
    prefs.region = "USA";
    prefs.format = LinkFormat::exfat;
    long seeded = 0, dropped = 0;
    for (const GameGroup &g : index.games)
    {
        std::vector<PackageDetail> d;
        for (const int p : g.packages)
            d.push_back(backend.package(all.value[static_cast<std::size_t>(p)].id).value);
        GameView v;
        load_game_view(&v, std::move(d), {}, prefs);
        if (v.page.mirrors.empty())
            continue;
        EXPECT_FALSE(v.shown.mirrors.empty()) << g.title; // never silently hides everything
        EXPECT_EQ(v.shown.mirrors, filter_mirrors(v.page, v.filter, prefs).mirrors);
        expect_sane(v);
        seeded += v.filter.active();
        dropped += !v.seed_note.empty();
        for (int s = 1; s < kMirrorSortCount; ++s)
        {
            v.sort = static_cast<MirrorSort>(s);
            set_game_filter(&v, v.filter, prefs);
            expect_sane(v);
        }
    }
    std::printf("[filters] %ld pages seeded, %ld dropped a preference\n", seeded, dropped);
}

} // namespace
} // namespace orbit
