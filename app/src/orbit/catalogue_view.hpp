// Wings TV app - What the Store grid shows: sort, filters, favourites, installed status.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Platform-neutral and testable on the host. The grid recomputes its list of
// packages from these rules only when something changed (catalogue, sources,
// installed games, favourites, sort or filter), never per frame.

#pragma once

#include "orbit/model.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace orbit
{

// New orders go at the end (the number is persisted); kSortModeCount follows.
// select_games() (game_view.cpp) has one case per order.
enum class SortMode : std::uint8_t
{
    az,
    za,
    installed_first,
    not_installed_first,
    favourites_first,
    tier_first,
    source,
    // Popularity orders (popularity.hpp): offered only when that data is on and loaded.
    popular,
    rated,
    reviewed,
    my_downloads, // this app's own queued downloads per game
};
constexpr int kSortModeCount = 11;
constexpr int kBasicSortCount = 7; // the orders that need no extra data
const char *sort_label(SortMode mode); // English keys: "A to Z", ...

enum class InstalledFilter : std::uint8_t
{
    any,
    installed,
    not_installed,
};

enum class TierFilter : std::uint8_t
{
    any,
    supported, // the best link is green (a .pkg, an automated or a supported provider)
    debrid,    // a ready debrid service takes at least one link
};

// The player's view of the catalogue; persisted (see load/save below).
struct CatalogueState
{
    SortMode sort = SortMode::az;
    InstalledFilter installed = InstalledFilter::any;
    bool favourites_only = false;
    TierFilter tier = TierFilter::any;
    bool has_links_only = false;
    std::string source; // empty: all sources
    // Game filters (game_view.hpp): a region a game's links or descriptions
    // state ("" any), a link that runs on the console's firmware (links that
    // do not say stay), a way to download without the PS5 browser.
    std::string region;
    bool runs_on_my_fw = false;
    bool no_browser = false;
    bool single_file_only = false; // a release that is one file (no parts)
    std::vector<std::string> favourites; // favourite_key()s, sorted
    std::vector<std::string> recent_searches; // newest first, at most kMaxRecentSearches

    bool is_favourite(const std::string &key) const;
    // Returns true when the key is a favourite afterwards.
    bool toggle_favourite(const std::string &key);
    // True when any filter (not the sort, not the search) narrows the list.
    bool filtered() const;
    // Back to no filters (favourites and the sort stay).
    void reset_filters();
    // Remembers a search the player ran: trimmed, a repeat (any case) moves to
    // the front, the oldest beyond kMaxRecentSearches drops. Empty terms and
    // terms with line breaks are ignored. True when the list changed.
    bool add_recent_search(std::string_view term);
};
constexpr std::size_t kMaxRecentSearches = 8;

// True when every word of `query` is found (any case) in the title (which
// carries tags such as [exFAT] and the region), title id, source or version.
// An empty query matches everything.
bool package_matches(const Package &package, std::string_view query);

// Stable across catalogue reloads (Pegasus' own package ids are not): the title
// id when the package has one, else the title.
std::string favourite_key(const Package &package);

// The installed games' title ids, normalised. known is false until the game
// database was read once: then nothing is claimed either way.
struct InstalledIndex
{
    bool known = false;
    std::unordered_set<std::string> ids;
    void assign(const std::vector<InstalledGame> &games, bool have);
    // 1 installed, 0 not installed, -1 unknown (database not read, or no title id).
    int status(const std::string &title_id) const;
};

// The best way to download a package from its own flags only (no debrid): the
// fallback while the backend's classification (Snapshot::tiers) is missing.
LinkTier package_tier(const Package &package);

// What the grid knows about one package: the packed tier (model.hpp pack_tier)
// from `tiers` when it belongs to this catalogue (same length), else the flags.
std::uint8_t tier_of(const std::vector<Package> &packages, const std::vector<std::uint8_t> &tiers,
                     std::size_t index);

// Indexes into `packages` that pass the filters and the search, in sort order.
// `tiers` is Snapshot::tiers (may be empty). Ties are broken by title, so the
// order is deterministic.
std::vector<int> select_packages(const std::vector<Package> &packages,
                                 const std::vector<std::uint8_t> &tiers,
                                 const CatalogueState &state, std::string_view query,
                                 const InstalledIndex &installed);

// "Installed", "Favourites, Has links", "None" (English keys, joined by ", ").
std::string filter_summary(const CatalogueState &state, std::string_view query);

// Persistence: key=value lines in the checked save container.
std::string encode_catalogue_state(const CatalogueState &state);
bool decode_catalogue_state(std::string_view payload, CatalogueState *out);
bool load_catalogue_state(const std::vector<std::string> &paths, CatalogueState *out);
int save_catalogue_state(const std::vector<std::string> &paths, const CatalogueState &state);

} // namespace orbit
