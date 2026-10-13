// Wings TV app - What the Store shows: one tile per game, and a game's page of links.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The view model between the pure link logic (game_group, game_page, ranking,
// link_facets) and the Store screen, so the screen only draws and routes
// input. Everything here is recomputed when something changed (catalogue,
// preferences, a filter), never per frame, and is tested on the host with the
// fake backend and the recorded catalogue.

#pragma once

#include "orbit/catalogue_view.hpp"
#include "orbit/game_group.hpp"
#include "orbit/game_page.hpp"
#include "orbit/game_filters.hpp"
#include "orbit/link_facets.hpp"
#include "orbit/prefs.hpp"
#include "orbit/ranking.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// Settings > Wings > Downloads as the ranking code takes it.
LinkPrefs link_prefs_of(const WingsLocalPrefs &prefs);

// ---- the grid: one tile per game ----

// What a package's links say, for the grid's filters. Read once per catalogue
// (in chunks, off the frame thread); `known` stays false until then.
struct PackageFacts
{
    bool known = false;
    std::vector<std::string> regions; // stated by its links, first-seen order
    int lowest_fw = -1;      // fw_need() of the lowest firmware a link runs on; -1 none states one
    bool fw_unknown = false; // a link states no firmware
    bool single_file = false; // a release of the game has no parts
};
PackageFacts package_facts(const PackageDetail &detail);

// "USA" "EUR" "JPN" "ASIA" "HK" "KOR" first, then others A to Z.
void sort_regions(std::vector<std::string> *regions);

// 1 a package of the game is installed, 0 none is (and the list is known), -1 unknown.
int game_installed(const GameGroup &game, const std::vector<Package> &packages,
                   const InstalledIndex &installed);
// Favourites keep their key per package (favourite_key(), catalogue.v1 stays
// readable by older builds): a game is a favourite when any of its packages is.
bool game_favourite(const CatalogueState &state, const GameGroup &game,
                    const std::vector<Package> &packages);
// Square on a game: on adds the first package's key, off removes every
// package's key. Returns true when the game is a favourite afterwards.
bool toggle_game_favourite(CatalogueState *state, const GameGroup &game,
                           const std::vector<Package> &packages);
// The package whose cover the tile shows: the first with a poster, else the first.
int art_package(const GameGroup &game, const std::vector<Package> &packages);
// The game's regions: package descriptions plus what its links state (once read).
std::vector<std::string> game_regions(const GameGroup &game, const std::vector<PackageFacts> &facts);
// "Runs on my firmware" for a game: 1 a link runs (or does not say), 0 every
// link needs a newer firmware, -1 cannot tell (no firmware set, links not read).
int game_runs_on(const GameGroup &game, const std::vector<PackageFacts> &facts,
                 const std::string &console_fw);
// The search matches a game when it matches any of its packages.
bool game_matches(const GameGroup &game, const std::vector<Package> &packages, std::string_view query);

// 1 a package of the game has a release in one file, 0 none has (links read),
// -1 cannot tell (links not read).
int game_single_file(const GameGroup &game, const std::vector<PackageFacts> &facts);

struct GridAggregate; // grid_filters.hpp: per-game answers, built once per catalogue / preference change

struct GameTile
{
    int game = 0;
    int art_package = 0; // index into the packages: the cover, and the id the tile stands for
    std::int8_t installed = -1;
    bool favourite = false;
    std::uint8_t tier = 0; // game_tier(): pack_tier()
    std::vector<std::string> regions;
    int popularity = -1; // popularity score when known and shown (popularity.hpp), else -1
};

struct PopularityView;
struct GridQuery
{
    const std::vector<Package> &packages;
    const std::vector<std::uint8_t> &tiers; // Snapshot::tiers (may be empty)
    const GameIndex &index;
    const std::vector<PackageFacts> &facts; // per package (may be empty: not read yet)
    const CatalogueState &state;
    std::string_view query;
    const InstalledIndex &installed;
    const LinkPrefs &prefs;
    const GridAggregate *aggregate = nullptr; // when it matches the index, the filters read it
    const PopularityView *popularity = nullptr; // null: no popularity data (the sorts fall back to A to Z)
};
// The tiles that pass the filters and the search, in the sort order (ties by
// title, then catalogue order: deterministic).
std::vector<GameTile> select_games(const GridQuery &query);
// The regions the grid's Region filter offers (every game's, known order first).
std::vector<std::string> catalogue_regions(const GameIndex &index, const std::vector<PackageFacts> &facts);

// ---- what this app already queued ----

// (packageId, linkIndex) pairs Wings asked Pegasus DL to download in this run:
// "Download all parts" skips them. Pegasus' jobs do not carry these ids.
struct StartedLinks
{
    std::set<std::string> keys;
    bool has(const LinkRef &ref) const;
    void add(const LinkRef &ref);
};

// ---- the game page ----

enum class PageRowKind : std::uint8_t
{
    best,    // the Best card (focusable)
    filters, // "Filters: ..." (focusable, opens the panel)
    hidden,  // "3 hidden: they need 12.xx" (text)
    section, // Game / Updates / DLC / Other
    release, // "USA · 12.xx · Base · v01.055 · Source"
    mirror,  // one card (focusable)
    empty,   // nothing matches the filters (focusable: reset) or no links at all
};

struct PageRow
{
    PageRowKind kind = PageRowKind::mirror;
    int section = -1;
    int release = -1;
    int mirror = -1;
    float y = 0.0f; // top, from the top of the list
    float h = 0.0f;
    bool focusable() const
    {
        return kind == PageRowKind::best || kind == PageRowKind::filters || kind == PageRowKind::mirror ||
               kind == PageRowKind::empty;
    }
};

// Row heights (virtual pixels) shared by the layout and the drawing.
namespace page_layout
{
constexpr float kBest = 150.0f;
constexpr float kFilters = 66.0f;
constexpr float kHidden = 40.0f;
constexpr float kSection = 72.0f;
constexpr float kRelease = 50.0f;
constexpr float kReleaseSub = 76.0f;
constexpr float kMirror = 96.0f;
constexpr float kMirrorWarning = 124.0f;
constexpr float kEmpty = 128.0f;
constexpr float kGap = 12.0f;
} // namespace page_layout

struct GameView
{
    std::string title;
    std::string art_key;                  // "pkg:<id>"
    std::vector<std::string> package_ids; // the game's packages, catalogue order
    std::vector<std::string> regions;     // descriptions and links, known order
    std::vector<std::string> sources;
    std::vector<std::string> title_ids;
    double listed_size = 0.0;        // the largest listed package size (0 unknown)
    std::string listed_size_source;  // whose listing it is
    bool loaded = false;
    std::string error;

    std::vector<PackageDetail> details; // what the backend said, the game's package order
    GamePage page;
    Facets facets;
    MirrorFilter filter;
    FilterResult shown;
    MirrorSort sort = MirrorSort::best; // order of the cards within a release (game_filters.hpp)
    std::string seed_note;              // "Your region (USA) would hide every download, so it is not applied."
    std::vector<PageRow> rows;
    float height = 0.0f; // of all rows

    // The backend's link for a page link (route, provider, services), or null.
    const PackageLink *link(const LinkRef &ref) const;
    // The first link of a mirror as a queue reference.
    LinkRef first_ref(int mirror) const;
    int section_of_mirror(int mirror) const;
};

// The page for `details`: built, ranked, facets counted, the default filter
// set ("Runs on my firmware" is on once a firmware is set and it would hide
// something) and laid out.
void load_game_view(GameView *view, std::vector<PackageDetail> details,
                    const std::vector<std::string> &source_order, const LinkPrefs &prefs);
// The preferences changed: ranked again, the filter kept (where it still applies).
void rerank_game_view(GameView *view, const std::vector<std::string> &source_order, const LinkPrefs &prefs);
// Another filter: rows again.
void set_game_filter(GameView *view, const MirrorFilter &filter, const LinkPrefs &prefs);

// "Listed size 61.2 GB (Community Mirror)", "" when no package lists one.
std::string listed_size_text(const GameView &view);

// One mirror card's words.
struct MirrorCard
{
    std::string host;     // the real host (the name's, when the URL is a wrapper)
    bool wrapped = false; // the URL is a decrypt page / link vault: "via <url host>"
    std::string via;
    std::string region;
    std::string fw;      // "12.xx", "7.xx BackPort"
    std::string fw_note; // "runs on 4.xx+" (backport target), "fw ?" (preference set, not stated), "needs 12.xx"
    FwFit fit = FwFit::runs;
    bool fit_known = false; // a firmware is set: colour the firmware
    std::string format;
    std::string version;
    std::string source;
    std::string kind; // "Game + DLC", "Update", ...
    std::string parts;   // "1 file", "22 parts (1-22)"
    std::string warning; // "part 4 missing", ...
    std::string name;    // the first link's name as the catalogue has it
    LinkTier tier = LinkTier::unknown;
    bool needs_browser = false;
    bool multipart = false;
    int part_count = 0;
    int started = 0; // parts (or the link) this app already queued
    bool best = false;
};
MirrorCard mirror_card(const GameView &view, int mirror, const LinkPrefs &prefs,
                       const StartedLinks &started);

// The Best card: "Debrid · USA · BP 4.xx · 1 file" and the reason.
std::string best_summary(const GameView &view);

// The part number a link has inside its mirror (by position when unnumbered); 0 not in it.
int mirror_part_number(const GamePage &page, int mirror, const LinkRef &ref);

// "Download all parts": what would be queued and how to say it.
struct PartsPlan
{
    std::vector<LinkRef> refs; // to queue, catalogue order, already-started ones left out
    // Parallel to refs: each part's number, and the same part on the release's
    // other hosts (best tier first, then host A to Z) to try when it keeps
    // failing. Only between sets numbered 1..N exactly alike; never guessed.
    std::vector<int> numbers;
    std::vector<std::vector<LinkRef>> alternates;
    std::vector<std::vector<std::string>> alternate_hosts;
    int total = 0;             // links in the set
    int skipped = 0;           // already started by this app
    bool one_at_a_time = false; // the parts open the PS5 browser: one per check, never all at once
    std::string host;
    std::string action; // "Download all 22 parts", "Download all 14 links", "Download the next part (3 of 8)"
    std::string title;  // the confirmation's title: "Download 22 parts from VikingFile?"
    std::string message;
};
PartsPlan plan_parts(const GameView &view, int mirror, const StartedLinks &started);

// ---- the page's filter panel ----

enum class LinkFilterRow : std::uint8_t
{
    runs_on_my_fw,
    no_browser,
    region,
    single_file,
    format,
    kind,
    host,
    source,
    sort, // the cards' order (GameView::sort, not part of MirrorFilter)
    reset,
};
// The rows worth showing: a facet with two or more values (or one already
// set, so it can be cleared), Reset last.
std::vector<LinkFilterRow> link_filter_rows(const Facets &facets, const MirrorFilter &filter,
                                            const LinkPrefs &prefs);
const char *link_filter_label(LinkFilterRow row);
std::string link_filter_value(LinkFilterRow row, const MirrorFilter &filter, const Facets &facets,
                              const LinkPrefs &prefs);
// Left / right / Cross on a row (Reset clears everything).
void cycle_link_filter(MirrorFilter *filter, LinkFilterRow row, int delta, const Facets &facets);
// "Runs on 10.xx, USA" or "None".
std::string link_filter_summary(const MirrorFilter &filter, const LinkPrefs &prefs);

} // namespace orbit
