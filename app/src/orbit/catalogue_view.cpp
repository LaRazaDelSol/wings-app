// Wings TV app - What the Store grid shows: sort, filters, favourites, installed status.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/catalogue_view.hpp"

#include "core/save_file.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cctype>

namespace orbit
{

namespace
{
std::string lower(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string normal_id(std::string_view id)
{
    std::string out;
    for (const char c : id)
    {
        if (c != ' ' && c != '\t')
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

constexpr std::size_t kMaxFavourites = 4000;
} // namespace

const char *sort_label(SortMode mode)
{
    switch (mode)
    {
    case SortMode::az:
        return tr("A to Z");
    case SortMode::za:
        return tr("Z to A");
    case SortMode::installed_first:
        return tr("Installed first");
    case SortMode::not_installed_first:
        return tr("Not installed first");
    case SortMode::favourites_first:
        return tr("Favourites first");
    case SortMode::tier_first:
        return tr("Best link first");
    case SortMode::source:
        return tr("Source");
    case SortMode::popular:
        return tr("Most popular");
    case SortMode::rated:
        return tr("Highest rated");
    case SortMode::reviewed:
        return tr("Most reviewed");
    case SortMode::my_downloads:
        return tr("Your most downloaded");
    }
    return "";
}

bool CatalogueState::is_favourite(const std::string &key) const
{
    return std::binary_search(favourites.begin(), favourites.end(), key);
}

bool CatalogueState::toggle_favourite(const std::string &key)
{
    const auto it = std::lower_bound(favourites.begin(), favourites.end(), key);
    if (it != favourites.end() && *it == key)
    {
        favourites.erase(it);
        return false;
    }
    if (favourites.size() >= kMaxFavourites)
        return false;
    favourites.insert(it, key);
    return true;
}

bool CatalogueState::filtered() const
{
    return installed != InstalledFilter::any || favourites_only || tier != TierFilter::any ||
           has_links_only || !source.empty() || !region.empty() || runs_on_my_fw || no_browser ||
           single_file_only;
}

void CatalogueState::reset_filters()
{
    installed = InstalledFilter::any;
    favourites_only = false;
    tier = TierFilter::any;
    has_links_only = false;
    source.clear();
    region.clear();
    runs_on_my_fw = false;
    no_browser = false;
    single_file_only = false;
}

std::string favourite_key(const Package &package)
{
    if (!package.title_id.empty())
        return normal_id(package.title_id);
    return "title:" + lower(package.title);
}

void InstalledIndex::assign(const std::vector<InstalledGame> &games, bool have)
{
    known = have;
    ids.clear();
    for (const InstalledGame &g : games)
    {
        if (!g.title_id.empty())
            ids.insert(normal_id(g.title_id));
    }
}

int InstalledIndex::status(const std::string &title_id) const
{
    if (!known || title_id.empty())
        return -1;
    return ids.count(normal_id(title_id)) != 0 ? 1 : 0;
}

LinkTier package_tier(const Package &package)
{
    if (package.has_pkg)
        return LinkTier::native_pkg;
    if (package.has_automated)
        return LinkTier::automated;
    if (package.has_supported)
        return LinkTier::supported;
    return package.link_count > 0 ? LinkTier::unknown : LinkTier::unsupported;
}

std::uint8_t tier_of(const std::vector<Package> &packages, const std::vector<std::uint8_t> &tiers,
                     std::size_t index)
{
    if (tiers.size() == packages.size() && index < tiers.size())
        return tiers[index];
    return pack_tier(package_tier(packages[index]), false);
}

bool package_matches(const Package &package, std::string_view query)
{
    std::size_t at = 0;
    while (at < query.size())
    {
        while (at < query.size() && query[at] == ' ')
            ++at;
        std::size_t end = at;
        while (end < query.size() && query[end] != ' ')
            ++end;
        const std::string_view word = query.substr(at, end - at);
        at = end;
        if (word.empty())
            continue;
        if (!contains_folded(package.title, word) && !contains_folded(package.title_id, word) &&
            !contains_folded(package.source_name, word) && !contains_folded(package.version, word))
            return false;
    }
    return true;
}

bool CatalogueState::add_recent_search(std::string_view term)
{
    std::size_t a = 0;
    std::size_t b = term.size();
    while (a < b && term[a] == ' ')
        ++a;
    while (b > a && term[b - 1] == ' ')
        --b;
    term = term.substr(a, b - a);
    if (term.empty() || term.find('\n') != std::string_view::npos ||
        term.find('\r') != std::string_view::npos)
        return false;
    const std::string folded = lower(term);
    if (!recent_searches.empty() && lower(recent_searches.front()) == folded &&
        recent_searches.front() == term)
        return false;
    recent_searches.erase(std::remove_if(recent_searches.begin(), recent_searches.end(),
                                         [&](const std::string &s) { return lower(s) == folded; }),
                          recent_searches.end());
    recent_searches.insert(recent_searches.begin(), std::string(term));
    if (recent_searches.size() > kMaxRecentSearches)
        recent_searches.resize(kMaxRecentSearches);
    return true;
}

std::vector<int> select_packages(const std::vector<Package> &packages,
                                 const std::vector<std::uint8_t> &tiers,
                                 const CatalogueState &state,
                                 std::string_view query, const InstalledIndex &installed)
{
    struct Entry
    {
        int index;
        std::string title; // lower case
        std::string source;
        int installed; // 1 / 0 / -1
        bool favourite;
        int tier;
    };
    std::vector<Entry> kept;
    kept.reserve(packages.size());
    for (std::size_t i = 0; i < packages.size(); ++i)
    {
        const Package &p = packages[i];
        if (!state.source.empty() && p.source_name != state.source)
            continue;
        if (!query.empty() && !package_matches(p, query))
            continue;
        if (state.has_links_only && p.link_count <= 0)
            continue;
        const int status = installed.status(p.title_id);
        // Unknown (database not read, no title id) matches neither side of the filter.
        if (state.installed == InstalledFilter::installed && status != 1)
            continue;
        if (state.installed == InstalledFilter::not_installed && status != 0)
            continue;
        const bool favourite = state.is_favourite(favourite_key(p));
        if (state.favourites_only && !favourite)
            continue;
        const std::uint8_t packed = tier_of(packages, tiers, i);
        const LinkTier tier = packed_tier(packed);
        if (state.tier == TierFilter::supported && !tier_is_green(tier))
            continue;
        // "Debrid": at least one link a configured debrid service takes.
        if (state.tier == TierFilter::debrid && !packed_via_debrid(packed))
            continue;
        kept.push_back({static_cast<int>(i), lower(p.title), lower(p.source_name), status, favourite,
                        static_cast<int>(tier)});
    }
    const SortMode mode = state.sort;
    std::stable_sort(kept.begin(), kept.end(),
                     [mode](const Entry &a, const Entry &b)
                     {
                         const auto by_title = [&] { return a.title < b.title; };
                         switch (mode)
                         {
                         case SortMode::az:
                             return a.title < b.title;
                         case SortMode::za:
                             return a.title > b.title;
                         case SortMode::installed_first:
                             // installed (1), then not installed (0), then unknown (-1)
                             return a.installed != b.installed ? a.installed > b.installed : by_title();
                         case SortMode::not_installed_first:
                         {
                             const auto rank = [](int s) { return s == 0 ? 0 : (s == 1 ? 1 : 2); };
                             return rank(a.installed) != rank(b.installed)
                                        ? rank(a.installed) < rank(b.installed)
                                        : by_title();
                         }
                         case SortMode::favourites_first:
                             return a.favourite != b.favourite ? a.favourite : by_title();
                         case SortMode::tier_first:
                             return a.tier != b.tier ? a.tier < b.tier : by_title();
                         case SortMode::source:
                             return a.source != b.source ? a.source < b.source : by_title();
                         case SortMode::popular:
                         case SortMode::rated:
                         case SortMode::reviewed:
                         case SortMode::my_downloads:
                             return by_title(); // games only (select_games)
                         }
                         return false;
                     });
    std::vector<int> out;
    out.reserve(kept.size());
    for (const Entry &e : kept)
        out.push_back(e.index);
    return out;
}

std::string filter_summary(const CatalogueState &state, std::string_view query)
{
    std::vector<std::string> parts;
    if (!state.source.empty())
        parts.push_back(state.source);
    if (!state.region.empty())
        parts.push_back(state.region);
    if (state.runs_on_my_fw)
        parts.push_back(tr("Runs on my firmware"));
    if (state.no_browser)
        parts.push_back(tr("No browser needed"));
    if (state.single_file_only)
        parts.push_back(tr("Single file only"));
    if (state.installed == InstalledFilter::installed)
        parts.push_back(tr("Installed"));
    else if (state.installed == InstalledFilter::not_installed)
        parts.push_back(tr("Not installed"));
    if (state.favourites_only)
        parts.push_back(tr("Favourites"));
    if (state.tier == TierFilter::supported)
        parts.push_back(tr("Supported links"));
    else if (state.tier == TierFilter::debrid)
        parts.push_back(tr("Debrid links"));
    if (state.has_links_only)
        parts.push_back(tr("Has links"));
    if (!query.empty())
        parts.push_back(tr("Search \"{query}\"", {{"query", std::string(query)}}));
    if (parts.empty())
        return tr("None");
    std::string out;
    for (const std::string &p : parts)
    {
        if (!out.empty())
            out += ", ";
        out += p;
    }
    return out;
}

std::string encode_catalogue_state(const CatalogueState &state)
{
    std::string out;
    out += "sort=" + std::to_string(static_cast<int>(state.sort)) + "\n";
    out += "installed=" + std::to_string(static_cast<int>(state.installed)) + "\n";
    out += std::string("fav_only=") + (state.favourites_only ? "1" : "0") + "\n";
    out += "tier=" + std::to_string(static_cast<int>(state.tier)) + "\n";
    out += std::string("has_links=") + (state.has_links_only ? "1" : "0") + "\n";
    std::string source;
    for (const char c : state.source)
    {
        if (c != '\n' && c != '\r')
            source.push_back(c);
    }
    out += "source=" + source + "\n";
    // Added with the game grid; older builds skip keys they do not know.
    std::string region;
    for (const char c : state.region)
    {
        if (c != '\n' && c != '\r')
            region.push_back(c);
    }
    out += "region=" + region + "\n";
    out += std::string("runs_fw=") + (state.runs_on_my_fw ? "1" : "0") + "\n";
    out += std::string("no_browser=") + (state.no_browser ? "1" : "0") + "\n";
    out += std::string("single_file=") + (state.single_file_only ? "1" : "0") + "\n";
    for (const std::string &key : state.favourites)
    {
        if (key.find('\n') == std::string::npos && !key.empty())
            out += "fav=" + key + "\n";
    }
    for (const std::string &term : state.recent_searches)
    {
        if (!term.empty() && term.find('\n') == std::string::npos)
            out += "recent=" + term + "\n";
    }
    return out;
}

bool decode_catalogue_state(std::string_view payload, CatalogueState *out)
{
    CatalogueState state;
    std::size_t at = 0;
    const auto number = [](std::string_view v, int low, int high, int fallback)
    {
        int n = 0;
        if (v.empty() || v.size() > 3)
            return fallback;
        for (const char c : v)
        {
            if (c < '0' || c > '9')
                return fallback;
            n = n * 10 + (c - '0');
        }
        return n >= low && n <= high ? n : fallback;
    };
    while (at < payload.size())
    {
        std::size_t end = payload.find('\n', at);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string_view line = payload.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        if (key == "sort")
            state.sort = static_cast<SortMode>(number(value, 0, kSortModeCount - 1, 0));
        else if (key == "installed")
            state.installed = static_cast<InstalledFilter>(number(value, 0, 2, 0));
        else if (key == "fav_only")
            state.favourites_only = value == "1";
        else if (key == "tier")
            state.tier = static_cast<TierFilter>(number(value, 0, 2, 0));
        else if (key == "has_links")
            state.has_links_only = value == "1";
        else if (key == "source")
            state.source = std::string(value);
        else if (key == "region")
            state.region = std::string(value);
        else if (key == "runs_fw")
            state.runs_on_my_fw = value == "1";
        else if (key == "no_browser")
            state.no_browser = value == "1";
        else if (key == "single_file")
            state.single_file_only = value == "1";
        else if (key == "fav" && !value.empty() && state.favourites.size() < kMaxFavourites)
            state.favourites.emplace_back(value);
        else if (key == "recent" && !value.empty() && state.recent_searches.size() < kMaxRecentSearches)
            state.recent_searches.emplace_back(value); // file order is newest first
    }
    std::sort(state.favourites.begin(), state.favourites.end());
    state.favourites.erase(std::unique(state.favourites.begin(), state.favourites.end()),
                           state.favourites.end());
    *out = std::move(state);
    return true;
}

bool load_catalogue_state(const std::vector<std::string> &paths, CatalogueState *out)
{
    for (const std::string &path : paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 1u << 20))
            continue;
        const hui::save::Decoded decoded = hui::save::decode(hui::save::Kind::prefs, data);
        if (decoded.ok && decoded.version == 1 && decode_catalogue_state(decoded.payload, out))
            return true;
    }
    return false;
}

int save_catalogue_state(const std::vector<std::string> &paths, const CatalogueState &state)
{
    const std::string data =
        hui::save::encode(hui::save::Kind::prefs, 1, encode_catalogue_state(state));
    int written = 0;
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            ++written;
    }
    return written;
}

} // namespace orbit
