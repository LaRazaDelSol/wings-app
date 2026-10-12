// Wings TV app - The Store tab: catalogue grid, search, filters, game page, link flows.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout and focus patterns follow Orbit Store's browse and game page screens
// (GPL-3.0-or-later); what happens when a link is activated is Pegasus' own
// rule set, decided by wings::logic::decideLinkAction behind the Backend.
// What the grid and the page show is worked out in game_view.* (one tile per
// game, the page's sections, releases and mirror cards); this file draws it
// and routes the controller.

#include "orbit/screens.hpp"
#include "orbit/filter_menu.hpp"

#include "orbit/model.hpp"
#include "orbit/popularity.hpp"
#include "orbit/steam_reviews.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace orbit
{

namespace
{

// The grid, as in the PlayStation catalogue: five square covers per row with
// small gutters, a round button to their left, the header above.
constexpr float kTile = 306.0f;
constexpr float kGap = 24.5f;
constexpr float kPitch = kTile + kGap;
constexpr float kGridLeft = 188.0f;
constexpr float kGridTop = 240.0f;
constexpr float kHeaderBaseline = 208.0f;
constexpr float kGridBottom = 1000.0f; // the third row peeks in, as on the console
constexpr float kButtonX = 134.0f;
constexpr float kButtonY = kGridTop + 46.0f;
constexpr float kButtonRadius = 34.0f;
constexpr float kPanelRow = 64.0f;
constexpr float kPanelPitch = 72.0f;

// The game page: the cover and the game's facts on the left, the title and
// the scrolling list of links on the right.
constexpr float kPageTop = 150.0f;
constexpr float kPageCover = 300.0f;
constexpr float kPageX = ui::kGutter + kPageCover + 56.0f;
constexpr float kPageRight = 1920.0f - ui::kGutter;
constexpr float kPageListTop = 290.0f;
constexpr float kPageListBottom = 994.0f;
constexpr float kCardIndent = 24.0f;
constexpr float kPageFadeClear = 44.0f; // room under the focused card for the bottom fade

// How many packages one background read of link facts covers.
constexpr std::size_t kFactsChunk = 128;

// The grid's filter page, top to bottom.
enum class GridFilter : std::uint8_t
{
    source,
    region,
    firmware,
    no_browser,
    single_file,
    installed,
    favourites,
    tier,
    has_links,
    reset,
};
// Grouped (filter_menu.hpp), the most used first: Show, Region & firmware, Format & host, Source.
constexpr GridFilter kGridFilters[] = {GridFilter::installed,   GridFilter::favourites,  GridFilter::has_links,
                                       GridFilter::region,      GridFilter::firmware,
                                       GridFilter::no_browser,  GridFilter::single_file, GridFilter::tier,
                                       GridFilter::source,      GridFilter::reset};
constexpr int kGridFilterCount = static_cast<int>(sizeof(kGridFilters) / sizeof(kGridFilters[0]));

FilterGroup grid_filter_group(GridFilter which)
{
    switch (which)
    {
    case GridFilter::installed:
    case GridFilter::favourites:
    case GridFilter::has_links:
        return FilterGroup::show;
    case GridFilter::region:
    case GridFilter::firmware:
        return FilterGroup::region_fw;
    case GridFilter::no_browser:
    case GridFilter::single_file:
    case GridFilter::tier:
        return FilterGroup::format_host;
    case GridFilter::source:
        return FilterGroup::source;
    case GridFilter::reset:
        break;
    }
    return FilterGroup::none;
}

bool grid_row_changed(GridFilter which, const CatalogueState &s)
{
    switch (which)
    {
    case GridFilter::installed:
        return s.installed != InstalledFilter::any;
    case GridFilter::favourites:
        return s.favourites_only;
    case GridFilter::has_links:
        return s.has_links_only;
    case GridFilter::region:
        return !s.region.empty();
    case GridFilter::firmware:
        return s.runs_on_my_fw;
    case GridFilter::no_browser:
        return s.no_browser;
    case GridFilter::single_file:
        return s.single_file_only;
    case GridFilter::tier:
        return s.tier != TierFilter::any;
    case GridFilter::source:
        return !s.source.empty();
    case GridFilter::reset:
        break;
    }
    return false;
}

// The row the filter page opens on: the first one the player changed, else the first group's first.
int grid_focus_row(const CatalogueState &s)
{
    std::vector<bool> changed;
    for (const GridFilter which : kGridFilters)
        changed.push_back(grid_row_changed(which, s));
    return first_focus_index(changed);
}

const InputFrame kIdle{};

std::string service_id(const std::string &label)
{
    return label == "TorBox" ? "torbox" : "real-debrid";
}

std::string trimmed(const std::string &text)
{
    std::size_t a = 0;
    std::size_t b = text.size();
    while (a < b && text[a] == ' ')
        ++a;
    while (b > a && text[b - 1] == ' ')
        --b;
    return text.substr(a, b - a);
}

int first_focusable(const GameView &view)
{
    for (std::size_t i = 0; i < view.rows.size(); ++i)
        if (view.rows[i].focusable())
            return static_cast<int>(i);
    return 0;
}

int find_row(const GameView &view, PageRowKind kind)
{
    for (std::size_t i = 0; i < view.rows.size(); ++i)
        if (view.rows[i].kind == kind)
            return static_cast<int>(i);
    return -1;
}

// The part number a link has inside its mirror (by position when unnumbered).
int part_number(const GamePage &page, int mirror, const LinkRef &ref)
{
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    for (std::size_t i = 0; i < m.links.size(); ++i)
    {
        const GameLink &l = page.links[static_cast<std::size_t>(m.links[i])];
        if (l.index == ref.link_index && page.packages[static_cast<std::size_t>(l.package)].id == ref.package_id)
            return i < m.numbers.size() ? m.numbers[i] : static_cast<int>(i) + 1;
    }
    return 0;
}

ui::Color fit_color(FwFit fit)
{
    switch (fit)
    {
    case FwFit::runs:
        return ui::color::tier_green;
    case FwFit::unknown:
        return ui::color::tier_yellow;
    case FwFit::too_new:
        break;
    }
    return ui::color::tier_red;
}

} // namespace

StoreScreen::StoreScreen(Context &context) : Screen(context)
{
    if (!ctx_.catalogue_paths.empty())
        load_catalogue_state(ctx_.catalogue_paths, &state_);
    if (!ctx_.wings_prefs_paths.empty())
        load_queued_record(queued_record_paths(ctx_.wings_prefs_paths), &started_);
}

void StoreScreen::focus()
{
    // The installed games are read again whenever the Store is shown, as the
    // Library does, so "Installed" is as fresh as the last visit.
    ctx_.store.refresh_now();
}

bool StoreScreen::modal() const
{
    return prompt_.is_open() || confirm_.is_open() || list_.is_open() || panel_ != Panel::none ||
           ctx_.store.link_action().busy;
}

int StoreScreen::section_accent() const
{
    if (panel_row_ < 0)
        return theme::accent_none;
    const std::size_t row = static_cast<std::size_t>(panel_row_);
    if (panel_ == Panel::filter && panel_row_ < kGridFilterCount)
        return filter_group_accent(grid_filter_group(kGridFilters[row]));
    if (panel_ == Panel::links && row < link_rows_.size())
        return filter_group_accent(link_filter_group(link_rows_[row]));
    if (panel_ == Panel::sort && row < sort_choices_.size())
        return sort_group_accent(sort_group(sort_choices_[row]));
    return theme::accent_none;
}

ui::Rect StoreScreen::tile_rect(int index) const
{
    const int col = index % kColumns;
    const int row = index / kColumns;
    return {kGridLeft + static_cast<float>(col) * kPitch,
            kGridTop + static_cast<float>(row) * kPitch - scroll_.value, kTile, kTile};
}

// Settings > Wings > Downloads: the grid's tiers and the page's order follow
// the player's firmware, region, format and debrid choice.
void StoreScreen::refresh_prefs()
{
    const WingsLocalPrefs &prefs = prefs_source_ != nullptr ? *prefs_source_ : own_prefs_;
    // Tile details only change how tiles are drawn: no rebuild for it.
    WingsLocalPrefs ranked = prefs;
    ranked.tile_details = seen_prefs_.tile_details;
    if (have_prefs_ && ranked == seen_prefs_)
        return;
    seen_prefs_ = prefs;
    have_prefs_ = true;
    link_prefs_ = link_prefs_of(prefs);
    dirty_ = true;
    if (page_.loaded)
    {
        rerank_game_view(&page_, source_order(), link_prefs_);
        row_ = first_focusable(page_);
    }
}

std::vector<std::string> StoreScreen::source_order() const
{
    std::vector<std::string> names;
    for (const Source &s : ctx_.store.state().sources)
        names.push_back(s.name);
    return names;
}

// Reads what every package's links say (regions, firmware), a chunk at a
// time on the Store's worker: the backend answers from memory, and the grid
// filters on it once it is all there.
void StoreScreen::read_facts(std::uint64_t serial, std::size_t from)
{
    const Snapshot &snapshot = ctx_.store.state();
    if (serial != facts_serial_ || serial != snapshot.catalog_serial)
        return;
    std::vector<std::string> ids;
    for (std::size_t i = from; i < snapshot.packages.size() && ids.size() < kFactsChunk; ++i)
        ids.push_back(snapshot.packages[i].id);
    if (ids.empty())
    {
        facts_ready_ = true;
        dirty_ = true;
        return;
    }
    ctx_.store.call<std::vector<PackageFacts>>(
        Store::lane_poll,
        [ids](Backend &backend)
        {
            Result<std::vector<PackageFacts>> out;
            out.status = 200;
            for (const std::string &id : ids)
            {
                const Result<PackageDetail> detail = backend.package(id);
                out.value.push_back(detail.ok() ? package_facts(detail.value) : PackageFacts{});
            }
            return out;
        },
        [this, serial, from](Result<std::vector<PackageFacts>> &&result)
        {
            if (serial != facts_serial_)
                return;
            if (facts_.size() < from + result.value.size())
                facts_.resize(from + result.value.size());
            std::move(result.value.begin(), result.value.end(), facts_.begin() + static_cast<long>(from));
            read_facts(serial, from + result.value.size());
        });
}

// Builds what the grid shows. Runs when the catalogue, its tiers, the installed
// games, the search, the sort, a filter or the preferences changed, never per frame.
void StoreScreen::rebuild()
{
    const Snapshot &snapshot = ctx_.store.state();
    if (seen_catalog_ != snapshot.catalog_serial)
    {
        source_names_.clear();
        for (const Package &package : snapshot.packages)
        {
            if (package.source_name.empty())
                continue;
            if (std::find(source_names_.begin(), source_names_.end(), package.source_name) ==
                source_names_.end())
                source_names_.push_back(package.source_name);
        }
        std::sort(source_names_.begin(), source_names_.end());
        games_ = group_games(snapshot.packages);
        facts_.clear();
        facts_ready_ = false;
        facts_serial_ = snapshot.catalog_serial;
        if (snapshot.have_catalog)
            read_facts(facts_serial_, 0);
    }
    if (snapshot.have_catalog && !state_.source.empty() &&
        std::find(source_names_.begin(), source_names_.end(), state_.source) == source_names_.end())
    {
        state_.source.clear();
        persist();
    }
    seen_catalog_ = snapshot.catalog_serial;
    seen_tiers_ = snapshot.tiers_serial;
    seen_installed_ = snapshot.installed_serial;
    {
        PopularityHub &hub = popularity_hub();
        seen_pop_ = hub.serial();
        pop_index_ = hub.index();
        pop_view_ = PopularityView{pop_index_.get(), &hub.downloads()};
        sort_choices_ = sort_menu_order(available_sorts(&pop_view_));
    }
    installed_index_.assign(snapshot.installed, snapshot.have_installed);
    // The per-game filter answers: once per catalogue, tiers, facts chunk or firmware, not per change.
    const std::string agg_key = std::to_string(snapshot.catalog_serial) + "/" + std::to_string(snapshot.tiers_serial) +
                                "/" + std::to_string(facts_.size()) + "/" + link_prefs_.firmware;
    if (agg_key != agg_key_)
    {
        agg_ = build_grid_aggregate(snapshot.packages, snapshot.tiers, games_, facts_, link_prefs_.firmware);
        agg_key_ = agg_key;
    }
    region_names_ = agg_.region_names();

    tiles_ = select_games(GridQuery{snapshot.packages, snapshot.tiers, games_, facts_, state_, query_,
                                    installed_index_, link_prefs_, &agg_, &pop_view_});
    header_filter_ = filter_summary(state_, query_);
    // Keep the focus on the same game when the list only changed around it.
    int focus = 0;
    if (!reset_focus_ && !focus_game_.empty())
    {
        for (std::size_t i = 0; i < tiles_.size(); ++i)
        {
            if (games_.games[static_cast<std::size_t>(tiles_[i].game)].id == focus_game_)
            {
                focus = static_cast<int>(i);
                break;
            }
        }
    }
    reset_focus_ = false;
    index_ = std::clamp(focus, 0, std::max(0, static_cast<int>(tiles_.size()) - 1));
    if (!tiles_.empty())
        focus_game_ = games_.games[static_cast<std::size_t>(tiles_[static_cast<std::size_t>(index_)].game)].id;
    else
        focus_game_.clear();
    tile_focus_.assign(tiles_.size(), 0.0f);
    prefetch_row_ = -1; // the covers to warm depend on the list
    dirty_ = false;
}

// Warms the art service's disk cache for the rows just above and below what is
// on screen, so scrolling meets covers that are already there.
void StoreScreen::prefetch_art()
{
    if (!ctx_.prefetch_art || view_ != View::grid || tiles_.empty())
        return;
    const Snapshot &state = ctx_.store.state();
    const int row = index_ / kColumns;
    if (row == prefetch_row_ && prefetch_serial_ == seen_catalog_)
        return;
    prefetch_row_ = row;
    prefetch_serial_ = seen_catalog_;
    const int first = std::max(0, (row - 2) * kColumns);
    const int last = std::min(static_cast<int>(tiles_.size()), (row + 5) * kColumns);
    std::vector<std::string> keys;
    for (int i = first; i < last; ++i)
    {
        const int at = tiles_[static_cast<std::size_t>(i)].art_package;
        if (at >= 0 && at < static_cast<int>(state.packages.size()))
            keys.push_back("pkg:" + state.packages[static_cast<std::size_t>(at)].id);
    }
    ctx_.prefetch_art(keys);
}

void StoreScreen::persist()
{
    if (!ctx_.catalogue_paths.empty())
        save_catalogue_state(ctx_.catalogue_paths, state_);
}

// The player changed the search, the sort, a filter or a favourite.
void StoreScreen::changed(bool reset_focus)
{
    dirty_ = true;
    if (reset_focus)
    {
        reset_focus_ = true;
        index_ = 0;
        scroll_.snap(0.0f);
    }
}

void StoreScreen::set_query(const std::string &query)
{
    query_ = trimmed(query);
    changed(true);
}

// The search box: results follow the typing after a short pause, Cancel puts
// the old term back, Done keeps the new one and remembers it.
namespace
{
constexpr float kSearchDebounce = 0.25f; // seconds after the last edit
} // namespace

void StoreScreen::open_search(Feedback &feedback)
{
    TextPrompt::Options options;
    options.max_length = 60;
    options.allow_empty = true;
    options.search = true;
    options.chips = state_.recent_searches;
    options.placeholder = tr("Title, title id, source or tag");
    options.done_label = tr("Search");
    query_before_ = query_;
    search_wait_ = -1.0f;
    prompt_.open(feedback, tr("Search the catalogue"), query_, options);
    seen_edits_ = prompt_.changed_serial();
    // Touch hook (agent T): the touchpad types on the open keyboard from here.
}

void StoreScreen::update_search(float dt)
{
    if (!prompt_.is_open())
        return;
    if (prompt_.changed_serial() != seen_edits_)
    {
        seen_edits_ = prompt_.changed_serial();
        search_wait_ = kSearchDebounce;
    }
    if (search_wait_ >= 0.0f)
    {
        search_wait_ -= dt;
        if (search_wait_ < 0.0f)
        {
            // Recomputed only when the text really differs from what is shown.
            if (trimmed(prompt_.text()) != query_)
                set_query(prompt_.text());
            search_wait_ = -1.0f;
        }
    }
}

void StoreScreen::set_source_filter(const std::string &name)
{
    state_.source = name;
    persist();
    changed(true);
}

void StoreScreen::set_catalogue_state(const CatalogueState &state)
{
    state_ = state;
    persist();
    changed(true);
}

std::vector<std::string> StoreScreen::shown_ids() const
{
    std::vector<std::string> ids;
    const Snapshot &snapshot = ctx_.store.state();
    for (const GameTile &tile : tiles_)
    {
        if (static_cast<std::size_t>(tile.art_package) < snapshot.packages.size())
            ids.push_back(snapshot.packages[static_cast<std::size_t>(tile.art_package)].id);
    }
    return ids;
}

int StoreScreen::tile_installed(int position) const
{
    return position >= 0 && position < static_cast<int>(tiles_.size())
               ? tiles_[static_cast<std::size_t>(position)].installed
               : -1;
}

bool StoreScreen::tile_favourite(int position) const
{
    return position >= 0 && position < static_cast<int>(tiles_.size()) &&
           tiles_[static_cast<std::size_t>(position)].favourite;
}

// Square on a tile. Returns true when the game is a favourite afterwards.
bool StoreScreen::toggle_favourite_at(int position)
{
    if (position < 0 || position >= static_cast<int>(tiles_.size()))
        return false;
    const Snapshot &snapshot = ctx_.store.state();
    GameTile &tile = tiles_[static_cast<std::size_t>(position)];
    const bool now =
        toggle_game_favourite(&state_, games_.games[static_cast<std::size_t>(tile.game)], snapshot.packages);
    persist();
    tile.favourite = now;
    // The order or the list itself depends on favourites: rebuild, same focus.
    if (state_.sort == SortMode::favourites_first || state_.favourites_only)
        changed(false);
    return now;
}

// Square on the game page.
bool StoreScreen::toggle_page_favourite()
{
    const Snapshot &snapshot = ctx_.store.state();
    bool now = false;
    bool found = false;
    for (const GameGroup &g : games_.games)
    {
        if (g.id == page_game_)
        {
            now = toggle_game_favourite(&state_, g, snapshot.packages);
            found = true;
            break;
        }
    }
    if (!found && !page_.details.empty())
        now = state_.toggle_favourite(favourite_key(page_.details.front().package));
    persist();
    page_favourite_ = now;
    dirty_ = true; // the grid's hearts (and a favourites filter) follow
    return now;
}

void StoreScreen::open_utilities(int page)
{
    panel_ = page == 1 ? Panel::sort : (page == 2 ? Panel::filter : Panel::menu);
    panel_row_ = panel_ == Panel::filter ? grid_focus_row(state_) : 0;
    if (panel_ == Panel::sort)
        for (std::size_t i = 0; i < sort_choices_.size(); ++i)
            if (sort_choices_[i] == effective_sort(state_.sort, &pop_view_))
                panel_row_ = static_cast<int>(i);
}

// ---- the game page ----

void StoreScreen::open_package(const std::string &package_id)
{
    const Snapshot &snapshot = ctx_.store.state();
    if (seen_catalog_ != snapshot.catalog_serial || dirty_)
        rebuild();
    for (std::size_t i = 0; i < snapshot.packages.size() && i < games_.game_of.size(); ++i)
    {
        if (snapshot.packages[i].id == package_id)
        {
            open_game(games_.game_of[i]);
            return;
        }
    }
    // Not in the catalogue the grid knows (yet): a page of that one package.
    page_ = GameView{};
    page_.package_ids = {package_id};
    page_.art_key = "pkg:" + package_id;
    page_game_.clear();
    page_favourite_ = false;
    view_ = View::detail;
    row_ = 0;
    detail_scroll_.snap(0.0f);
    header_.reset();
    hero_urls().clear();
    row_focus_.clear();
    waiting_detail_ = true;
    load_page();
}

void StoreScreen::open_game(int game)
{
    if (game < 0 || game >= static_cast<int>(games_.games.size()))
        return;
    const Snapshot &snapshot = ctx_.store.state();
    const GameGroup &g = games_.games[static_cast<std::size_t>(game)];
    page_ = GameView{};
    page_.title = g.title;
    for (const int p : g.packages)
        page_.package_ids.push_back(snapshot.packages[static_cast<std::size_t>(p)].id);
    page_.art_key = "pkg:" + snapshot.packages[static_cast<std::size_t>(art_package(g, snapshot.packages))].id;
    page_.regions = game_regions(g, facts_);
    page_game_ = g.id;
    page_favourite_ = game_favourite(state_, g, snapshot.packages);
    view_ = View::detail;
    row_ = 0;
    detail_scroll_.snap(0.0f);
    header_.reset();
    hero_urls().clear();
    row_focus_.clear();
    waiting_detail_ = true;
    load_page();
}

// Every package of the game, with its links, in one call on the worker (the
// backend answers from the catalogue it read: no network).
void StoreScreen::load_page()
{
    const std::uint64_t id = ++page_request_;
    const std::vector<std::string> ids = page_.package_ids;
    ctx_.store.call<std::vector<PackageDetail>>(
        Store::lane_poll,
        [ids](Backend &backend)
        {
            Result<std::vector<PackageDetail>> out;
            out.status = 200;
            for (const std::string &one : ids)
            {
                Result<PackageDetail> detail = backend.package(one);
                if (detail.ok())
                {
                    out.value.push_back(std::move(detail.value));
                }
                else if (out.error.empty())
                {
                    out.error = detail.error;
                    out.status = detail.status;
                }
            }
            // One package that reads is a page; the others are left out.
            if (!out.value.empty())
            {
                out.error.clear();
                out.status = 200;
            }
            return out;
        },
        [this, id](Result<std::vector<PackageDetail>> &&result)
        {
            if (id != page_request_)
                return;
            waiting_detail_ = false;
            if (!result.ok())
            {
                page_.error = result.error;
                return;
            }
            refresh_prefs(); // the preferences as they are now, before the page is built
            if (page_.title.empty() && !result.value.empty())
                page_.title = clean_title(result.value.front().package.title);
            load_game_view(&page_, std::move(result.value), source_order(), link_prefs_);
            row_ = first_focusable(page_);
            row_focus_.assign(page_.rows.size(), 0.0f);
            detail_scroll_.snap(0.0f);
        });
}

void StoreScreen::close_detail()
{
    view_ = View::grid;
    waiting_detail_ = false;
    ++page_request_; // a page still loading is not wanted any more
    hero_urls().clear(); // the header's hero is only fetched while a page is open
    header_.reset();
    if (panel_ == Panel::links)
        panel_ = Panel::none;
}

const PageRow *StoreScreen::focused_page_row() const
{
    if (view_ != View::detail || !page_.loaded || row_ < 0 || row_ >= static_cast<int>(page_.rows.size()))
        return nullptr;
    return &page_.rows[static_cast<std::size_t>(row_)];
}

bool StoreScreen::focus_page_mirror(int mirror)
{
    for (std::size_t i = 0; i < page_.rows.size(); ++i)
    {
        if (page_.rows[i].kind == PageRowKind::mirror && page_.rows[i].mirror == mirror)
        {
            row_ = static_cast<int>(i);
            header_.on_header = false; // the list, not the header's buttons
            header_.slide.snap(1.0f);
            return true;
        }
    }
    return false;
}

void StoreScreen::open_link_filters()
{
    if (!page_.loaded || page_.page.mirrors.empty())
        return;
    link_rows_ = link_filter_rows(page_.facets, page_.filter, link_prefs_);
    panel_ = Panel::links;
    std::vector<bool> changed;
    for (const LinkFilterRow row : link_rows_)
        changed.push_back(link_row_changed(row, page_.filter, page_.sort));
    panel_row_ = first_focus_index(changed);
}

void StoreScreen::set_link_filter(const MirrorFilter &filter)
{
    if (!page_.loaded)
        return;
    set_game_filter(&page_, filter, link_prefs_);
    const int filters = find_row(page_, PageRowKind::filters);
    row_ = filters >= 0 ? filters : first_focusable(page_);
    row_focus_.assign(page_.rows.size(), 0.0f);
}

// ---- update ----

int StoreScreen::panel_rows() const
{
    switch (panel_)
    {
    case Panel::menu:
        return 3;
    case Panel::sort:
        return static_cast<int>(sort_choices_.size());
    case Panel::filter:
        return kGridFilterCount;
    case Panel::links:
        return static_cast<int>(link_rows_.size());
    case Panel::none:
        break;
    }
    return 0;
}

// One step of a grid filter row (left / right / Cross).
void StoreScreen::cycle_filter(int row, int delta, Feedback &feedback)
{
    const auto wrap = [delta](int value, int count) { return ((value + delta) % count + count) % count; };
    const GridFilter which = row >= 0 && row < kGridFilterCount ? kGridFilters[row] : GridFilter::reset;
    switch (which)
    {
    case GridFilter::source:
    {
        int at = 0;
        if (!state_.source.empty())
        {
            const auto it = std::find(source_names_.begin(), source_names_.end(), state_.source);
            at = it == source_names_.end() ? 0 : 1 + static_cast<int>(it - source_names_.begin());
        }
        at = wrap(at, 1 + static_cast<int>(source_names_.size()));
        state_.source = at == 0 ? std::string() : source_names_[static_cast<std::size_t>(at - 1)];
        break;
    }
    case GridFilter::region:
    {
        int at = 0;
        if (!state_.region.empty())
        {
            const auto it = std::find(region_names_.begin(), region_names_.end(), state_.region);
            at = it == region_names_.end() ? 0 : 1 + static_cast<int>(it - region_names_.begin());
        }
        at = wrap(at, 1 + static_cast<int>(region_names_.size()));
        state_.region = at == 0 ? std::string() : region_names_[static_cast<std::size_t>(at - 1)];
        break;
    }
    case GridFilter::firmware:
        state_.runs_on_my_fw = !state_.runs_on_my_fw;
        break;
    case GridFilter::no_browser:
        state_.no_browser = !state_.no_browser;
        break;
    case GridFilter::single_file:
        state_.single_file_only = !state_.single_file_only;
        break;
    case GridFilter::installed:
        state_.installed = static_cast<InstalledFilter>(wrap(static_cast<int>(state_.installed), 3));
        break;
    case GridFilter::favourites:
        state_.favourites_only = !state_.favourites_only;
        break;
    case GridFilter::tier:
        state_.tier = static_cast<TierFilter>(wrap(static_cast<int>(state_.tier), 3));
        break;
    case GridFilter::has_links:
        state_.has_links_only = !state_.has_links_only;
        break;
    case GridFilter::reset:
        state_.reset_filters();
        query_.clear();
        break;
    }
    persist();
    changed(true);
    feedback.play(hui::audio::Cue::select);
}

// One step of a link filter row on the game page.
void StoreScreen::cycle_link_filter_row(int row, int delta, Feedback &feedback)
{
    if (row < 0 || row >= static_cast<int>(link_rows_.size()))
        return;
    const LinkFilterRow which = link_rows_[static_cast<std::size_t>(row)];
    MirrorFilter filter = page_.filter;
    cycle_link_filter(&filter, which, delta, page_.facets);
    if (which == LinkFilterRow::sort)
        page_.sort = cycle_mirror_sort(page_.sort, delta);
    else if (which == LinkFilterRow::reset)
        page_.sort = MirrorSort::best;
    set_game_filter(&page_, filter, link_prefs_);
    link_rows_ = link_filter_rows(page_.facets, page_.filter, link_prefs_);
    // The same row keeps the focus when it is still there.
    panel_row_ = std::min(row, static_cast<int>(link_rows_.size()) - 1);
    for (std::size_t i = 0; i < link_rows_.size(); ++i)
        if (link_rows_[i] == which)
            panel_row_ = static_cast<int>(i);
    // The page behind: the focus waits on the Filters row.
    const int filters = find_row(page_, PageRowKind::filters);
    row_ = filters >= 0 ? filters : first_focusable(page_);
    row_focus_.assign(page_.rows.size(), 0.0f);
    feedback.play(hui::audio::Cue::select);
}

void StoreScreen::update_panel(const InputFrame &input, Feedback &feedback)
{
    const int rows = panel_rows();
    panel_row_ = std::clamp(panel_row_, 0, std::max(0, rows - 1));
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = panel_row_ + (input.nav == Direction::down ? 1 : -1);
        if (next >= 0 && next < rows)
        {
            panel_row_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    const bool sideways = input.nav == Direction::left || input.nav == Direction::right;
    const int delta = input.nav == Direction::right ? 1 : -1;
    if (panel_ == Panel::filter && sideways && kGridFilters[panel_row_] != GridFilter::reset)
        cycle_filter(panel_row_, delta, feedback);
    if (panel_ == Panel::links && sideways && panel_row_ < rows &&
        link_rows_[static_cast<std::size_t>(panel_row_)] != LinkFilterRow::reset)
        cycle_link_filter_row(panel_row_, delta, feedback);
    // Square: clear every filter from wherever the focus is (the Clear row is the last one).
    if (input.is_pressed(Action::west) && (panel_ == Panel::filter || panel_ == Panel::links) && rows > 0)
    {
        const int keep = panel_row_;
        if (panel_ == Panel::filter)
            cycle_filter(kGridFilterCount - 1, 1, feedback);
        else
            cycle_link_filter_row(rows - 1, 1, feedback);
        panel_row_ = std::clamp(keep, 0, std::max(0, panel_rows() - 1));
        return;
    }
    if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
    {
        if (panel_ == Panel::menu || panel_ == Panel::links || input.is_pressed(Action::menu))
        {
            panel_ = Panel::none;
        }
        else
        {
            panel_row_ = panel_ == Panel::sort ? 0 : 1;
            panel_ = Panel::menu;
        }
        feedback.play(hui::audio::Cue::modal_close);
        return;
    }
    if (!input.is_pressed(Action::confirm))
        return;
    switch (panel_)
    {
    case Panel::menu:
        if (panel_row_ == 0)
        {
            panel_ = Panel::sort;
            open_utilities(1);
            feedback.play(hui::audio::Cue::select);
        }
        else if (panel_row_ == 1)
        {
            panel_ = Panel::filter;
            panel_row_ = grid_focus_row(state_);
            feedback.play(hui::audio::Cue::select);
        }
        else
        {
            cycle_filter(kGridFilterCount - 1, 1, feedback);
            panel_ = Panel::none;
        }
        break;
    case Panel::sort:
        if (panel_row_ >= 0 && panel_row_ < static_cast<int>(sort_choices_.size()))
            state_.sort = sort_choices_[static_cast<std::size_t>(panel_row_)];
        persist();
        changed(true);
        feedback.play(hui::audio::Cue::select);
        panel_ = Panel::none;
        break;
    case Panel::filter:
        cycle_filter(panel_row_, 1, feedback);
        break;
    case Panel::links:
        cycle_link_filter_row(panel_row_, 1, feedback);
        break;
    case Panel::none:
        break;
    }
}

void StoreScreen::update_grid(const InputFrame &input, Feedback &feedback, Intent &intent)
{
    const Snapshot &state = ctx_.store.state();
    const int count = static_cast<int>(tiles_.size());
    if (input.is_pressed(Action::north))
    {
        open_search(feedback);
        return;
    }
    if (!state.have_catalog || state.packages.empty())
    {
        if (input.nav == Direction::up)
            intent.kind = Intent::Kind::top_bar;
        // An empty catalogue: the way out is a source.
        if (input.is_pressed(Action::confirm) && state.have_catalog)
        {
            intent.kind = Intent::Kind::show_tab;
            intent.tab = tab::sources;
        }
        return;
    }
    // Utilities (Options) from anywhere on the grid.
    if (input.is_pressed(Action::menu))
    {
        open_utilities(0);
        feedback.play(hui::audio::Cue::modal_open);
        return;
    }
    if (on_button_)
    {
        if (input.nav == Direction::right)
        {
            if (count > 0)
            {
                on_button_ = false;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::up)
        {
            intent.kind = Intent::Kind::top_bar;
        }
        else if (input.nav != Direction::none)
        {
            refuse(feedback, input);
        }
        if (input.is_pressed(Action::confirm))
        {
            open_utilities(0);
            feedback.play(hui::audio::Cue::modal_open);
        }
        return;
    }
    if (count == 0)
    {
        // Nothing matches: the button and Reset are the way out.
        if (input.nav == Direction::up)
            intent.kind = Intent::Kind::top_bar;
        else if (input.nav == Direction::left)
            on_button_ = true;
        if (input.is_pressed(Action::confirm))
        {
            state_.reset_filters();
            query_.clear();
            persist();
            changed(true);
            feedback.play(hui::audio::Cue::select);
        }
        return;
    }
    const int cols = kColumns;
    int next = index_;
    bool moved = false;
    if (input.nav == Direction::left)
    {
        if (index_ % cols == 0)
        {
            on_button_ = true;
            feedback.play(hui::audio::Cue::focus);
            return;
        }
        moved = true;
        next = index_ - 1;
    }
    else if (input.nav == Direction::right)
    {
        moved = index_ % cols + 1 < cols && index_ + 1 < count;
        next = index_ + 1;
    }
    else if (input.nav == Direction::down)
    {
        moved = index_ + cols < count;
        next = index_ + cols;
    }
    else if (input.nav == Direction::up)
    {
        if (index_ >= cols)
        {
            moved = true;
            next = index_ - cols;
        }
        else
        {
            intent.kind = Intent::Kind::top_bar;
            return;
        }
    }
    if (input.nav != Direction::none)
    {
        if (moved)
        {
            index_ = next;
            focus_game_ = games_.games[static_cast<std::size_t>(tiles_[static_cast<std::size_t>(index_)].game)].id;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    if (input.is_pressed(Action::west))
    {
        const bool now = toggle_favourite_at(index_);
        feedback.play(hui::audio::Cue::select);
        intent.toast = now ? std::string(tr("Added to favourites"))
                           : std::string(tr("Removed from favourites"));
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        open_game(tiles_[static_cast<std::size_t>(index_)].game);
        feedback.play(hui::audio::Cue::open);
    }
}

// The existing "download this link" flow for one (packageId, linkIndex).
void StoreScreen::activate_ref(const LinkRef &ref, Feedback &feedback)
{
    const PackageLink *found = page_.link(ref);
    pending_ref_ = ref;
    if (found == nullptr)
    {
        pending_ = Pending::notice;
        confirm_.notice(tr("Link not found"), tr("This package has no usable address for that link."));
        feedback.play(hui::audio::Cue::modal_open);
        return;
    }
    pending_link_ = *found;
    const PackageLink &link = pending_link_;
    const std::string &title = page_.title;
    switch (link.route)
    {
    case LinkRoute::missing:
        pending_ = Pending::notice;
        confirm_.notice(tr("Link not found"), tr("This package has no usable address for that link."));
        break;
    case LinkRoute::blocked:
        pending_ = Pending::notice;
        confirm_.notice(tr("Not supported"),
                        tr("{provider} is not supported by Pegasus DL, so this link cannot be "
                           "downloaded from the console.",
                           {{"provider", link.provider.empty() ? std::string(tr("This provider"))
                                                               : link.provider}}));
        break;
    case LinkRoute::native_pkg:
        pending_ = Pending::native_confirm;
        confirm_.open(tr("Send to PS5 Downloads?"),
                      tr("{title} is a .pkg file. Wings asks Pegasus DL to hand it to the PS5's own "
                         "Downloads; you finish the install from the home screen.",
                         {{"title", title}}),
                      tr("Send"), tr("Cancel"), false);
        break;
    case LinkRoute::choose_debrid:
    {
        pending_ = Pending::debrid_choice;
        std::vector<ListItem> items;
        choice_services_.clear();
        for (const std::string &service : link.services)
        {
            ListItem item;
            item.label = service;
            item.detail = tr("Resolve this link with {service}", {{"service", service}});
            items.push_back(std::move(item));
            choice_services_.push_back(service_id(service));
        }
        list_.open(tr("Choose a debrid service"), link.name, std::move(items), ListDialog::Mode::single);
        break;
    }
    case LinkRoute::debrid:
        pending_ = Pending::none;
        start_single("");
        break;
    case LinkRoute::automated:
    case LinkRoute::capture:
        if (link.needs_browser)
        {
            pending_ = Pending::browser_confirm;
            confirm_.open(
                tr("Open the PS5 browser?"),
                tr("Pegasus DL opens this link in the PS5 browser so you can complete the check "
                   "(a captcha). Finish it there, then press the PS button and return to Wings. "
                   "The download is added to the queue by itself."),
                tr("Open browser"), tr("Cancel"), false);
        }
        else
        {
            pending_ = Pending::none;
            start_single("");
        }
        break;
    case LinkRoute::browser_only:
        pending_ = Pending::browser_confirm;
        confirm_.open(tr("Open in the PS5 browser?"),
                      tr("{provider} cannot be downloaded by Pegasus DL. The link opens in the PS5 "
                         "browser and you download it there yourself.",
                         {{"provider", link.provider.empty() ? link.host : link.provider}}),
                      tr("Open browser"), tr("Cancel"), false);
        break;
    }
    if (confirm_.is_open() || list_.is_open())
        feedback.play(hui::audio::Cue::modal_open);
}

void StoreScreen::start_single(const std::string &service)
{
    ctx_.store.start_link(pending_ref_.package_id, pending_ref_.link_index, service);
    popularity_hub().count_download(page_.title_ids); // "Your most downloaded"
    single_ref_ = pending_ref_;
    single_waiting_ = true;
}

// A card: one link runs the link flow; a multipart set offers all parts or one.
void StoreScreen::activate_mirror(int mirror, Feedback &feedback)
{
    if (mirror < 0 || mirror >= static_cast<int>(page_.page.mirrors.size()))
        return;
    const Mirror &m = page_.page.mirrors[static_cast<std::size_t>(mirror)];
    if (!m.multipart())
    {
        activate_ref(page_.first_ref(mirror), feedback);
        return;
    }
    parts_mirror_ = mirror;
    parts_plan_ = plan_parts(page_, mirror, started_);
    std::vector<ListItem> items;
    ListItem all;
    all.label = parts_plan_.action;
    all.enabled = !parts_plan_.refs.empty();
    if (parts_plan_.one_at_a_time)
        all.detail = tr("Each part opens the PS5 browser for a check, so they go one at a time.");
    else
        all.detail = tr("Queued one after another, in catalogue order.");
    items.push_back(std::move(all));
    for (std::size_t i = 0; i < m.links.size(); ++i)
    {
        const GameLink &l = page_.page.links[static_cast<std::size_t>(m.links[i])];
        const LinkRef ref{page_.page.packages[static_cast<std::size_t>(l.package)].id, l.index};
        ListItem item;
        item.label = tr("Part {n}", {{"n", std::to_string(i < m.numbers.size() ? m.numbers[i]
                                                                                : static_cast<int>(i) + 1)}});
        item.detail = l.name;
        if (started_.has(ref))
            item.right = tr("Queued");
        items.push_back(std::move(item));
    }
    const std::string warning = m.warning();
    pending_ = Pending::parts_menu;
    list_.open(m.host + " \xC2\xB7 " + m.parts_text(),
               warning.empty() ? std::string(tr("A multi-part archive: you need every part to extract it."))
                               : warning,
               std::move(items), ListDialog::Mode::single);
    feedback.play(hui::audio::Cue::modal_open);
}

void StoreScreen::activate_row(Feedback &feedback)
{
    const PageRow *row = focused_page_row();
    if (row == nullptr)
        return;
    switch (row->kind)
    {
    case PageRowKind::best:
    case PageRowKind::mirror:
        activate_mirror(row->mirror, feedback);
        break;
    case PageRowKind::filters:
        open_link_filters();
        feedback.play(hui::audio::Cue::modal_open);
        break;
    case PageRowKind::empty:
        if (page_.filter.active())
        {
            set_game_filter(&page_, MirrorFilter{}, link_prefs_);
            page_.seed_note.clear();
            const int filters = find_row(page_, PageRowKind::filters);
            row_ = filters >= 0 ? filters : first_focusable(page_);
            row_focus_.assign(page_.rows.size(), 0.0f);
            feedback.play(hui::audio::Cue::select);
        }
        else
        {
            refuse(feedback, InputFrame{});
        }
        break;
    case PageRowKind::hidden:
    case PageRowKind::section:
    case PageRowKind::release:
        break;
    }
}

// "Download all parts": every part through the same start_link() call the
// single cards use, one at a time in catalogue order (download_all.cpp).
void StoreScreen::start_batch(const std::string &service)
{
    if (parts_plan_.refs.empty() || parts_mirror_ < 0 ||
        parts_mirror_ >= static_cast<int>(page_.page.mirrors.size()) || batch_.active)
        return;
    popularity_hub().count_download(page_.title_ids);
    std::vector<int> numbers;
    for (const LinkRef &ref : parts_plan_.refs)
        numbers.push_back(part_number(page_.page, parts_mirror_, ref));
    batch_.begin(
        parts_plan_.refs, std::move(numbers), parts_plan_.host,
        [this, service](const LinkRef &ref, DownloadAll::Done done)
        { ctx_.store.start_link(ref.package_id, ref.link_index, service, std::move(done)); },
        [this](const LinkRef &ref)
        {
            started_.add(ref);
            save_started();
        },
        [this] { finish_batch(); });
}

void StoreScreen::save_started()
{
    if (!ctx_.wings_prefs_paths.empty())
        save_queued_record(queued_record_paths(ctx_.wings_prefs_paths), started_);
}

void StoreScreen::finish_batch()
{
    // The single-link reactions (notices, toasts) are not for these.
    seen_link_ = ctx_.store.state().link_serial;
    seen_link_action_ = ctx_.store.link_action().serial;
    if (batch_.end == DownloadAll::End::finished)
    {
        batch_toast_ = batch_.toast();
        return;
    }
    pending_ = Pending::notice;
    confirm_.notice(batch_.report_title(), batch_.report_message());
}

void StoreScreen::update_detail(const InputFrame &input, Feedback &feedback, Intent &intent)
{
    if (input.is_pressed(Action::back))
    {
        close_detail();
        feedback.play(hui::audio::Cue::back);
        return;
    }
    if (!page_.loaded)
        return;
    if (input.is_pressed(Action::west))
    {
        const bool now = toggle_page_favourite();
        feedback.play(hui::audio::Cue::select);
        intent.toast = now ? std::string(tr("Added to favourites"))
                           : std::string(tr("Removed from favourites"));
    }
    if (input.is_pressed(Action::menu) && !page_.page.mirrors.empty())
    {
        open_link_filters();
        feedback.play(hui::audio::Cue::modal_open);
        return;
    }
    const int count = static_cast<int>(page_.rows.size());
    row_ = std::clamp(row_, 0, std::max(0, count - 1));
    if (header_.on_header)
    {
        update_header(input, feedback, intent);
        return;
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int step = input.nav == Direction::down ? 1 : -1;
        int next = row_ + step;
        while (next >= 0 && next < count && !page_.rows[static_cast<std::size_t>(next)].focusable())
            next += step;
        if (next >= 0 && next < count)
        {
            row_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (step < 0)
        {
            // Up from the first card: back to the header's buttons.
            header_.on_header = true;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    else if (input.nav == Direction::left || input.nav == Direction::right)
    {
        refuse(feedback, input);
    }
    if (input.is_pressed(Action::confirm))
        activate_row(feedback);
}

// The header's buttons: Install slides the page down to the downloads,
// Favourite toggles, the three dots open the page's other actions.
void StoreScreen::update_header(const InputFrame &input, Feedback &feedback, Intent &intent)
{
    const auto to_list = [&]
    {
        header_.on_header = false;
        row_ = first_focusable(page_);
        row_focus_.assign(page_.rows.size(), 0.0f);
    };
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int next = header_step(header_.button, input.nav);
        if (next != header_.button)
        {
            header_.button = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    else if (input.nav == Direction::down)
    {
        to_list();
        feedback.play(hui::audio::Cue::focus);
    }
    else if (input.nav == Direction::up)
    {
        refuse(feedback, input);
    }
    if (!input.is_pressed(Action::confirm))
        return;
    switch (static_cast<HeaderButton>(header_.button))
    {
    case HeaderButton::install:
        to_list();
        feedback.play(hui::audio::Cue::select);
        break;
    case HeaderButton::favourite:
    {
        const bool now = toggle_page_favourite();
        feedback.play(hui::audio::Cue::select);
        intent.toast = now ? std::string(tr("Added to favourites"))
                           : std::string(tr("Removed from favourites"));
        break;
    }
    case HeaderButton::more:
    {
        header_.more = header_more_items(!page_.page.mirrors.empty(), page_.filter.active());
        if (header_.more.empty())
        {
            refuse(feedback, InputFrame{});
            break;
        }
        std::vector<ListItem> items;
        for (const HeaderMore item : header_.more)
        {
            ListItem row;
            switch (item)
            {
            case HeaderMore::filters:
                row.label = tr("Filter the downloads");
                row.detail = link_filter_summary(page_.filter, link_prefs_);
                break;
            case HeaderMore::clear_filters:
                row.label = tr("Clear filters");
                break;
            case HeaderMore::cycle_order:
                row.label = tr("Order of the downloads");
                row.right = mirror_sort_label(page_.sort);
                break;
            }
            items.push_back(std::move(row));
        }
        pending_ = Pending::more_menu;
        list_.open(tr("More"), page_.title, std::move(items), ListDialog::Mode::single);
        feedback.play(hui::audio::Cue::modal_open);
        break;
    }
    }
}

void StoreScreen::apply_more(HeaderMore item, Feedback &feedback)
{
    switch (item)
    {
    case HeaderMore::filters:
        open_link_filters();
        feedback.play(hui::audio::Cue::modal_open);
        break;
    case HeaderMore::clear_filters:
        set_game_filter(&page_, MirrorFilter{}, link_prefs_);
        page_.seed_note.clear();
        row_ = first_focusable(page_);
        row_focus_.assign(page_.rows.size(), 0.0f);
        feedback.play(hui::audio::Cue::select);
        break;
    case HeaderMore::cycle_order:
        page_.sort = cycle_mirror_sort(page_.sort, 1);
        set_game_filter(&page_, page_.filter, link_prefs_);
        row_ = first_focusable(page_);
        row_focus_.assign(page_.rows.size(), 0.0f);
        feedback.play(hui::audio::Cue::select);
        break;
    }
}

void StoreScreen::update_flow(Feedback &, Intent &intent)
{
    const Snapshot &state = ctx_.store.state();
    const ActionState &action = ctx_.store.link_action();
    if (batch_.active)
    {
        // The parts report through finish_part().
        seen_link_action_ = action.serial;
        seen_link_ = state.link_serial;
        return;
    }
    if (action.serial != seen_link_action_)
    {
        seen_link_action_ = action.serial;
        if (!action.error.empty())
        {
            single_waiting_ = false;
            pending_ = Pending::notice;
            confirm_.notice(tr("Could not start the download"), action.error);
        }
    }
    if (state.link_serial == seen_link_)
        return;
    seen_link_ = state.link_serial;
    const LinkResult &result = state.link_result;
    if (single_waiting_)
    {
        // Remembered so "Download all parts" leaves it out.
        single_waiting_ = false;
        if (result.kind == LinkResult::Kind::dispatched || result.kind == LinkResult::Kind::capture)
        {
            started_.add(single_ref_);
            save_started();
        }
    }
    switch (result.kind)
    {
    case LinkResult::Kind::dispatched:
        if (result.dispatch.native)
        {
            pending_ = Pending::notice;
            confirm_.notice(result.dispatch.native_title.empty()
                                ? std::string(tr("Sent to PS5 Downloads"))
                                : result.dispatch.native_title,
                            result.dispatch.native_message.empty()
                                ? std::string(tr("Open Downloads on the PS5 home screen to follow it."))
                                : result.dispatch.native_message);
        }
        else if (!result.dispatch.toast.empty())
        {
            intent.toast = result.dispatch.toast;
        }
        break;
    case LinkResult::Kind::select_files:
    {
        pending_ = Pending::select_files;
        selection_id_ = result.selection_id;
        selection_service_ = result.service;
        choice_services_.clear();
        std::vector<ListItem> items;
        for (const DebridFile &file : result.files)
        {
            ListItem item;
            item.label = file.name;
            item.right = format_bytes(file.size_bytes);
            item.checked = true;
            items.push_back(std::move(item));
            choice_services_.push_back(std::to_string(file.index));
        }
        list_.open(tr("Choose the files to download"),
                   tr("The debrid service found several files for this link."), std::move(items),
                   ListDialog::Mode::multi, tr("Download"));
        break;
    }
    case LinkResult::Kind::browser_opened:
        intent.toast = result.message.empty() ? std::string(tr("Link opened in the PS5 browser."))
                                              : result.message;
        break;
    case LinkResult::Kind::blocked:
        pending_ = Pending::notice;
        confirm_.notice(tr("Not supported"), result.message);
        break;
    case LinkResult::Kind::capture:
        break; // the app shows the capture panel
    }
}

Intent StoreScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    // The Store shows installed status: ask for the game list while it is open.
    if (state.online())
        ctx_.store.want_installed(true);
    refresh_prefs();
    if (dirty_ || seen_catalog_ != state.catalog_serial || seen_tiers_ != state.tiers_serial ||
        seen_installed_ != state.installed_serial || seen_pop_ != popularity_hub().serial())
        rebuild();
    prefetch_art();
    if (!batch_toast_.empty())
    {
        intent.toast = batch_toast_;
        batch_toast_.clear();
    }

    // Circle stops "Download all parts" after the part in flight.
    if (batch_.active && input.is_pressed(Action::back))
        batch_.cancel();
    // Panels: the one that is open takes the whole input.
    const bool had_prompt = prompt_.is_open();
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();
    const bool had_panel = panel_ != Panel::none;
    const bool busy = ctx_.store.link_action().busy;

    const TextPrompt::Result typed = prompt_.update(had_prompt ? input : kIdle, dt, feedback);
    if (typed == TextPrompt::Result::accepted)
    {
        if (trimmed(prompt_.text()) != query_)
            set_query(prompt_.text());
        if (state_.add_recent_search(query_))
            persist();
        search_wait_ = -1.0f;
        on_button_ = false;
    }
    else if (typed == TextPrompt::Result::cancelled)
    {
        // Live results are undone: the term from before the box opened comes back.
        if (query_ != query_before_)
            set_query(query_before_);
        search_wait_ = -1.0f;
    }
    else
    {
        update_search(dt);
    }
    const ConfirmDialog::Result confirmed =
        confirm_.update(had_confirm ? input : kIdle, dt, feedback);
    if (confirmed != ConfirmDialog::Result::none)
    {
        const Pending was = pending_;
        pending_ = Pending::none;
        if (confirmed == ConfirmDialog::Result::confirmed &&
            (was == Pending::native_confirm || was == Pending::browser_confirm))
        {
            start_single("");
        }
        else if (confirmed == ConfirmDialog::Result::confirmed && was == Pending::parts_confirm)
        {
            // Several debrid services take these parts: one choice for all of them.
            const PackageLink *first =
                parts_plan_.refs.empty() ? nullptr : page_.link(parts_plan_.refs.front());
            if (parts_plan_.one_at_a_time && !parts_plan_.refs.empty())
            {
                activate_ref(parts_plan_.refs.front(), feedback);
            }
            else if (first != nullptr && first->route == LinkRoute::choose_debrid && !first->services.empty())
            {
                std::vector<ListItem> items;
                choice_services_.clear();
                for (const std::string &service : first->services)
                {
                    ListItem item;
                    item.label = service;
                    item.detail = tr("Resolve every part with {service}", {{"service", service}});
                    items.push_back(std::move(item));
                    choice_services_.push_back(service_id(service));
                }
                pending_ = Pending::parts_service;
                list_.open(tr("Choose a debrid service"), parts_plan_.host, std::move(items),
                           ListDialog::Mode::single);
            }
            else
            {
                start_batch("");
            }
        }
    }
    const ListDialog::Result listed = list_.update(had_list ? input : kIdle, dt, feedback);
    if (listed != ListDialog::Result::none)
    {
        const Pending was = pending_;
        pending_ = Pending::none;
        const int chosen = list_.chosen();
        const bool service_ok = chosen >= 0 && chosen < static_cast<int>(choice_services_.size());
        if (listed == ListDialog::Result::chosen && was == Pending::debrid_choice && service_ok)
        {
            start_single(choice_services_[static_cast<std::size_t>(chosen)]);
        }
        else if (listed == ListDialog::Result::chosen && was == Pending::parts_service && service_ok)
        {
            start_batch(choice_services_[static_cast<std::size_t>(chosen)]);
        }
        else if (listed == ListDialog::Result::chosen && was == Pending::more_menu && chosen >= 0 &&
                 chosen < static_cast<int>(header_.more.size()))
        {
            apply_more(header_.more[static_cast<std::size_t>(chosen)], feedback);
        }
        else if (listed == ListDialog::Result::confirmed && was == Pending::select_files)
        {
            std::vector<int> indexes;
            for (const int item : list_.checked())
            {
                if (item >= 0 && item < static_cast<int>(choice_services_.size()))
                    indexes.push_back(std::atoi(choice_services_[static_cast<std::size_t>(item)].c_str()));
            }
            ctx_.store.debrid_select(selection_id_, indexes, selection_service_);
        }
        else if (listed == ListDialog::Result::chosen && was == Pending::parts_menu && parts_mirror_ >= 0 &&
                 parts_mirror_ < static_cast<int>(page_.page.mirrors.size()))
        {
            if (chosen == 0)
            {
                if (parts_plan_.refs.empty())
                {
                    // Every part is already queued: nothing to do.
                }
                else
                {
                    pending_ = Pending::parts_confirm;
                    confirm_.open(parts_plan_.title, parts_plan_.message, tr("Download all"), tr("Cancel"),
                                  false);
                    feedback.play(hui::audio::Cue::modal_open);
                }
            }
            else
            {
                const Mirror &m = page_.page.mirrors[static_cast<std::size_t>(parts_mirror_)];
                const int at = chosen - 1;
                if (at >= 0 && at < static_cast<int>(m.links.size()))
                {
                    const GameLink &l = page_.page.links[static_cast<std::size_t>(m.links[static_cast<std::size_t>(at)])];
                    activate_ref(LinkRef{page_.page.packages[static_cast<std::size_t>(l.package)].id, l.index},
                                 feedback);
                }
            }
        }
    }
    if (had_panel && !had_prompt && !had_confirm && !had_list && !busy)
        update_panel(input, feedback);
    panel_in_.target = panel_ != Panel::none ? 1.0f : 0.0f;
    panel_in_.update(dt, 20.0f);

    const bool panel = had_prompt || had_confirm || had_list || had_panel || busy;
    update_flow(feedback, intent);
    if (!panel && focused && state.online())
    {
        if (view_ == View::grid)
            update_grid(input, feedback, intent);
        else
            update_detail(input, feedback, intent);
    }
    else if (focused && !state.online() && input.nav == Direction::up && !panel)
    {
        intent.kind = Intent::Kind::top_bar;
    }

    // Focus fades and scrolling.
    const int count = static_cast<int>(tiles_.size());
    tile_focus_.resize(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i)
    {
        float &value = tile_focus_[static_cast<std::size_t>(i)];
        const float target = focused && view_ == View::grid && !on_button_ && i == index_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    button_focus_.target = focused && view_ == View::grid && on_button_ && !had_panel ? 1.0f : 0.0f;
    button_focus_.update(dt, 16.0f);
    // Row-wise: the focused row sits second from the top once the list scrolls.
    const int row = index_ / kColumns;
    scroll_.target = std::max(0.0f, static_cast<float>(row - 1) * kPitch);
    scroll_.update(dt, 11.0f);

    const int rows = static_cast<int>(page_.rows.size());
    row_focus_.resize(static_cast<std::size_t>(rows), 0.0f);
    for (int i = 0; i < rows; ++i)
    {
        float &value = row_focus_[static_cast<std::size_t>(i)];
        const float target =
            focused && view_ == View::detail && !header_.on_header && i == row_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    if (view_ == View::detail)
    {
        header_.update(dt, focused);
        // The hero art URL, once the popularity index is there: asked for only
        // while this page is open (never for grid tiles).
        if (page_.loaded && !header_.hero_resolved && pop_index_ != nullptr)
        {
            header_.hero_resolved = true;
            const std::string url = pop_index_->hero(page_.title_ids);
            if (!url.empty() && hero_urls().set(page_.art_key, url))
                header_.hero_url = url;
            const std::string low = pop_index_->hero_low(page_.title_ids);
            if (!low.empty() && hero_urls().set_low(page_.art_key, low))
                header_.hero_low_url = low;
        }
        // No record in the index: ask for the live Steam review summary (idempotent, one worker, cached).
        if (page_.loaded && pop_index_ != nullptr && pop_index_->best(page_.title_ids) == nullptr)
            if (SteamReviewService *live = steam_reviews())
                live->request(page_.title_ids, page_.title);
    }
    if (view_ == View::detail && row_ >= 0 && row_ < rows)
    {
        // Keep the focused card in view, with the headers just above it.
        const PageRow &focus_row = page_.rows[static_cast<std::size_t>(row_)];
        float top = focus_row.y;
        for (int k = row_ - 1; k >= 0 && !page_.rows[static_cast<std::size_t>(k)].focusable(); --k)
            top = page_.rows[static_cast<std::size_t>(k)].y;
        // Keep the focused card clear of the "more below" fade at the bottom.
        const float height = kPageListBottom - kPageListTop - kPageFadeClear;
        float target = detail_scroll_.target;
        if (row_ == first_focusable(page_))
            target = 0.0f;
        else if (top < target)
            target = top;
        else if (focus_row.y + focus_row.h > target + height)
            target = focus_row.y + focus_row.h - height;
        detail_scroll_.target = std::max(0.0f, target);
    }
    detail_scroll_.update(dt, 11.0f);
    return intent;
}

// ---- drawing ----

// The header above the grid: "All games" at the left, the sort and the filter
// at the right, as in the PlayStation Store.
void StoreScreen::draw_header(hui::gfx::DrawList &list, float legend_alpha) const
{
    const ui::Type &type = ctx_.type;
    const Snapshot &snapshot = ctx_.store.state();
    const float w = ui::text(list, type.regular, tr("All games"), kGridLeft, kHeaderBaseline, 30.0f,
                             ui::color::text);
    float left_end = kGridLeft + w;
    if (snapshot.have_catalog)
    {
        const std::string count =
            tiles_.size() == games_.games.size()
                ? i18n::count(static_cast<long>(tiles_.size()))
                : tr("{shown} of {count}", {{"shown", i18n::count(static_cast<long>(tiles_.size()))},
                                            {"count", i18n::count(static_cast<long>(games_.games.size()))}});
        left_end += 20.0f + ui::text(list, type.regular, count, kGridLeft + w + 20.0f, kHeaderBaseline, 21.0f,
                                     ui::color::text3);
    }
    const float right = kGridLeft + 5.0f * kTile + 4.0f * kGap;
    const std::string filter = tr("Filter by: {value}", {{"value", header_filter_}});
    const float fw = ui::text_fit(list, type.regular, filter, right, kHeaderBaseline, 26.0f, 760.0f,
                                  ui::color::text, ui::Align::right);
    const float sep = right - fw - 16.0f;
    list.rounded_rect({sep - 1.0f, kHeaderBaseline - 24.0f, 2.0f, 30.0f}, 1.0f,
                      ui::Color::rgb(0xffffff, 0.5f));
    const std::string sort = tr("Sort by: {value}", {{"value", std::string(sort_label(effective_sort(state_.sort, &pop_view_)))}});
    const float sw = ui::text(list, type.regular, sort, sep - 16.0f, kHeaderBaseline, 26.0f, ui::color::text,
                              ui::Align::right);
    // The colours always come with their words on the tiles; this explains
    // them, in the header (the hint bar covers the bottom of the screen). Only
    // when it fits between the count and the sort, and while a tile shows its
    // tier (Tile details).
    const LinkTier items[] = {LinkTier::supported, LinkTier::debrid, LinkTier::unknown,
                              LinkTier::unsupported};
    constexpr float kLegend = 18.0f;
    float legend_w = 0.0f;
    for (const LinkTier tier : items)
        legend_w += 18.0f + type.regular.measure(tier_label(tier), kLegend) + 24.0f;
    float x = left_end + 48.0f;
    if (legend_alpha > 0.01f && !tiles_.empty() && x + legend_w < sep - 16.0f - sw - 48.0f)
    {
        list.push_opacity(legend_alpha);
        for (const LinkTier tier : items)
        {
            ui::Color c = tier_color(tier);
            if (tier == LinkTier::unsupported)
                c = ui::Color::rgb(0x8a8a8a, 0.7f);
            list.circle(x + 6.0f, kHeaderBaseline - 6.0f, 5.0f, c);
            x += 18.0f + ui::text(list, type.regular, tier_label(tier), x + 18.0f, kHeaderBaseline, kLegend,
                                  ui::color::text3) + 24.0f;
        }
        list.pop_opacity();
    }
}

// The round sort / filter button at the left of the first row.
void StoreScreen::draw_utility_button(hui::gfx::DrawList &list, float focus) const
{
    const ui::Rect r{kButtonX - kButtonRadius, kButtonY - kButtonRadius, 2.0f * kButtonRadius,
                     2.0f * kButtonRadius};
    ui::focus_ring(list, r, kButtonRadius, focus);
    list.circle(kButtonX, kButtonY, kButtonRadius, ui::Color::rgb(0x2a2a2a));
    // Sort icon: an arrow pointing down beside three shortening bars.
    const ui::Color ink = ui::color::text;
    list.line(kButtonX - 14.0f, kButtonY - 13.0f, kButtonX - 14.0f, kButtonY + 12.0f, 3.0f, ink);
    list.line(kButtonX - 21.0f, kButtonY + 5.0f, kButtonX - 14.0f, kButtonY + 13.0f, 3.0f, ink);
    list.line(kButtonX - 7.0f, kButtonY + 5.0f, kButtonX - 14.0f, kButtonY + 13.0f, 3.0f, ink);
    list.rounded_rect({kButtonX - 4.0f, kButtonY - 15.0f, 26.0f, 4.0f}, 2.0f, ink);
    list.rounded_rect({kButtonX - 4.0f, kButtonY - 5.0f, 19.0f, 4.0f}, 2.0f, ink);
    list.rounded_rect({kButtonX - 4.0f, kButtonY + 5.0f, 12.0f, 4.0f}, 2.0f, ink);
    list.rounded_rect({kButtonX - 4.0f, kButtonY + 15.0f, 5.0f, 4.0f}, 2.0f, ink);
    // A dot when the list is not the plain A to Z catalogue.
    if (state_.filtered() || !query_.empty() || state_.sort != SortMode::az)
        list.circle(kButtonX + kButtonRadius * 0.72f, kButtonY - kButtonRadius * 0.72f, 6.5f,
                    ui::color::text);
}

// One tile: the cover, a black gradient rising from the bottom, the title in
// white (ellipsis), the installed status, a heart, the best-link tier and the
// game's regions.
void StoreScreen::draw_tile_cell(hui::gfx::DrawList &list, const GameTile &tile, const ui::Rect &r,
                                 float focus) const
{
    const ui::Type &type = ctx_.type;
    const Snapshot &snapshot = ctx_.store.state();
    if (tile.game < 0 || tile.game >= static_cast<int>(games_.games.size()) || tile.art_package < 0 ||
        tile.art_package >= static_cast<int>(snapshot.packages.size()))
        return;
    const GameGroup &game = games_.games[static_cast<std::size_t>(tile.game)];
    const Package &package = snapshot.packages[static_cast<std::size_t>(tile.art_package)];
    const float lift = 8.0f * focus;
    const ui::Rect t = r.inset(-lift);
    draw_tile(ctx_, list, "pkg:" + package.id, game.title, t, focus);
    // Readability on any art: black from the bottom edge up.
    const float gh = t.h * 0.5f;
    list.gradient_rect({t.x, t.y + t.h - gh, t.w, gh}, ui::kArtRadius,
                       ui::Color::rgb(0x000000, 0.0f), ui::Color::rgb(0x000000, 0.9f));
    const float pad = 16.0f;
    const float inner = t.w - 2.0f * pad;
    float baseline = t.y + t.h - 16.0f;
    // Status under the title; nothing while the game list is unknown.
    if (tile.installed == 1)
    {
        ui::icon_check(list, t.x + pad + 8.0f, baseline - 6.0f, 18.0f, ui::color::text, 2.6f);
        ui::text(list, type.semibold, tr("Installed"), t.x + pad + 24.0f, baseline, 17.0f,
                 ui::color::text);
        baseline -= 28.0f;
    }
    else if (tile.installed == 0)
    {
        ui::text(list, type.regular, tr("Not installed"), t.x + pad, baseline, 17.0f,
                 ui::color::text2);
        baseline -= 28.0f;
    }
    else
    {
        baseline -= 4.0f;
    }
    // Title: one line, or two with an ellipsis.
    constexpr float kSize = 21.0f;
    if (type.medium.measure(game.title, kSize) <= inner)
        ui::text(list, type.medium, game.title, t.x + pad, baseline, kSize, ui::color::text);
    else
        ui::paragraph(list, type.medium, game.title, t.x + pad, baseline - 26.0f, kSize, inner,
                      26.0f, ui::color::text, 2);
    // The best link: a dot and a word (green supported, yellow debrid, neutral
    // unknown, muted unsupported).
    const LinkTier tier = packed_tier(tile.tier);
    ui::Color dot = tier_color(tier);
    if (tier == LinkTier::unsupported)
        dot = ui::Color::rgb(0x8a8a8a, 0.7f);
    const char *label = tier_label(tier);
    constexpr float kTierSize = 16.0f;
    const float tw = type.medium.measure(label, kTierSize);
    const ui::Rect pill{t.x + 12.0f, t.y + 12.0f, tw + 38.0f, 32.0f};
    // Settings > Store grid > Tile details: the pill and the regions only
    // (always, never, or fading in with the focus).
    const WingsLocalPrefs &prefs = prefs_source_ != nullptr ? *prefs_source_ : own_prefs_;
    const float details = tile_detail_alpha(prefs.tile_details, focus);
    if (details > 0.01f)
    {
    list.push_opacity(details);
    list.rounded_rect(pill, pill.h * 0.5f, ui::color::glass);
    list.circle(pill.x + 14.0f, pill.cy(), 5.0f, dot);
    ui::text(list, type.medium, label, pill.x + 27.0f, pill.cy() + 6.0f, kTierSize,
             ui::color::text);
    // Regions under the pill, as many as fit ("+2" for the rest).
    if (!tile.regions.empty())
    {
        constexpr float kRegionSize = 15.0f;
        const float cy = pill.y + pill.h + 23.0f;
        const float limit = t.x + t.w - 12.0f;
        float cx = pill.x;
        const std::size_t n = tile.regions.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::string more = "+" + std::to_string(n - i);
            const float w = type.semibold.measure(tile.regions[i], kRegionSize) + 28.0f;
            const float more_w = type.semibold.measure(more, kRegionSize) + 28.0f;
            const bool last = i + 1 == n;
            if (i == 3 || cx + w + (last ? 0.0f : more_w + 6.0f) > limit)
            {
                ui::chip(list, type, cx, cy, more, ui::color::glass, ui::color::text, kRegionSize);
                break;
            }
            cx += ui::chip(list, type, cx, cy, tile.regions[i], ui::color::glass, ui::color::text, kRegionSize) + 6.0f;
        }
    }
    list.pop_opacity();
    }
    float badge_right = t.x + t.w - 12.0f; // popularity badge: top right, left of the heart
    if (tile.favourite)
        badge_right -= 46.0f;
    if (tile.popularity >= 0)
    {
        const std::string text = std::to_string(tile.popularity);
        const float bw = type.semibold.measure(text, 16.0f) + 28.0f;
        ui::chip(list, type, badge_right - bw, pill.cy(), text, ui::color::glass, ui::color::text, 16.0f);
    }
    if (tile.favourite)
    {
        const float cx = t.x + t.w - 30.0f;
        const float cy = pill.cy();
        list.circle(cx, cy, 18.0f, ui::color::glass);
        ui::icon_heart_filled(list, cx, cy + 1.0f, 20.0f, ui::color::text);
    }
}

void StoreScreen::draw_grid(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    if (!state.online())
    {
        draw_heading(ctx_, list, tr("Store"), {});
        draw_waiting(ctx_, list, 330.0f);
        return;
    }
    if (!state.have_catalog)
    {
        draw_heading(ctx_, list, tr("Store"), {});
        // The catalogue is large: show the shell with skeleton tiles meanwhile.
        const float pulse = 0.5f + 0.5f * std::sin(ctx_.clock * 3.0f);
        for (int i = 0; i < kColumns * 2; ++i)
            list.rounded_rect(tile_rect(i), ui::kArtRadius,
                              ui::Color::rgb(0xffffff, 0.05f + 0.03f * pulse));
        ui::text(list, type.regular,
                 state.catalog_error.empty()
                     ? std::string(tr("Loading the catalogue\xE2\x80\xA6"))
                     : tr("The catalogue could not be read: {error}", {{"error", state.catalog_error}}),
                 kGridLeft, kHeaderBaseline, 22.0f, ui::color::text3);
        return;
    }
    if (state.packages.empty())
    {
        draw_heading(ctx_, list, tr("Store"), {});
        // Nothing yet: the Sources tab is where packages come from.
        const ui::Rect card{ui::kGutter, 300.0f, 1920.0f - 2.0f * ui::kGutter, 220.0f};
        ui::modal(list, card);
        ui::text(list, type.light, tr("No packages yet"), card.x + 48.0f, card.y + 86.0f, 40.0f,
                 ui::color::text);
        ui::paragraph(list, type.regular,
                      tr("Packages come from the sources you add to Pegasus DL. Open the Sources "
                         "tab to add one, or to pick from the suggested list."),
                      card.x + 48.0f, card.y + 134.0f, 24.0f, card.w - 96.0f, 34.0f,
                      ui::color::text2, 2);
        const ui::Rect go{card.x + card.w - 48.0f - 260.0f, card.y + card.h - 40.0f - ui::kButtonHeight,
                          260.0f, ui::kButtonHeight};
        ui::button(list, type, go, tr("Open Sources"), ui::ButtonKind::primary, focused ? 1.0f : 0.0f);
        return;
    }
    {
        const WingsLocalPrefs &prefs = prefs_source_ != nullptr ? *prefs_source_ : own_prefs_;
        const bool on_tile = focused && !on_button_ && index_ >= 0 &&
                             static_cast<std::size_t>(index_) < tile_focus_.size();
        draw_header(list, tier_legend_alpha(prefs.tile_details,
                                            on_tile ? tile_focus_[static_cast<std::size_t>(index_)] : 0.0f));
    }
    draw_utility_button(list, button_focus_.value);
    if (tiles_.empty())
    {
        ui::text(list, type.light, tr("No games match"), kGridLeft, kGridTop + 90.0f, 44.0f,
                 ui::color::text);
        ui::text(list, type.regular,
                 tr("Nothing fits the search and the filters. Press Cross to reset them, or "
                    "Options to change them."),
                 kGridLeft, kGridTop + 140.0f, 24.0f, ui::color::text2);
        return;
    }
    list.push_clip({0.0f, kGridTop - 24.0f, 1920.0f, kGridBottom - kGridTop + 24.0f});
    for (std::size_t i = 0; i < tiles_.size(); ++i)
    {
        const ui::Rect r = tile_rect(static_cast<int>(i));
        if (r.y + r.h < kGridTop - 24.0f || r.y > kGridBottom)
            continue;
        const float f = i < tile_focus_.size() && focused ? tile_focus_[i] : 0.0f;
        draw_tile_cell(list, tiles_[i], r, f);
    }
    list.pop_clip();
}

void StoreScreen::draw_panel(hui::gfx::DrawList &list) const
{
    const float a = std::clamp(panel_in_.value, 0.0f, 1.0f);
    if (a < 0.01f)
        return;
    const ui::Type &type = ctx_.type;
    list.push_opacity(a);
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, ui::Color::rgb(0x000000, 0.55f));
    struct Row
    {
        std::string label;
        std::string value;
        bool checked = false;
        bool muted = false;
    };
    // The filter panels' groups, parallel to rows: a header opens above a group's first row.
    struct Tag
    {
        std::string section;
        int accent = theme::accent_none;
        bool changed = false;
    };
    std::vector<Row> rows;
    std::vector<Tag> tags;
    const char *title = tr("Utilities");
    std::string note;
    const auto on_off = [](bool on) { return std::string(on ? tr("On") : tr("Off")); };
    if (panel_ == Panel::menu || panel_ == Panel::none)
    {
        rows.push_back({tr("Sort by"), sort_label(effective_sort(state_.sort, &pop_view_))});
        rows.push_back({tr("Filter by"), header_filter_});
        rows.push_back({tr("Reset filters"), {}, false, !state_.filtered() && query_.empty()});
    }
    else if (panel_ == Panel::sort)
    {
        title = tr("Sort by");
        for (const SortMode mode : sort_choices_)
            rows.push_back({sort_label(mode), {}, mode == effective_sort(state_.sort, &pop_view_)});
    }
    else if (panel_ == Panel::links)
    {
        title = tr("Filter links");
        for (const LinkFilterRow row : link_rows_)
        {
            const bool reset = row == LinkFilterRow::reset;
            rows.push_back({link_filter_label(row),
                            reset ? std::string()
                            : row == LinkFilterRow::sort
                                ? std::string(mirror_sort_label(page_.sort))
                                : link_filter_value(row, page_.filter, page_.facets, link_prefs_),
                            false, reset && !page_.filter.active() && page_.sort == MirrorSort::best});
        }
        note = tr("{shown} of {total} downloads shown",
                  {{"shown", std::to_string(page_.shown.mirrors.size())},
                   {"total", std::to_string(page_.facets.total)}});
        if (!page_.seed_note.empty())
            note += "  ·  " + page_.seed_note;
        if (page_.shown.hidden_too_new > 0)
            note += "  \xC2\xB7  " + trn(page_.shown.hidden_too_new, "{count} needs {fw} or newer",
                                         "{count} need {fw} or newer", {{"fw", page_.shown.lowest_needed}});
    }
    else
    {
        title = tr("Filter by");
        for (const GridFilter which : kGridFilters)
        {
            switch (which)
            {
            case GridFilter::source:
                rows.push_back({tr("Source"), state_.source.empty() ? std::string(tr("All sources")) : state_.source});
                break;
            case GridFilter::region:
                rows.push_back({tr("Region"), state_.region.empty()
                                                  ? std::string(tr("All regions"))
                                                  : state_.region + " (" + std::to_string(agg_.region_count(state_.region)) + ")"});
                break;
            case GridFilter::firmware:
                rows.push_back({tr("Runs on my firmware"),
                                state_.runs_on_my_fw && !link_prefs_.firmware.empty()
                                    ? on_off(true) + " (" + link_prefs_.firmware + ", " + std::to_string(agg_.runs_on) + ")"
                                    : on_off(state_.runs_on_my_fw),
                                false, link_prefs_.firmware.empty()});
                break;
            case GridFilter::no_browser:
                rows.push_back({tr("No browser needed"),
                                on_off(state_.no_browser) + " (" + std::to_string(agg_.no_browser) + ")"});
                break;
            case GridFilter::single_file:
                rows.push_back({tr("Single file only"),
                                on_off(state_.single_file_only) + " (" + std::to_string(agg_.single_file) + ")"});
                break;
            case GridFilter::installed:
            {
                const char *installed = tr("Any");
                if (state_.installed == InstalledFilter::installed)
                    installed = tr("Installed");
                else if (state_.installed == InstalledFilter::not_installed)
                    installed = tr("Not installed");
                rows.push_back({tr("Installation"), installed});
                break;
            }
            case GridFilter::favourites:
                rows.push_back({tr("Favourites only"), on_off(state_.favourites_only)});
                break;
            case GridFilter::tier:
            {
                const char *tier = tr("Any");
                if (state_.tier == TierFilter::supported)
                    tier = tr("Supported");
                else if (state_.tier == TierFilter::debrid)
                    tier = tr("Debrid");
                rows.push_back({tr("Link tier"), tier});
                break;
            }
            case GridFilter::has_links:
                rows.push_back({tr("Has links"), on_off(state_.has_links_only)});
                break;
            case GridFilter::reset:
                rows.push_back({tr("Reset filters"), {}, false, !state_.filtered() && query_.empty()});
                break;
            }
        }
        if (link_prefs_.firmware.empty())
            note = tr("Runs on my firmware needs your firmware: Settings > Wings > Downloads.");
        else if (!installed_index_.known)
            note = tr("Installed status needs Pegasus DL's game list, which has not loaded yet.");
        else if (!facts_ready_ && (state_.runs_on_my_fw || !state_.region.empty()))
            note = tr("Still reading the links: the list fills in as they are read.");
    }
    // The filter and sort panels: named groups, a summary strip (and the result count).
    const bool grouped = panel_ == Panel::filter || panel_ == Panel::links || panel_ == Panel::sort;
    std::string summary;
    if (panel_ == Panel::sort)
    {
        for (std::size_t i = 0; i < rows.size() && i < sort_choices_.size(); ++i)
        {
            const SortGroup group = sort_group(sort_choices_[i]);
            tags.push_back({sort_group_name(group), sort_group_accent(group), false});
        }
        summary = tr("Sorted by {value}", {{"value", std::string(sort_label(effective_sort(state_.sort, &pop_view_)))}}) +
                  "  ·  " +
                  result_count_text(static_cast<int>(tiles_.size()), static_cast<int>(games_.games.size()), false);
    }
    else if (panel_ == Panel::filter)
    {
        for (std::size_t i = 0; i < rows.size() && i < static_cast<std::size_t>(kGridFilterCount); ++i)
        {
            const FilterGroup group = grid_filter_group(kGridFilters[i]);
            tags.push_back({filter_group_name(group), filter_group_accent(group),
                            grid_row_changed(kGridFilters[i], state_)});
        }
        summary = filters_on_text(grid_filters_on(state_)) + "  ·  " +
                  result_count_text(static_cast<int>(tiles_.size()), static_cast<int>(games_.games.size()), false);
    }
    else if (panel_ == Panel::links)
    {
        for (std::size_t i = 0; i < rows.size() && i < link_rows_.size(); ++i)
        {
            const FilterGroup group = link_filter_group(link_rows_[i]);
            tags.push_back({filter_group_name(group), filter_group_accent(group),
                            link_row_changed(link_rows_[i], page_.filter, page_.sort)});
        }
        summary = filters_on_text(link_filters_on(page_.filter)) + "  ·  " +
                  result_count_text(static_cast<int>(page_.shown.mirrors.size()), page_.facets.total, true);
        note.clear(); // the strip above says "N of M"; the seed note and the too-new count stay
        if (!page_.seed_note.empty())
            note = page_.seed_note;
        if (page_.shown.hidden_too_new > 0)
            note += std::string(note.empty() ? "" : "  ·  ") +
                    trn(page_.shown.hidden_too_new, "{count} needs {fw} or newer", "{count} need {fw} or newer",
                        {{"fw", page_.shown.lowest_needed}});
    }
    const float strip = grouped ? kMenuSummary : 0.0f;
    tags.resize(rows.size());
    const float body = grouped ? menu_rows_height(tags) : static_cast<float>(rows.size()) * kPanelPitch;
    const float width = 760.0f;
    const float height = 110.0f + strip + body + (note.empty() ? 20.0f : 64.0f);
    const ui::Rect r{960.0f - width * 0.5f, 540.0f - height * 0.5f + (1.0f - a) * 16.0f, width, height};
    ui::modal(list, r);
    ui::text(list, type.light, title, r.x + 36.0f, r.y + 68.0f, 36.0f, ui::color::text);
    if (grouped)
    {
        ui::text_fit(list, type.medium, summary, r.x + 36.0f, r.y + 104.0f, 22.0f, r.w - 72.0f - 150.0f,
                     ui::color::text2);
        ui::text(list, type.regular, tr("Square: clear"), r.x + r.w - 36.0f, r.y + 104.0f, 19.0f,
                 ui::color::text3, ui::Align::right);
    }
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const float top = r.y + 96.0f + strip +
                          (grouped ? menu_row_top(tags, i) : static_cast<float>(i) * kPanelPitch);
        if (grouped && theme::section_starts(tags, i))
        {
            // A small non-focusable header: an accent dot and the group's name.
            list.circle(r.x + 42.0f, top - kMenuHeader * 0.5f + 2.0f, 5.0f, theme::accent_color(tags[i].accent));
            ui::text_fit(list, type.semibold, tags[i].section, r.x + 58.0f, top - 8.0f, 18.0f, r.w - 120.0f,
                         ui::color::text2);
        }
        const ui::Rect row{r.x + 24.0f, top, r.w - 48.0f, grouped ? kMenuRow : kPanelRow};
        const bool focus = static_cast<int>(i) == panel_row_;
        ui::focus_ring(list, row, 16.0f, focus ? 1.0f : 0.0f);
        ui::surface(list, row, 16.0f, ui::Color::rgb(0xffffff, focus ? 0.14f : 0.07f));
        const ui::Color ink = rows[i].muted ? ui::color::text3 : ui::color::text;
        ui::text_fit(list, type.medium, rows[i].label, row.x + 24.0f, row.cy() + 8.0f, 24.0f, 330.0f, ink);
        if (!rows[i].value.empty())
            ui::text_fit(list, type.regular, rows[i].value, row.x + row.w - 24.0f, row.cy() + 7.0f,
                         22.0f, 330.0f, focus ? ui::color::text : ui::color::text2, ui::Align::right);
        if (rows[i].checked)
            ui::icon_check(list, row.x + row.w - 36.0f, row.cy(), 24.0f, ui::color::text, 3.0f);
        if (tags[i].changed) // a filter that is on: a small accent bar at the row's left edge
            list.rounded_rect({row.x + 6.0f, row.y + 12.0f, 4.0f, row.h - 24.0f}, 2.0f,
                              theme::accent_color(tags[i].accent));
    }
    if (!note.empty())
        ui::text_fit(list, type.regular, note, r.x + 36.0f, r.y + r.h - 28.0f, 19.0f, r.w - 72.0f,
                     ui::color::text3);
    list.pop_opacity();
}

// One mirror card: the host and how many files on the first line, what the
// name says as chips on the second, the tier and Best at the right, a warning
// line for a part set with a problem.
void StoreScreen::draw_mirror(hui::gfx::DrawList &list, int mirror, const ui::Rect &r, float focus) const
{
    const ui::Type &type = ctx_.type;
    const MirrorCard c = mirror_card(page_, mirror, link_prefs_, started_);
    ui::focus_ring(list, r, 18.0f, focus);
    ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.075f + 0.065f * focus));
    list.rounded_rect({r.x + 8.0f, r.y + 16.0f, 5.0f, r.h - 32.0f}, 2.5f,
                      c.tier == LinkTier::unknown ? ui::Color::rgb(0xffffff, 0.25f) : tier_color(c.tier));
    // The right block: the tier, then Best / queued.
    constexpr float kChip = 17.0f;
    const float right = r.x + r.w - 24.0f;
    const float tier_w = type.semibold.measure(tier_label(c.tier), kChip) + 28.0f;
    // A dark backing under the tinted chip keeps its colour readable on the
    // light focused fill.
    if (focus > 0.01f)
        list.rounded_rect({right - tier_w, r.y + 34.0f - (kChip + 16.0f) * 0.5f, tier_w, kChip + 16.0f}, 8.0f,
                          ui::Color::rgb(0x000000, 0.6f * focus));
    draw_tier_chip(ctx_, list, right - tier_w, r.y + 34.0f, c.tier, kChip);
    float rx = right - tier_w - 10.0f;
    if (c.best)
    {
        const char *best = tr("Best");
        const float w = type.semibold.measure(best, kChip) + 28.0f;
        ui::chip(list, type, rx - w, r.y + 34.0f, best, ui::color::text, ui::color::night0, kChip);
        rx -= w + 10.0f;
    }
    if (c.started > 0)
    {
        const std::string queued = c.multipart && c.started < c.part_count
                                       ? tr("{n} of {total} queued", {{"n", std::to_string(c.started)},
                                                                      {"total", std::to_string(c.part_count)}})
                                       : std::string(tr("Queued"));
        ui::text(list, type.medium, queued, right, r.y + 76.0f, 19.0f, ui::color::text2, ui::Align::right);
    }
    // Line 1: host, wrapper, parts.
    float x = r.x + 30.0f;
    const float limit = rx - 16.0f;
    x += ui::text_fit(list, type.medium, c.host, x, r.y + 42.0f, 27.0f, 330.0f, ui::color::text) + 16.0f;
    if (c.wrapped && !c.via.empty())
        x += ui::text_fit(list, type.regular, tr("via {host}", {{"host", c.via}}), x, r.y + 42.0f, 18.0f, 300.0f,
                          hui::gfx::mix(ui::color::text3, ui::color::text, focus)) + 16.0f;
    if (x < limit)
        ui::text_fit(list, type.regular, c.parts, x, r.y + 42.0f, 21.0f, limit - x,
                     c.multipart ? ui::color::text : hui::gfx::mix(ui::color::text2, ui::color::text, focus));
    // Line 2: what the name says.
    const float cy = r.y + 74.0f;
    float cx = r.x + 30.0f;
    const float chips_end = c.started > 0 ? right - 190.0f : right;
    const ui::Color neutral = ui::Color::rgb(0xffffff, 0.08f);
    const auto chip = [&](const std::string &label, ui::Color fill, ui::Color ink, ui::Color edge)
    {
        if (label.empty())
            return;
        const float w = type.semibold.measure(label, kChip) + 28.0f;
        if (cx + w > chips_end)
            return;
        cx += ui::chip(list, type, cx, cy, label, fill, ink, kChip, edge) + 8.0f;
    };
    // The focused card's fill is light grey: lift the dim inks with it so the
    // chips keep their contrast.
    const ui::Color soft = hui::gfx::mix(ui::color::text2, ui::color::text, focus);
    const ui::Color faint = hui::gfx::mix(ui::color::text3, ui::color::text, focus);
    chip(c.region, neutral, soft, ui::Color{0, 0, 0, 0});
    const ui::Color fit = c.fit_known ? fit_color(c.fit) : soft;
    const ui::Color fit_fill = c.fit_known ? fit.with_alpha(0.15f) : neutral;
    const ui::Color fit_edge = c.fit_known ? fit.with_alpha(0.6f) : ui::Color{0, 0, 0, 0};
    chip(c.fw, fit_fill, fit, fit_edge);
    chip(c.fw_note, fit_fill, fit, fit_edge);
    chip(c.format, neutral, soft, ui::Color{0, 0, 0, 0});
    chip(c.version, neutral, soft, ui::Color{0, 0, 0, 0});
    chip(c.kind, neutral, soft, ui::Color{0, 0, 0, 0});
    chip(c.source, ui::Color{0, 0, 0, 0}, faint, ui::Color::rgb(0xffffff, 0.25f + 0.25f * focus));
    // Line 3: what is wrong with the set.
    if (!c.warning.empty())
    {
        // On a dark backing so the red reads on the light focused fill too.
        const float ww = std::min(type.medium.measure(c.warning, 20.0f), r.w - 60.0f);
        list.rounded_rect({r.x + 22.0f, r.y + 92.0f, ww + 16.0f, 26.0f}, 8.0f,
                          ui::Color::rgb(0x000000, 0.35f + 0.3f * focus));
        ui::text_fit(list, type.medium, c.warning, r.x + 30.0f, r.y + 111.0f, 20.0f, r.w - 60.0f,
                     ui::color::tier_red);
    }
}

void StoreScreen::draw_page_row(hui::gfx::DrawList &list, const PageRow &row, const ui::Rect &r, float focus) const
{
    const ui::Type &type = ctx_.type;
    const GamePage &page = page_.page;
    switch (row.kind)
    {
    case PageRowKind::best:
    {
        const MirrorCard c = mirror_card(page_, row.mirror, link_prefs_, started_);
        ui::focus_ring(list, r, 20.0f, focus);
        ui::surface(list, r, 20.0f, ui::Color::rgb(0xffffff, 0.13f + 0.04f * focus));
        // The primary action: the same white Best chip as on the cards, bigger.
        const float lw = ui::chip(list, type, r.x + 32.0f, r.y + 38.0f, tr("Best"), ui::color::text,
                                  ui::color::night0, 20.0f);
        if (focus > 0.01f) // as on the cards: a dark backing under the tinted chip
            list.rounded_rect({r.x + 32.0f + lw + 12.0f, r.y + 38.0f - 16.5f,
                               type.semibold.measure(tier_label(c.tier), 17.0f) + 28.0f, 33.0f},
                              8.0f, ui::Color::rgb(0x000000, 0.6f * focus));
        draw_tier_chip(ctx_, list, r.x + 32.0f + lw + 12.0f, r.y + 38.0f, c.tier, 17.0f);
        const float text_w = r.w - 64.0f - 380.0f;
        ui::text_fit(list, type.medium, best_summary(page_), r.x + 32.0f, r.y + 92.0f, 30.0f, text_w,
                     ui::color::text);
        const ui::Color soft = hui::gfx::mix(ui::color::text2, ui::color::text, focus);
        ui::text_fit(list, type.regular, page.best_why, r.x + 32.0f, r.y + 128.0f, 21.0f, text_w, soft);
        const float right = r.x + r.w - 32.0f;
        ui::text_fit(list, type.medium, c.host, right, r.y + 54.0f, 27.0f, 360.0f, ui::color::text,
                     ui::Align::right);
        ui::text_fit(list, type.regular, c.parts, right, r.y + 90.0f, 21.0f, 360.0f, soft, ui::Align::right);
        ui::text_fit(list, type.regular, c.source, right, r.y + 124.0f, 20.0f, 360.0f,
                     hui::gfx::mix(ui::color::text3, ui::color::text2, focus), ui::Align::right);
        break;
    }
    case PageRowKind::filters:
    {
        ui::focus_ring(list, r, 18.0f, focus);
        ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.06f + 0.07f * focus));
        const float lw = ui::text(list, type.semibold, tr("Filters"), r.x + 28.0f, r.cy() + 9.0f, 24.0f,
                                  ui::color::text);
        const std::string count = tr("{shown} of {total} downloads shown",
                                     {{"shown", std::to_string(page_.shown.mirrors.size())},
                                      {"total", std::to_string(page_.facets.total)}});
        const ui::Color faint = hui::gfx::mix(ui::color::text3, ui::color::text, focus);
        const float cw = ui::text(list, type.regular, count, r.x + r.w - 28.0f, r.cy() + 8.0f, 20.0f, faint,
                                  ui::Align::right);
        ui::text_fit(list, type.regular, link_filter_summary(page_.filter, link_prefs_), r.x + 28.0f + lw + 20.0f,
                     r.cy() + 8.0f, 22.0f, r.w - 56.0f - lw - 40.0f - cw,
                     page_.filter.active() ? ui::color::text : faint);
        break;
    }
    case PageRowKind::hidden:
        ui::text_fit(list, type.regular,
                     trn(page_.shown.hidden_too_new, "{count} download hidden: it needs firmware {fw} or newer",
                         "{count} downloads hidden: they need firmware {fw} or newer",
                         {{"fw", page_.shown.lowest_needed}}),
                     r.x + 8.0f, r.y + 28.0f, 20.0f, r.w - 16.0f, ui::color::text3);
        break;
    case PageRowKind::section:
    {
        int cards = 0;
        for (const PageRow &other : page_.rows)
            cards += other.kind == PageRowKind::mirror && other.section == row.section ? 1 : 0;
        const float lw = ui::text(list, type.semibold, section_label(static_cast<Section>(row.section)), r.x,
                                  r.y + 50.0f, 32.0f, ui::color::text);
        ui::text(list, type.regular, trn(cards, "{count} download", "{count} downloads"), r.x + lw + 18.0f,
                 r.y + 50.0f, 21.0f, ui::color::text3);
        list.rounded_rect({r.x, r.y + r.h - 8.0f, r.w, 1.5f}, 0.75f, ui::color::line);
        break;
    }
    case PageRowKind::release:
    {
        const Release &rel = page.releases[static_cast<std::size_t>(row.release)];
        ui::text_fit(list, type.medium, rel.header(page), r.x + kCardIndent, r.y + 34.0f, 22.0f,
                     r.w - kCardIndent, ui::color::text2);
        const std::string sub = rel.subheader();
        if (!sub.empty())
            ui::text_fit(list, type.regular, sub, r.x + kCardIndent, r.y + 64.0f, 19.0f, r.w - kCardIndent,
                         ui::color::text3);
        break;
    }
    case PageRowKind::mirror:
        draw_mirror(list, row.mirror, {r.x + kCardIndent, r.y, r.w - kCardIndent, r.h}, focus);
        break;
    case PageRowKind::empty:
        if (page.mirrors.empty())
        {
            ui::text(list, type.light, tr("No download links"), r.x, r.y + 52.0f, 36.0f, ui::color::text);
            ui::text_fit(list, type.regular, tr("The sources list this game without a link to download it."),
                         r.x, r.y + 96.0f, 23.0f, r.w, ui::color::text2);
        }
        else
        {
            ui::focus_ring(list, r, 18.0f, focus);
            ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.05f + 0.06f * focus));
            ui::text(list, type.light, tr("No downloads match these filters"), r.x + 32.0f, r.y + 56.0f, 34.0f,
                     ui::color::text);
            ui::text_fit(list, type.regular,
                         tr("Clear filters: press Cross to show every download again, or Options to change them."),
                         r.x + 32.0f, r.y + 98.0f, 23.0f, r.w - 64.0f, ui::color::text2);
        }
        break;
    }
}

void StoreScreen::draw_detail(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    // The header (hero band, big cover, title, cards, buttons) slides up and
    // fades as the focus goes down to the downloads; the compact page (small
    // cover, facts column) fades in under it. game_header.hpp.
    HeaderView head;
    head.page = &page_;
    head.favourite = page_favourite_;
    head.loading = !page_.loaded;
    if (!page_.loaded)
    {
        head.failed = !waiting_detail_ && !page_.error.empty();
        head.status = head.failed ? page_.error : std::string(tr("Loading the links\xE2\x80\xA6"));
    }
    const GamePage &page = page_.page;
    std::string meta_line;
    const PopRecord *pop = pop_index_ ? pop_index_->best(page_.title_ids) : nullptr;
    if (page_.loaded)
    {
        std::vector<std::string> meta;
        meta.push_back(trn(page.unique_links, "{count} link", "{count} links"));
        meta.push_back(trn(static_cast<long>(page.releases.size()), "{count} release", "{count} releases"));
        meta.push_back(trn(static_cast<long>(page_.sources.size()), "{count} source", "{count} sources"));
        for (const std::string &m : meta)
            meta_line += (meta_line.empty() ? "" : "  \xC2\xB7  ") + m;
        head.meta_line = meta_line; // the header shows the rating as its own row
        head.rating = header_rating(pop);
        head.facts = header_facts(page_, link_prefs_, started_);
        head.cards = build_header_cards(head.facts);
        head.hero_full = !header_.hero_url.empty();
        head.hero_low = !header_.hero_low_url.empty();
        if (head.hero_full || head.hero_low)
            head.hero_key = page_.art_key;
        if (pop != nullptr)
            meta_line += "  \xC2\xB7  " + popularity_line(pop);
        else if (const SteamReviewService *live = steam_reviews())
        {
            // Lower confidence than the index and never the sort score (steam_reviews.hpp).
            SteamSummary summary;
            if (live->lookup(page_.title_ids, &summary))
            {
                meta_line += "  \xC2\xB7  " + live_rating_line(summary);
                head.rating_live = live_rating_line(summary); // the hero's rating row
            }
        }
    }
    const HeaderLayout head_layout = game_header_layout(ctx_, head);
    draw_game_header(ctx_, list, head, head_layout, header_, focused);
    if (!page_.loaded)
        return;

    // The list's top and left move from under the header to the compact page's.
    const float slide = header_.slide.value;
    const float list_top = head_layout.list_top + (kPageListTop - head_layout.list_top) * slide;
    const float x = ui::kGutter + (kPageX - ui::kGutter) * slide;
    const float w = kPageRight - x;

    if (slide > 0.01f)
    {
        list.push_opacity(slide);
        const ui::Rect cover{ui::kGutter, kPageTop, kPageCover, kPageCover};
        draw_cover(ctx_, list, page_.art_key, page_.title, cover, ui::kArtRadius);
        const float tw = kPageRight - kPageX;
        ui::text_fit(list, type.light, page_.title, kPageX, 204.0f, 52.0f, page_favourite_ ? tw - 56.0f : tw,
                     ui::color::text);
        if (page_favourite_)
            ui::icon_heart_filled(list, kPageRight - 22.0f, 186.0f, 34.0f, ui::color::text);
        ui::text_fit(list, type.regular, meta_line, kPageX, 250.0f, 23.0f, tw, ui::color::text3);

        // The facts under the cover: regions, sources, the listed size, title ids.
        float y = kPageTop + kPageCover + 46.0f;
        const float cw = kPageCover;
        if (!page_.regions.empty())
        {
            ui::text(list, type.semibold, tr("Regions"), ui::kGutter, y, 19.0f, ui::color::text3);
            y += 30.0f;
            float cx = ui::kGutter;
            for (const std::string &region : page_.regions)
            {
                const float rw = type.semibold.measure(region, 17.0f) + 28.0f;
                if (cx + rw > ui::kGutter + cw)
                {
                    y += 40.0f;
                    cx = ui::kGutter;
                }
                cx += ui::chip(list, type, cx, y, region, ui::Color::rgb(0xffffff, 0.1f), ui::color::text, 17.0f) +
                      8.0f;
            }
            y += 50.0f;
        }
        ui::text(list, type.semibold,
                 trn(static_cast<long>(page_.sources.size()), "{count} source", "{count} sources"), ui::kGutter, y,
                 19.0f, ui::color::text3);
        y += 32.0f;
        for (std::size_t i = 0; i < page_.sources.size() && i < 4; ++i)
        {
            ui::text_fit(list, type.regular, page_.sources[i], ui::kGutter, y, 21.0f, cw, ui::color::text2);
            y += 30.0f;
        }
        const std::string size = listed_size_text(page_);
        if (!size.empty())
        {
            y += 14.0f;
            y = ui::paragraph(list, type.regular, size, ui::kGutter, y, 21.0f, cw, 30.0f, ui::color::text2, 2);
        }
        if (!page_.title_ids.empty())
        {
            y += 14.0f;
            std::string ids;
            for (const std::string &id : page_.title_ids)
                ids += (ids.empty() ? "" : ", ") + id;
            y = ui::paragraph(list, type.regular, ids, ui::kGutter, y, 18.0f, cw, 26.0f, ui::color::text3, 3);
        }
        // Where the popularity numbers come from: under the facts, clear of the
        // hint bar at the bottom of the screen (which would cover it).
        if (pop != nullptr)
            ui::paragraph(list, type.regular, attribution_text(pop_index_.get()), ui::kGutter,
                          std::max(y + 30.0f, 900.0f), 17.0f, cw, 24.0f, ui::color::text3, 3);
        list.pop_opacity();
    }

    // The list: Best, Filters, then sections of releases and their cards.
    list.push_clip({x - 28.0f, list_top - 14.0f, w + 56.0f, kPageListBottom - list_top + 14.0f});
    for (std::size_t i = 0; i < page_.rows.size(); ++i)
    {
        const PageRow &row = page_.rows[i];
        const float ry = list_top + row.y - detail_scroll_.value;
        // A row scrolled (almost) out at the top is skipped: no sliver above the list.
        if (ry + row.h < list_top || ry > kPageListBottom)
            continue;
        const float f = i < row_focus_.size() && focused ? row_focus_[i] : 0.0f;
        draw_page_row(list, row, {x, ry, w, row.h}, f);
    }
    list.pop_clip();
    // More below: a fade says so. Full width and down to the screen edge, into the colour the hero band
    // ends on, so it has no edges to show.
    if (page_.height - detail_scroll_.value > kPageListBottom - list_top + 8.0f)
        list.gradient_rect({0.0f, kPageListBottom - 90.0f, 1920.0f, 1080.0f - (kPageListBottom - 90.0f)}, 0.0f,
                           ui::Color::rgb(0x080808, 0.0f), ui::Color::rgb(0x080808, 0.92f));
}

void StoreScreen::draw(Frame &frame, bool focused) const
{
    if (view_ == View::detail)
        draw_detail(frame, focused);
    else
        draw_grid(frame, focused);
    hui::gfx::DrawList &overlay = frame.overlay;
    draw_panel(overlay);
    if (ctx_.store.link_action().busy)
    {
        if (batch_.active)
        {
            const std::string text =
                tr("Queueing part {n} of {total} from {host}…",
                   {{"n", std::to_string(std::min(batch_.done + 1, batch_.total))},
                    {"total", std::to_string(batch_.total)},
                    {"host", batch_.host}}) +
                "  (" + tr("Circle: stop after this part") + ")";
            draw_busy(ctx_, overlay, text.c_str());
        }
        else
            draw_busy(ctx_, overlay, tr("Asking Pegasus DL\xE2\x80\xA6"));
    }
    // The const draw path of the panels needs a mutable frame: it is one.
    Context &ctx = ctx_;
    prompt_.draw(ctx, frame);
    confirm_.draw(ctx, frame);
    list_.draw(ctx, frame);
}

std::vector<hui::ui::Hint> StoreScreen::hints() const
{
    if (panel_ == Panel::filter || panel_ == Panel::links)
        return {{hui::ui::Button::cross, tr("Change")},
                {hui::ui::Button::square, tr("Clear")},
                {hui::ui::Button::circle, tr("Back")}};
    if (panel_ != Panel::none)
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Back")}};
    // A dialog over the page or the grid: only its own buttons work.
    if (confirm_.is_open() || list_.is_open())
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Back")}};
    if (view_ == View::detail)
    {
        const char *cross = tr("Download");
        if (header_.on_header)
        {
            cross = header_.button == static_cast<int>(HeaderButton::install)     ? tr("Show downloads")
                    : header_.button == static_cast<int>(HeaderButton::favourite) ? tr("Favourite")
                                                                                  : tr("More");
        }
        else if (const PageRow *row = focused_page_row())
        {
            if (row->kind == PageRowKind::filters)
                cross = tr("Filters");
            else if (row->kind == PageRowKind::empty)
                cross = page_.filter.active() ? tr("Clear filters") : nullptr;
            else if (row->mirror >= 0 && row->mirror < static_cast<int>(page_.page.mirrors.size()) &&
                     page_.page.mirrors[static_cast<std::size_t>(row->mirror)].multipart())
                cross = tr("Choose parts");
        }
        std::vector<hui::ui::Hint> items;
        if (cross != nullptr)
            items.push_back({hui::ui::Button::cross, cross});
        items.push_back({hui::ui::Button::square, tr("Favourite")});
        if (!page_.page.mirrors.empty())
            items.push_back({hui::ui::Button::options, tr("Filters")});
        items.push_back({hui::ui::Button::circle, tr("Back")});
        items.push_back({hui::ui::Button::l1, tr("Switch Tabs"), hui::ui::Button::r1});
        return items;
    }
    // Nothing matches: Cross resets the search and the filters (as the screen says).
    const bool none = tiles_.empty() && ctx_.store.state().have_catalog && !ctx_.store.state().packages.empty();
    std::vector<hui::ui::Hint> items = {
        {hui::ui::Button::cross, on_button_ ? tr("Sort and filter") : none ? tr("Reset filters") : tr("Open")}};
    if (!on_button_ && !none)
        items.push_back({hui::ui::Button::square, tr("Favourite")});
    items.push_back({hui::ui::Button::triangle, tr("Search")});
    items.push_back({hui::ui::Button::options, tr("Utilities")});
    items.push_back({hui::ui::Button::l1, tr("Switch Tabs"), hui::ui::Button::r1});
    return items;
}

} // namespace orbit
