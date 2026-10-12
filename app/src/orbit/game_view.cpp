// Wings TV app - What the Store shows: one tile per game, and a game's page of links.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_view.hpp"

#include "orbit/grid_filters.hpp"
#include "orbit/i18n.hpp"
#include "orbit/popularity.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace orbit
{

namespace
{

constexpr const char *kDot = " \xC2\xB7 "; // " · "
constexpr const char *kKnownRegions[] = {"USA", "EUR", "JPN", "ASIA", "HK", "KOR"};

std::string lower(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

void add_unique(std::vector<std::string> *list, const std::string &value)
{
    if (!value.empty() && std::find(list->begin(), list->end(), value) == list->end())
        list->push_back(value);
}

std::string join(const std::vector<std::string> &parts, const char *separator)
{
    std::string out;
    for (const std::string &p : parts)
    {
        if (p.empty())
            continue;
        if (!out.empty())
            out += separator;
        out += p;
    }
    return out;
}

const Package *package_at(const std::vector<Package> &packages, int index)
{
    return index >= 0 && index < static_cast<int>(packages.size()) ? &packages[static_cast<std::size_t>(index)]
                                                                    : nullptr;
}

std::string started_key(const std::string &package_id, int link_index)
{
    return package_id + '\x1f' + std::to_string(link_index);
}

void layout_rows(GameView *view)
{
    namespace L = page_layout;
    view->rows.clear();
    float y = 0.0f;
    const auto add = [&](PageRowKind kind, float h, int section = -1, int release = -1, int mirror = -1)
    {
        PageRow row;
        row.kind = kind;
        row.section = section;
        row.release = release;
        row.mirror = mirror;
        row.y = y;
        row.h = h;
        view->rows.push_back(row);
        y += h + L::kGap;
    };
    const GamePage &page = view->page;
    if (page.mirrors.empty())
    {
        add(PageRowKind::empty, L::kEmpty);
        view->height = y;
        return;
    }
    if (page.best >= 0)
        add(PageRowKind::best, L::kBest, -1, -1, page.best);
    add(PageRowKind::filters, L::kFilters);
    if (view->shown.hidden_too_new > 0)
        add(PageRowKind::hidden, L::kHidden);
    if (view->shown.mirrors.empty())
        add(PageRowKind::empty, L::kEmpty);
    int section = -1;
    int release = -1;
    for (const int m : view->shown.mirrors)
    {
        const Mirror &mirror = page.mirrors[static_cast<std::size_t>(m)];
        const Release &rel = page.releases[static_cast<std::size_t>(mirror.release)];
        const int s = static_cast<int>(rel.section);
        if (s != section)
        {
            if (section >= 0)
                y += L::kGap; // a little air before the next section
            add(PageRowKind::section, L::kSection, s);
            section = s;
            release = -1;
        }
        if (mirror.release != release)
        {
            add(PageRowKind::release, rel.subheader().empty() ? L::kRelease : L::kReleaseSub, s, mirror.release);
            release = mirror.release;
        }
        add(PageRowKind::mirror, mirror.warning().empty() ? L::kMirror : L::kMirrorWarning, s, mirror.release, m);
    }
    view->height = y;
}

void count_facets(GameView *view, const LinkPrefs &prefs)
{
    view->facets = page_facets(view->page, prefs);
    for (const std::string &r : view->page.regions())
        add_unique(&view->regions, r);
    sort_regions(&view->regions);
}

std::string value_text(const std::string &value, const std::vector<FacetValue> &values)
{
    if (value.empty())
        return tr("Any");
    std::string shown = value == "?" ? std::string(tr("Not stated")) : value;
    for (const FacetValue &v : values)
        if (v.value == value)
            return shown + " (" + std::to_string(v.count) + ")";
    return shown;
}

// Left / right through "" (any) and the facet's values.
std::string cycle_value(const std::string &current, const std::vector<FacetValue> &values, int delta)
{
    std::vector<std::string> options = {""};
    for (const FacetValue &v : values)
        options.push_back(v.value);
    int at = 0;
    for (std::size_t i = 0; i < options.size(); ++i)
        if (options[i] == current)
            at = static_cast<int>(i);
    const int n = static_cast<int>(options.size());
    at = ((at + delta) % n + n) % n;
    return options[static_cast<std::size_t>(at)];
}

bool shows(const std::vector<FacetValue> &values, bool set)
{
    return values.size() >= 2 || set;
}

} // namespace

LinkPrefs link_prefs_of(const WingsLocalPrefs &prefs)
{
    LinkPrefs out;
    out.firmware = prefs.firmware;
    out.region = prefs.region;
    out.prefer_debrid = prefs.prefer_debrid;
    const std::string f = lower(prefs.format);
    if (f == "exfat")
        out.format = LinkFormat::exfat;
    else if (f == "fpkg")
        out.format = LinkFormat::fpkg;
    else if (f == "pkg")
        out.format = LinkFormat::pkg;
    else if (f == "dump")
        out.format = LinkFormat::dump;
    else if (f == "ffpfs")
        out.format = LinkFormat::ffpfs;
    return out;
}

// ---- the grid ----

PackageFacts package_facts(const PackageDetail &detail)
{
    PackageFacts facts;
    facts.known = true;
    std::map<std::string, bool> multi; // base release -> has parts
    for (const ParsedLink &link : parse_links(detail.links))
    {
        const LinkMeta &m = link.meta;
        add_unique(&facts.regions, m.region);
        // Only the game itself says whether it runs: DLC and updates do not.
        if (section_of(m.kind) != Section::base)
            continue;
        bool &parts = multi[release_key(m)];
        parts = parts || m.has_part();
        const int need = fw_need(m.min_fw());
        if (need < 0)
            facts.fw_unknown = true;
        else if (facts.lowest_fw < 0 || need < facts.lowest_fw)
            facts.lowest_fw = need;
    }
    for (const auto &entry : multi)
        facts.single_file = facts.single_file || !entry.second;
    return facts;
}

void sort_regions(std::vector<std::string> *regions)
{
    const auto rank = [](const std::string &r)
    {
        int i = 0;
        for (const char *k : kKnownRegions)
        {
            if (r == k)
                return i;
            ++i;
        }
        return 100;
    };
    std::stable_sort(regions->begin(), regions->end(),
                     [&](const std::string &a, const std::string &b)
                     { return rank(a) != rank(b) ? rank(a) < rank(b) : a < b; });
}

int game_installed(const GameGroup &game, const std::vector<Package> &packages, const InstalledIndex &installed)
{
    int status = -1;
    for (const int p : game.packages)
    {
        const Package *pkg = package_at(packages, p);
        if (pkg == nullptr)
            continue;
        const int s = installed.status(pkg->title_id);
        if (s == 1)
            return 1;
        if (s == 0)
            status = 0;
    }
    return status;
}

bool game_favourite(const CatalogueState &state, const GameGroup &game, const std::vector<Package> &packages)
{
    if (state.favourites.empty())
        return false;
    for (const int p : game.packages)
    {
        const Package *pkg = package_at(packages, p);
        if (pkg != nullptr && state.is_favourite(favourite_key(*pkg)))
            return true;
    }
    return false;
}

bool toggle_game_favourite(CatalogueState *state, const GameGroup &game, const std::vector<Package> &packages)
{
    if (game_favourite(*state, game, packages))
    {
        for (const int p : game.packages)
        {
            const Package *pkg = package_at(packages, p);
            if (pkg == nullptr)
                continue;
            const std::string key = favourite_key(*pkg);
            if (state->is_favourite(key))
                state->toggle_favourite(key);
        }
        return false;
    }
    for (const int p : game.packages)
    {
        const Package *pkg = package_at(packages, p);
        if (pkg != nullptr)
            return state->toggle_favourite(favourite_key(*pkg));
    }
    return false;
}

int art_package(const GameGroup &game, const std::vector<Package> &packages)
{
    for (const int p : game.packages)
    {
        const Package *pkg = package_at(packages, p);
        if (pkg != nullptr && !pkg->poster_url.empty())
            return p;
    }
    return game.packages.empty() ? 0 : game.packages.front();
}

std::vector<std::string> game_regions(const GameGroup &game, const std::vector<PackageFacts> &facts)
{
    std::vector<std::string> out = game.regions;
    for (const int p : game.packages)
    {
        if (p < 0 || p >= static_cast<int>(facts.size()))
            continue;
        for (const std::string &r : facts[static_cast<std::size_t>(p)].regions)
            add_unique(&out, r);
    }
    sort_regions(&out);
    return out;
}

int game_runs_on(const GameGroup &game, const std::vector<PackageFacts> &facts, const std::string &console_fw)
{
    const int console = fw_console(console_fw);
    if (console < 0)
        return -1;
    bool unread = false;
    for (const int p : game.packages)
    {
        if (p < 0 || p >= static_cast<int>(facts.size()) || !facts[static_cast<std::size_t>(p)].known)
        {
            unread = true;
            continue;
        }
        const PackageFacts &f = facts[static_cast<std::size_t>(p)];
        // A link that does not say stays, as on the game page.
        if (f.fw_unknown || (f.lowest_fw >= 0 && f.lowest_fw <= console))
            return 1;
    }
    return unread ? -1 : 0;
}

int game_single_file(const GameGroup &game, const std::vector<PackageFacts> &facts)
{
    bool unread = false;
    for (const int p : game.packages)
    {
        if (p < 0 || p >= static_cast<int>(facts.size()) || !facts[static_cast<std::size_t>(p)].known)
        {
            unread = true;
            continue;
        }
        if (facts[static_cast<std::size_t>(p)].single_file)
            return 1;
    }
    return unread ? -1 : 0;
}

bool game_matches(const GameGroup &game, const std::vector<Package> &packages, std::string_view query)
{
    if (query.empty())
        return true;
    for (const int p : game.packages)
    {
        const Package *pkg = package_at(packages, p);
        if (pkg != nullptr && package_matches(*pkg, query))
            return true;
    }
    return false;
}

std::vector<GameTile> select_games(const GridQuery &q)
{
    struct Entry
    {
        GameTile tile;
        std::string title; // lower case
        std::string source;
        int rank = 0; // tier_rank of the game's tier
        long pop = -1; // popularity_sort_key (-1: no data, sorts last)
        const std::string *id = nullptr;
    };
    const CatalogueState &state = q.state;
    const bool fw_set = !q.prefs.firmware.empty();
    const SortMode mode = effective_sort(state.sort, q.popularity);
    std::vector<Entry> kept;
    kept.reserve(q.index.games.size());
    for (std::size_t gi = 0; gi < q.index.games.size(); ++gi)
    {
        const GameGroup &g = q.index.games[gi];
        if (g.packages.empty())
            continue;
        if (!state.source.empty())
        {
            bool any = false;
            for (const int p : g.packages)
            {
                const Package *pkg = package_at(q.packages, p);
                any = any || (pkg != nullptr && pkg->source_name == state.source);
            }
            if (!any)
                continue;
        }
        if (!q.query.empty() && !game_matches(g, q.packages, q.query))
            continue;
        if (state.has_links_only && g.link_count <= 0)
            continue;
        const int status = game_installed(g, q.packages, q.installed);
        // Unknown (database not read, no title id) matches neither side of the filter.
        if (state.installed == InstalledFilter::installed && status != 1)
            continue;
        if (state.installed == InstalledFilter::not_installed && status != 0)
            continue;
        const bool favourite = game_favourite(state, g, q.packages);
        if (state.favourites_only && !favourite)
            continue;
        bool green = false;
        bool via = false;
        bool no_browser = false;
        const GameAgg *agg = q.aggregate != nullptr && q.aggregate->games.size() == q.index.games.size()
                                 ? &q.aggregate->games[gi]
                                 : nullptr;
        for (const int p : g.packages)
        {
            if (p < 0 || p >= static_cast<int>(q.packages.size()))
                continue;
            const std::uint8_t packed = tier_of(q.packages, q.tiers, static_cast<std::size_t>(p));
            green = green || tier_is_green(packed_tier(packed));
            via = via || packed_via_debrid(packed);
            no_browser = no_browser || no_browser_tier(packed_tier(packed)) || packed_via_debrid(packed);
        }
        if (state.tier == TierFilter::supported && !green)
            continue;
        if (state.tier == TierFilter::debrid && !via)
            continue;
        if (state.no_browser && !(agg != nullptr ? agg->no_browser : no_browser))
            continue;
        std::vector<std::string> regions = agg != nullptr ? agg->regions : game_regions(g, q.facts);
        if (!state.region.empty() && std::find(regions.begin(), regions.end(), state.region) == regions.end())
            continue;
        if (state.runs_on_my_fw && fw_set &&
            (agg != nullptr ? agg->runs_on : game_runs_on(g, q.facts, q.prefs.firmware)) == 0)
            continue;
        if (state.single_file_only && (agg != nullptr ? agg->single_file : game_single_file(g, q.facts)) == 0)
            continue;
        Entry e;
        e.tile.game = static_cast<int>(gi);
        e.tile.art_package = art_package(g, q.packages);
        e.tile.installed = static_cast<std::int8_t>(status);
        e.tile.favourite = favourite;
        e.tile.tier = game_tier(g, q.packages, q.tiers, q.prefs);
        e.tile.regions = std::move(regions);
        e.title = lower(g.title);
        e.source = g.sources.empty() ? std::string() : lower(g.sources.front());
        e.rank = tier_rank(packed_tier(e.tile.tier));
        e.id = &g.id;
        if (q.popularity != nullptr)
        {
            if (q.popularity->index != nullptr)
            {
                const PopRecord *rec = q.popularity->index->best(g.title_ids);
                e.tile.popularity = rec != nullptr ? rec->score : -1;
            }
            if (is_popularity_sort(mode))
                e.pop = popularity_sort_key(mode, q.popularity, g.title_ids);
        }
        kept.push_back(std::move(e));
    }
    std::sort(kept.begin(), kept.end(),
              [mode](const Entry &a, const Entry &b)
              {
                  const auto by_title = [&]
                  { return a.title != b.title ? a.title < b.title : *a.id < *b.id; };
                  switch (mode)
                  {
                  case SortMode::az:
                      return by_title();
                  case SortMode::za:
                      return a.title != b.title ? a.title > b.title : *a.id < *b.id;
                  case SortMode::installed_first:
                      // installed (1), then not installed (0), then unknown (-1)
                      return a.tile.installed != b.tile.installed ? a.tile.installed > b.tile.installed
                                                                  : by_title();
                  case SortMode::not_installed_first:
                  {
                      const auto rank = [](int s) { return s == 0 ? 0 : (s == 1 ? 1 : 2); };
                      return rank(a.tile.installed) != rank(b.tile.installed)
                                 ? rank(a.tile.installed) < rank(b.tile.installed)
                                 : by_title();
                  }
                  case SortMode::favourites_first:
                      return a.tile.favourite != b.tile.favourite ? a.tile.favourite : by_title();
                  case SortMode::tier_first:
                      return a.rank != b.rank ? a.rank < b.rank : by_title();
                  case SortMode::source:
                      return a.source != b.source ? a.source < b.source : by_title();
                  case SortMode::popular:
                  case SortMode::rated:
                  case SortMode::reviewed:
                  case SortMode::my_downloads:
                      // larger first; games without data (-1) last; ties by title
                      return a.pop != b.pop ? a.pop > b.pop : by_title();
                  }
                  return by_title();
              });
    std::vector<GameTile> out;
    out.reserve(kept.size());
    for (Entry &e : kept)
        out.push_back(std::move(e.tile));
    return out;
}

std::vector<std::string> catalogue_regions(const GameIndex &index, const std::vector<PackageFacts> &facts)
{
    std::vector<std::string> out;
    for (const GameGroup &g : index.games)
        for (const std::string &r : game_regions(g, facts))
            add_unique(&out, r);
    sort_regions(&out);
    return out;
}

// ---- started links ----

bool StartedLinks::has(const LinkRef &ref) const
{
    return keys.count(started_key(ref.package_id, ref.link_index)) != 0;
}

void StartedLinks::add(const LinkRef &ref)
{
    keys.insert(started_key(ref.package_id, ref.link_index));
}

// ---- the game page ----

const PackageLink *GameView::link(const LinkRef &ref) const
{
    for (const PackageDetail &d : details)
    {
        if (d.package.id != ref.package_id)
            continue;
        for (const PackageLink &l : d.links)
            if (l.index == ref.link_index)
                return &l;
    }
    return nullptr;
}

LinkRef GameView::first_ref(int mirror) const
{
    if (mirror < 0 || mirror >= static_cast<int>(page.mirrors.size()))
        return {};
    const GameLink &l = page.links[static_cast<std::size_t>(page.mirrors[static_cast<std::size_t>(mirror)].links.front())];
    return LinkRef{page.packages[static_cast<std::size_t>(l.package)].id, l.index};
}

int GameView::section_of_mirror(int mirror) const
{
    if (mirror < 0 || mirror >= static_cast<int>(page.mirrors.size()))
        return -1;
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    return static_cast<int>(page.releases[static_cast<std::size_t>(m.release)].section);
}

void load_game_view(GameView *view, std::vector<PackageDetail> details, const std::vector<std::string> &source_order,
                    const LinkPrefs &prefs)
{
    view->details = std::move(details);
    view->loaded = true;
    view->error.clear();
    view->sources.clear();
    view->title_ids.clear();
    view->listed_size = 0.0;
    view->listed_size_source.clear();
    for (const PackageDetail &d : view->details)
    {
        add_unique(&view->sources, d.package.source_name);
        add_unique(&view->title_ids, d.package.title_id);
        for (const std::string &r : description_regions(d.package.description))
            add_unique(&view->regions, r);
        if (d.package.size_bytes > view->listed_size)
        {
            view->listed_size = d.package.size_bytes;
            view->listed_size_source = d.package.source_name;
        }
    }
    view->page = build_game_page(view->details, source_order);
    rank_page(&view->page, prefs);
    count_facets(view, prefs);
    // Research 4.2: firmware, region and format from the preferences, never hiding every download.
    const FilterSeed seed = seed_filter(view->page, view->facets, prefs);
    view->seed_note = seed.note;
    set_game_filter(view, seed.filter, prefs);
}

void rerank_game_view(GameView *view, const std::vector<std::string> &source_order, const LinkPrefs &prefs)
{
    if (!view->loaded)
        return;
    view->page = build_game_page(view->details, source_order);
    rank_page(&view->page, prefs);
    count_facets(view, prefs);
    MirrorFilter filter = view->filter;
    if (prefs.firmware.empty())
        filter.runs_on_my_fw = false;
    set_game_filter(view, filter, prefs);
}

void set_game_filter(GameView *view, const MirrorFilter &filter, const LinkPrefs &prefs)
{
    view->filter = filter;
    view->shown = filter_mirrors(view->page, filter, prefs);
    sort_mirrors(view->page, &view->shown.mirrors, view->sort);
    layout_rows(view);
}

std::string listed_size_text(const GameView &view)
{
    if (view.listed_size <= 0.0)
        return {};
    std::string text = tr("Listed size {size}", {{"size", format_bytes(view.listed_size)}});
    // Research 5: a listing this large covers several builds, not one download.
    if (view.listed_size >= 150.0 * 1024.0 * 1024.0 * 1024.0)
        text += " " + std::string(tr("(all builds)"));
    if (!view.listed_size_source.empty() && view.sources.size() > 1)
        text += " (" + view.listed_size_source + ")";
    return text;
}

MirrorCard mirror_card(const GameView &view, int mirror, const LinkPrefs &prefs, const StartedLinks &started)
{
    MirrorCard c;
    const GamePage &page = view.page;
    if (mirror < 0 || mirror >= static_cast<int>(page.mirrors.size()))
        return c;
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    const GameLink &first = page.links[static_cast<std::size_t>(m.links.front())];
    const LinkMeta &meta = first.meta;
    c.host = m.host.empty() ? std::string(tr("Unknown host")) : m.host;
    c.wrapped = meta.wrapped;
    if (c.wrapped)
    {
        const PackageLink *link = view.link(view.first_ref(mirror));
        if (link != nullptr)
            c.via = link->host;
    }
    c.region = meta.region;
    if (!meta.lang_to.empty() && !c.region.empty())
        c.region += " > " + meta.lang_to;
    if (!meta.fw.empty())
        c.fw = meta.fw + (meta.fw_plus ? "+" : "");
    if (meta.backport)
        c.fw += (c.fw.empty() ? "" : " ") + std::string(tr("BackPort"));
    if (!meta.backport_target.empty())
        c.fw_note = tr("runs on {fw}+", {{"fw", meta.backport_target}});
    if (!prefs.firmware.empty())
    {
        c.fit_known = true;
        c.fit = fw_fit(meta.min_fw(), prefs.firmware);
        if (c.fit == FwFit::unknown && c.fw_note.empty())
            c.fw_note = tr("fw ?");
        else if (c.fit == FwFit::too_new)
            c.fw_note = tr("needs {fw}", {{"fw", meta.min_fw()}});
    }
    if (meta.format != LinkFormat::none)
        c.format = format_label(meta.format);
    if (!meta.version.empty())
        c.version = "v" + meta.version;
    c.source = page.packages[static_cast<std::size_t>(first.package)].source_name;
    if (meta.kind == LinkKind::game_dlc)
        c.kind = meta.all_dlc ? tr("Game + all DLC") : kind_label(meta.kind);
    c.parts = m.parts_text();
    c.warning = m.warning();
    c.name = first.name;
    c.tier = m.tier;
    c.needs_browser = m.needs_browser;
    c.multipart = m.multipart();
    c.part_count = static_cast<int>(m.links.size());
    for (const int l : m.links)
    {
        const GameLink &gl = page.links[static_cast<std::size_t>(l)];
        if (started.has(LinkRef{page.packages[static_cast<std::size_t>(gl.package)].id, gl.index}))
            ++c.started;
    }
    c.best = page.best == mirror;
    return c;
}

std::string best_summary(const GameView &view)
{
    return join(view.page.best_chips, kDot);
}

PartsPlan plan_parts(const GameView &view, int mirror, const StartedLinks &started)
{
    PartsPlan plan;
    const GamePage &page = view.page;
    if (mirror < 0 || mirror >= static_cast<int>(page.mirrors.size()))
        return plan;
    const Mirror &m = page.mirrors[static_cast<std::size_t>(mirror)];
    plan.total = static_cast<int>(m.links.size());
    plan.host = m.host;
    plan.refs = page.queue_refs(mirror, [&](const LinkRef &ref) { return started.has(ref); });
    plan.skipped = plan.total - static_cast<int>(plan.refs.size());
    plan.one_at_a_time = m.needs_browser;
    const long n = static_cast<long>(plan.refs.size());
    if (n == 0)
    {
        plan.action = tr("Every part is already queued");
        return plan;
    }
    if (plan.one_at_a_time)
    {
        // Each part opens the PS5 browser for a check: never several at once.
        int number = plan.total - static_cast<int>(n) + 1;
        for (std::size_t i = 0; i < m.links.size(); ++i)
        {
            const GameLink &gl = page.links[static_cast<std::size_t>(m.links[i])];
            if (gl.index == plan.refs.front().link_index &&
                page.packages[static_cast<std::size_t>(gl.package)].id == plan.refs.front().package_id)
            {
                number = i < m.numbers.size() ? m.numbers[i] : static_cast<int>(i) + 1;
                break;
            }
        }
        plan.action = tr("Download the next part (part {n})", {{"n", std::to_string(number)}});
        plan.refs.resize(1);
        plan.title = tr("Download part {n} from {host}?", {{"n", std::to_string(number)}, {"host", plan.host}});
        plan.message = tr("{host} needs a check in the PS5 browser, so each part needs its own capture: Wings "
                          "opens one part at a time.",
                          {{"host", plan.host}});
        if (!m.warning().empty())
            plan.message += " " + tr("Careful: {warning}.", {{"warning", m.warning()}});
        return plan;
    }
    plan.action = m.status == PartStatus::complete || m.status == PartStatus::unnumbered
                      ? trn(plan.total, "Download all {count} part", "Download all {count} parts")
                      : trn(plan.total, "Download all {count} link", "Download all {count} links");
    plan.title = trn(n, "Download {count} part from {host}?", "Download {count} parts from {host}?",
                     {{"host", plan.host}});
    std::string message = trn(n,
                              "Wings asks Pegasus DL to queue {count} part from {host}, in catalogue order, "
                              "as if you chose it yourself.",
                              "Wings asks Pegasus DL to queue {count} parts from {host}, one after another "
                              "in catalogue order, as if you chose each one yourself.",
                              {{"host", plan.host}});
    if (plan.skipped > 0)
        message += " " + trn(plan.skipped, "{count} part this app already queued is left out.",
                             "{count} parts this app already queued are left out.");
    if (!m.warning().empty())
        message += " " + tr("Careful: {warning}.", {{"warning", m.warning()}});
    message += " " + std::string(tr("It is a multi-part archive: you need every part to extract it."));
    plan.message = std::move(message);
    return plan;
}

// ---- the page's filter panel ----

std::vector<LinkFilterRow> link_filter_rows(const Facets &f, const MirrorFilter &filter, const LinkPrefs &prefs)
{
    std::vector<LinkFilterRow> rows;
    if ((!prefs.firmware.empty() && f.show_firmware()) || filter.runs_on_my_fw)
        rows.push_back(LinkFilterRow::runs_on_my_fw);
    // Grouped for the panel (filter_menu.hpp): region & firmware, show, format & host, source, sort.
    if (f.show_regions() || !filter.region.empty())
        rows.push_back(LinkFilterRow::region);
    if (f.show_no_browser() || filter.no_browser)
        rows.push_back(LinkFilterRow::no_browser);
    if (f.show_single_file() || filter.single_file)
        rows.push_back(LinkFilterRow::single_file);
    if (shows(f.sections, filter.section >= 0))
        rows.push_back(LinkFilterRow::kind);
    if (f.show_formats() || !filter.format.empty())
        rows.push_back(LinkFilterRow::format);
    if (shows(f.hosts, !filter.host.empty()))
        rows.push_back(LinkFilterRow::host);
    if (shows(f.sources, !filter.source.empty()))
        rows.push_back(LinkFilterRow::source);
    if (f.total > 1)
        rows.push_back(LinkFilterRow::sort);
    rows.push_back(LinkFilterRow::reset);
    return rows;
}

const char *link_filter_label(LinkFilterRow row)
{
    switch (row)
    {
    case LinkFilterRow::runs_on_my_fw:
        return tr("Runs on my firmware");
    case LinkFilterRow::no_browser:
        return tr("No browser needed");
    case LinkFilterRow::region:
        return tr("Region");
    case LinkFilterRow::single_file:
        return tr("Single file only");
    case LinkFilterRow::format:
        return tr("Format");
    case LinkFilterRow::kind:
        return tr("Kind");
    case LinkFilterRow::host:
        return tr("Host");
    case LinkFilterRow::source:
        return tr("Source");
    case LinkFilterRow::sort:
        return tr("Sort cards by");
    case LinkFilterRow::reset:
        break;
    }
    return tr("Clear filters");
}

std::string link_filter_value(LinkFilterRow row, const MirrorFilter &filter, const Facets &f, const LinkPrefs &prefs)
{
    const auto on_off = [](bool on) { return std::string(on ? tr("On") : tr("Off")); };
    switch (row)
    {
    case LinkFilterRow::runs_on_my_fw:
        if (filter.runs_on_my_fw && !prefs.firmware.empty())
            return on_off(true) + " (" + prefs.firmware + ")";
        return on_off(filter.runs_on_my_fw);
    case LinkFilterRow::no_browser:
        return on_off(filter.no_browser);
    case LinkFilterRow::region:
        return value_text(filter.region, f.regions);
    case LinkFilterRow::single_file:
        return on_off(filter.single_file);
    case LinkFilterRow::format:
        return value_text(filter.format, f.formats);
    case LinkFilterRow::kind:
    {
        if (filter.section < 0)
            return tr("Any");
        const std::string key = std::to_string(filter.section);
        std::string text = section_label(static_cast<Section>(filter.section));
        for (const FacetValue &v : f.sections)
            if (v.value == key)
                text += " (" + std::to_string(v.count) + ")";
        return text;
    }
    case LinkFilterRow::host:
        return value_text(filter.host, f.hosts);
    case LinkFilterRow::source:
        return value_text(filter.source, f.sources);
    case LinkFilterRow::sort: // the screen fills it from GameView::sort
    case LinkFilterRow::reset:
        break;
    }
    return {};
}

void cycle_link_filter(MirrorFilter *filter, LinkFilterRow row, int delta, const Facets &f)
{
    switch (row)
    {
    case LinkFilterRow::runs_on_my_fw:
        filter->runs_on_my_fw = !filter->runs_on_my_fw;
        break;
    case LinkFilterRow::no_browser:
        filter->no_browser = !filter->no_browser;
        break;
    case LinkFilterRow::region:
        filter->region = cycle_value(filter->region, f.regions, delta);
        break;
    case LinkFilterRow::single_file:
        filter->single_file = !filter->single_file;
        break;
    case LinkFilterRow::format:
        filter->format = cycle_value(filter->format, f.formats, delta);
        break;
    case LinkFilterRow::kind:
    {
        const std::string current = filter->section < 0 ? std::string() : std::to_string(filter->section);
        const std::string next = cycle_value(current, f.sections, delta);
        filter->section = next.empty() ? -1 : std::atoi(next.c_str());
        break;
    }
    case LinkFilterRow::host:
        filter->host = cycle_value(filter->host, f.hosts, delta);
        break;
    case LinkFilterRow::source:
        filter->source = cycle_value(filter->source, f.sources, delta);
        break;
    case LinkFilterRow::sort: // GameView::sort: the screen handles it
        break;
    case LinkFilterRow::reset:
        *filter = MirrorFilter{};
        break;
    }
}

std::string link_filter_summary(const MirrorFilter &filter, const LinkPrefs &prefs)
{
    std::vector<std::string> parts;
    if (filter.runs_on_my_fw)
        parts.push_back(prefs.firmware.empty() ? std::string(tr("Runs on my firmware"))
                                               : tr("Runs on {fw}", {{"fw", prefs.firmware}}));
    if (filter.no_browser)
        parts.emplace_back(tr("No browser needed"));
    if (!filter.region.empty())
        parts.push_back(filter.region == "?" ? std::string(tr("Region not stated")) : filter.region);
    if (filter.single_file)
        parts.emplace_back(tr("Single file only"));
    if (!filter.format.empty())
        parts.push_back(filter.format == "?" ? std::string(tr("Format not stated")) : filter.format);
    if (filter.section >= 0)
        parts.emplace_back(section_label(static_cast<Section>(filter.section)));
    if (!filter.host.empty())
        parts.push_back(filter.host);
    if (!filter.source.empty())
        parts.push_back(filter.source);
    if (parts.empty())
        return tr("None");
    return join(parts, ", ");
}

} // namespace orbit
