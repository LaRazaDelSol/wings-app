// Wings TV app - The shell's screens and what they share.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Orbit Store's screens.hpp (GPL-3.0-or-later). Screens are
// immediate mode, like the kit: update() owns state and animation, draw() is
// const and only records shapes. A screen never talks to the network; it reads
// the Store's snapshot, asks the ArtCache for images, sends commands to the
// Store and returns an Intent when the app should navigate.

#pragma once

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "orbit/art.hpp"
#include "orbit/catalogue_view.hpp"
#include "orbit/cover_art.hpp"
#include "orbit/destination_browser.hpp"
#include "orbit/download_all.hpp"
#include "orbit/files_logic.hpp"
#include "orbit/game_header.hpp"
#include "orbit/game_view.hpp"
#include "orbit/grid_filters.hpp"
#include "orbit/i18n.hpp"
#include "orbit/panels.hpp"
#include "orbit/pegasus_offer.hpp"
#include "orbit/prefs.hpp"
#include "orbit/shadowmount_scan.hpp"
#include "orbit/store.hpp"
#include "orbit/popularity.hpp"
#include "orbit/theme.hpp"
#include "orbit/ui.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/feedback.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orbit
{

using hui::Action;
using hui::Direction;
using hui::InputFrame;
using Feedback = hui::ui::Feedback;

// One frame, back to front: backdrop, scene, [glass capture], overlay.
struct Frame
{
    hui::gfx::BackdropSpec backdrop;
    hui::gfx::DrawList scene;
    hui::gfx::DrawList overlay;
    bool glass = false;
    std::uint32_t glass_texture = 0;

    void reset()
    {
        backdrop = {};
        scene.clear();
        overlay.clear();
        glass = false;
    }
};

// Long-lived services every screen receives.
struct Context
{
    Context(const ui::Type &type_, const hui::ui::Fonts &fonts_, Store &store_, ArtCache &art_)
        : type(type_), fonts(fonts_), store(store_), art(art_)
    {
    }

    const ui::Type &type;
    const hui::ui::Fonts &fonts; // the kit's slots, for glyphs and the keyboard
    Store &store;
    ArtCache &art;
    std::uint32_t mark = 0;        // the Wings mark, 128 px
    std::uint32_t loader_mark = 0; // the mark, dimmed outer strokes, 256 px
    std::string app_version;       // this app's contentVersion
    // What the player reads for the version ("0.1.1", plus the build for a test build); app_version when empty.
    std::string app_label;
    // Opens Pegasus DL's web UI in the console browser; empty where impossible.
    std::function<bool()> open_pegasus_ui;
    // Settings > Cover art. Null where the HTTPS art module is not built in
    // (the preview and the unit tests); the section then says so.
    CoverArtControl *cover_art = nullptr;
    // The poster resolver; the App tells it the cover icon source (Settings > Art sources). Null where there is none.
    PosterIndex *posters = nullptr;
    // The grid tells the art service which covers are just off screen (keys
    // like "pkg:123"). Empty where there is no art service.
    std::function<void(const std::vector<std::string> &)> prefetch_art;
    // Where the Store grid keeps its sort, filters and favourites (the checked
    // save container, first writable wins). Empty: not persisted (preview, tests).
    std::vector<std::string> catalogue_paths;
    // Where Wings keeps its own link-ranking and popularity preferences (never
    // sent to Pegasus DL). Empty: not persisted (preview, tests).
    std::vector<std::string> wings_prefs_paths;
    // Settings > Wings > Store grid: back to A to Z with no filters (the App sets it).
    std::function<void()> reset_store_grid;
    float clock = 0.0f; // seconds since the app opened
    // The first-run "load Pegasus DL" block is drawn under the waiting card (the App sets it every
    // frame): the card then drops its "Need it? Settings > ..." line and the raw link error.
    bool offer_showing = false;
    bool offer_grace = false; // ... and still inside the 10 s grace: spinner + "Looking for" line
    // The latest report written by the diagnostics writer (console build); the
    // Logs tab shows it. Empty in the preview and the tests: the screen then
    // builds a short one from what it knows.
    std::string diagnostics_text;
    // The title log (console build: sys::log with a tag). Empty elsewhere.
    std::function<void(const std::string &)> log_line;
    // ShadowMount's local API (after-download scan): empty pieces leave the feature dormant.
    ShadowMountEnv shadowmount;
    // The App's after-download scan (it sets this); the Downloads tab draws its banner.
    AfterInstall *after_install = nullptr;
    // Ends the title cleanly (the console build: the system is asked to close it). Empty
    // where Wings cannot close itself (preview, tests).
    std::function<void()> close_app;
};

// The top-level tabs, left to right.
namespace tab
{
constexpr int store = 0;
constexpr int downloads = 1;
constexpr int library = 2;
constexpr int files = 3;
constexpr int sources = 4;
constexpr int settings = 5;
constexpr int logs = 6;
constexpr int count = 7;
} // namespace tab

struct Intent
{
    enum class Kind : std::uint8_t
    {
        none,
        top_bar,  // the focus left the screen upward
        show_tab, // tab: one of tab::
    } kind = Kind::none;
    int tab = 0;
    std::string toast; // a short confirmation the app shows
};

class Screen
{
  public:
    explicit Screen(Context &context) : ctx_(context)
    {
    }
    virtual ~Screen() = default;
    // The focus arrives on the screen (from the top bar, or after a dialog).
    virtual void focus()
    {
    }
    // focused is false while the top bar has the focus: animate only.
    virtual Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) = 0;
    virtual void draw(Frame &frame, bool focused) const = 0;
    virtual std::vector<hui::ui::Hint> hints() const
    {
        return {};
    }
    // True while a dialog or keyboard of the screen takes all input.
    virtual bool modal() const
    {
        return false;
    }
    // The theme::Accent of the section the focus is in (-1: none). The App owns the wash and
    // eases it to this; a screen never picks a hue.
    virtual int section_accent() const
    {
        return -1;
    }

  protected:
    Context &ctx_;
};

// A dropdown's controller state: Cross opens it, up and down move, Cross
// chooses and Circle closes it unchanged.
struct Picker
{
    bool open = false;
    int active = 0;
    ui::Ease in;

    void show(int selected)
    {
        open = true;
        active = selected < 0 ? 0 : selected;
    }
    // While open: the chosen index once it is chosen, else -1.
    int update(const InputFrame &input, int count, Feedback &feedback);
    void animate(float dt);
};

// ---- shared drawing (views.cpp) ----

// A cover, fading in when its art arrives; a placeholder with the title before.
void draw_cover(Context &ctx, hui::gfx::DrawList &list, const std::string &art_key,
                const std::string &title, const ui::Rect &r, float radius, float opacity = 1.0f);
// A cover tile: the ring carries focus.
void draw_tile(Context &ctx, hui::gfx::DrawList &list, const std::string &art_key,
               const std::string &title, const ui::Rect &r, float focus, float opacity = 1.0f);
// Refusal at an edge: quiet, and silent on a held direction.
void refuse(Feedback &feedback, const InputFrame &input);
// The kit theme the on-screen keyboard (and later kit components) are drawn in.
hui::ui::Theme keyboard_theme();
// The "waiting for Pegasus" card shown instead of content while it is offline.
void draw_waiting(Context &ctx, hui::gfx::DrawList &list, float top);
// A screen's big title with a right-aligned summary on the same line.
void draw_heading(Context &ctx, hui::gfx::DrawList &list, const char *title,
                  const std::string &summary);
// A centred "working" card while a request is in flight.
void draw_busy(Context &ctx, hui::gfx::DrawList &list, const char *text);
// The words of a link route as a short chip label.
const char *route_chip(LinkRoute route);
// Link support tiers: green / yellow / neutral / red, always with a word.
ui::Color tier_color(LinkTier tier);
// A tier chip (tinted fill, coloured text and edge); returns its width.
float draw_tier_chip(const Context &ctx, hui::gfx::DrawList &list, float x, float cy,
                     LinkTier tier, float size = 17.0f);
// "Supported / Debrid / Unknown / Not supported" with their colours, right-aligned
// at right_x on the given baseline. Returns the left edge it reached.
float draw_tier_legend(const Context &ctx, hui::gfx::DrawList &list, float right_x,
                       float baseline);

// Shared geometry of the content area under the heading.
namespace layout
{
constexpr float kTitleY = 190.0f;
constexpr float kFilterY = 222.0f;
constexpr float kFilterHeight = 56.0f;
constexpr float kListTop = 300.0f;
constexpr float kGridTop = 322.0f;
constexpr float kListBottom = 940.0f;
} // namespace layout

// ---- the screens ----

// A page of rows with one focus: the base of the simple list screens.
class ListScreen : public Screen
{
  public:
    struct Row
    {
        std::string title;
        std::string detail;
        std::string right;   // right-aligned status text
        std::string badge;   // small label after the title
        std::string art_key; // optional square thumbnail
        float progress = -1.0f; // 0..1 draws a bar; negative draws none
        bool disabled = false;  // drawn dimmed: a placeholder that does nothing yet
        bool info = false;      // shown for information only: outlined, not filled like a control
        std::vector<std::string> segments; // non-empty: a Left/Right switch drawn as pills
        int segment = -1;                  // the lit pill in segments
        std::string section;               // non-empty: a header of this name opens above the row
        int accent = -1;                   // theme::Accent of the row's section (-1 none)
    };
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    int index() const
    {
        return index_;
    }
    // The accent of the focused row's section (theme::Accent; -1 none). Tests.
    int focus_accent() const
    {
        return focus_accent_;
    }
    int section_accent() const override
    {
        return focus_accent_;
    }

  protected:
    explicit ListScreen(Context &context) : Screen(context)
    {
    }
    virtual const char *title() const = 0;
    virtual std::string subtitle() const
    {
        return {};
    }
    virtual std::vector<Row> rows() const = 0;
    virtual const char *empty_text() const
    {
        return tr("Nothing here yet.");
    }
    // Pegasus must be answering for the rows to mean anything.
    virtual bool needs_pegasus() const
    {
        return true;
    }
    virtual void on_confirm(int row, Feedback &feedback, Intent &intent)
    {
        (void)row;
        (void)feedback;
        (void)intent;
    }
    virtual void on_focus()
    {
    }
    virtual void on_frame(bool focused)
    {
        (void)focused;
    }
    virtual const char *confirm_label() const
    {
        return nullptr;
    }

    int index_ = 0;
    int focus_accent_ = -1;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
};

// The catalogue: a grid of games (one tile per game, game_view.hpp) with search,
// sort and filters, and a game's page with its links folded into sections,
// releases and mirrors, the Best pick, link filters, "Download all parts" and
// the whole "download this link" decision flow.
class StoreScreen final : public Screen
{
  public:
    explicit StoreScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override;
    int section_accent() const override; // the open filter / sort menu's focused group, else -1

    // Tests and previews.
    bool detail_open() const
    {
        return view_ == View::detail;
    }
    const std::string &query() const
    {
        return query_;
    }
    int shown_count() const
    {
        return static_cast<int>(tiles_.size());
    }
    // Tests: type into the open text prompt.
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    void open_package(const std::string &package_id);
    void set_query(const std::string &query);
    // Tests: how the search box is wired.
    const TextPrompt &search_prompt() const
    {
        return prompt_;
    }
    TextPrompt &search_prompt()
    {
        return prompt_;
    }
    void set_source_filter(const std::string &name);
    // Sort, filters and favourites as the player has them (persisted).
    const CatalogueState &catalogue_state() const
    {
        return state_;
    }
    void set_catalogue_state(const CatalogueState &state);
    // The id of the package each tile stands for (its cover), in order (tests).
    std::vector<std::string> shown_ids() const;
    // The tiles and the games behind them (tests).
    const std::vector<GameTile> &tiles() const
    {
        return tiles_;
    }
    const GameIndex &games() const
    {
        return games_;
    }
    // Every package's link facts have been read (the grid's firmware and region filters).
    bool facts_ready() const
    {
        return facts_ready_;
    }
    // Where the link preferences come from (the App passes Settings' own);
    // set_prefs() keeps a copy instead (tests, previews).
    void use_prefs(const WingsLocalPrefs *prefs)
    {
        prefs_source_ = prefs;
    }
    void set_prefs(const WingsLocalPrefs &prefs)
    {
        own_prefs_ = prefs;
        prefs_source_ = &own_prefs_;
    }
    // The open game page (tests, previews).
    const GameView &game_page() const
    {
        return page_;
    }
    // The page header (tests, previews): the focus is on its buttons / which one.
    bool header_focused() const
    {
        return header_.on_header;
    }
    int header_button() const
    {
        return header_.button;
    }
    // The focused row of the page, or null.
    const PageRow *focused_page_row() const;
    // Puts the page's focus on a mirror's card (tests, previews).
    bool focus_page_mirror(int mirror);
    void open_link_filters();
    // Puts a filter on the open page (tests, previews).
    void set_link_filter(const MirrorFilter &filter);
    bool link_filters_open() const
    {
        return panel_ == Panel::links;
    }
    const StartedLinks &started_links() const
    {
        return started_;
    }
    bool batch_running() const
    {
        return batch_.active;
    }
    const ListDialog &list_dialog() const
    {
        return list_;
    }
    bool confirm_open() const
    {
        return confirm_.is_open();
    }
    // The focused tile's position and what it shows (tests).
    int focus_index() const
    {
        return index_;
    }
    bool on_utility_button() const
    {
        return on_button_;
    }
    bool utilities_open() const
    {
        return panel_ != Panel::none;
    }
    // -1 / 0 / 1: unknown / not installed / installed, for the tile at position.
    int tile_installed(int position) const;
    bool tile_favourite(int position) const;
    // Opens the Utilities panel on a page (0 menu, 1 sort, 2 filter): previews.
    void open_utilities(int page);

  private:
    enum class View : std::uint8_t
    {
        grid,
        detail,
    };
    enum class Pending : std::uint8_t
    {
        none,
        native_confirm,  // a .pkg: confirm the hand-off
        browser_confirm, // the PS5 browser will open for a check
        debrid_choice,   // which service
        select_files,    // which files of a debrid selection
        notice,          // information only
        parts_menu,      // a multipart card: all parts, or one part
        parts_confirm,   // "Download 22 parts from VikingFile?"
        parts_service,   // which debrid service takes all the parts
        more_menu,       // the game page header's three-dots menu
    };
    enum class Panel : std::uint8_t
    {
        none,
        menu,   // Utilities: Sort by, Filter by, Reset
        sort,   // the sort orders
        filter, // the grid's filters (kGridFilters)
        links,  // the game page's link filters
    };
    static constexpr int kColumns = 5;

    ui::Rect tile_rect(int index) const;
    void refresh_prefs();
    void rebuild();
    void read_facts(std::uint64_t serial, std::size_t from);
    std::vector<std::string> source_order() const;
    void prefetch_art();
    void persist();
    void changed(bool reset_focus);
    void open_search(Feedback &feedback);
    void update_search(float dt);
    bool toggle_favourite_at(int position);
    bool toggle_page_favourite();
    void update_grid(const InputFrame &input, Feedback &feedback, Intent &intent);
    void update_panel(const InputFrame &input, Feedback &feedback);
    void cycle_filter(int row, int delta, Feedback &feedback);
    void cycle_link_filter_row(int row, int delta, Feedback &feedback);
    int panel_rows() const;
    void open_game(int game);
    void load_page();
    void update_detail(const InputFrame &input, Feedback &feedback, Intent &intent);
    void update_header(const InputFrame &input, Feedback &feedback, Intent &intent);
    void apply_more(HeaderMore item, Feedback &feedback);
    void update_flow(Feedback &feedback, Intent &intent);
    void activate_row(Feedback &feedback);
    void activate_mirror(int mirror, Feedback &feedback);
    void activate_ref(const LinkRef &ref, Feedback &feedback);
    void start_single(const std::string &service);
    void start_batch(const std::string &service);
    void finish_batch();
    void save_started();
    void draw_header(hui::gfx::DrawList &list, float legend_alpha) const;
    void draw_tile_cell(hui::gfx::DrawList &list, const GameTile &tile, const ui::Rect &r, float focus) const;
    void draw_utility_button(hui::gfx::DrawList &list, float focus) const;
    void draw_panel(hui::gfx::DrawList &list) const;
    void draw_grid(Frame &frame, bool focused) const;
    void draw_detail(Frame &frame, bool focused) const;
    void draw_page_row(hui::gfx::DrawList &list, const PageRow &row, const ui::Rect &r, float focus) const;
    void draw_mirror(hui::gfx::DrawList &list, int mirror, const ui::Rect &r, float focus) const;
    void close_detail();

    View view_ = View::grid;
    // The grid.
    std::string query_;
    // The search box: the term to restore on Cancel, and the live-results debounce.
    std::string query_before_;
    std::uint32_t seen_edits_ = 0;
    float search_wait_ = -1.0f; // seconds until the typed text is applied; negative: nothing waiting
    CatalogueState state_; // sort, filters, favourites, recent searches
    InstalledIndex installed_index_;
    std::string header_filter_; // "Filter by:" value, built with the list
    std::vector<std::string> source_names_;
    std::vector<std::string> region_names_; // what the Region filter offers
    GameIndex games_;                       // the catalogue grouped into games
    std::vector<PackageFacts> facts_;       // per package, filled in chunks after a catalogue load
    std::uint64_t facts_serial_ = ~0ull;    // the catalogue the facts belong to
    bool facts_ready_ = false;
    GridAggregate agg_;                     // per-game filter answers and the panel's counts
    std::string agg_key_;                   // what agg_ was built from (rebuilt only when it changes)
    std::vector<GameTile> tiles_;           // what the grid shows, in order
    std::uint64_t seen_catalog_ = ~0ull;
    std::uint64_t seen_tiers_ = ~0ull;
    std::uint64_t seen_installed_ = ~0ull;
    std::shared_ptr<const PopularityIndex> pop_index_; // popularity data in use (null: off or not loaded)
    PopularityView pop_view_;
    std::uint64_t seen_pop_ = ~0ull;
    std::vector<SortMode> sort_choices_; // what the Utilities > Sort by panel offers
    bool dirty_ = false;       // rebuild at the next update
    bool reset_focus_ = false; // ... and put the focus back on the first tile
    std::string focus_game_;   // the focused game's id, kept across rebuilds
    int prefetch_row_ = -1; // the grid row the last art prefetch was made for
    std::uint64_t prefetch_serial_ = ~0ull;
    bool on_button_ = false; // the focus is on the round sort/filter button
    int index_ = 0;
    ui::Ease scroll_;
    ui::Ease button_focus_;
    std::vector<float> tile_focus_;
    // Link preferences (Settings > Wings > Downloads).
    const WingsLocalPrefs *prefs_source_ = nullptr;
    WingsLocalPrefs own_prefs_;
    WingsLocalPrefs seen_prefs_;
    bool have_prefs_ = false;
    LinkPrefs link_prefs_;
    // The Utilities panel (and the page's link filters).
    Panel panel_ = Panel::none;
    int panel_row_ = 0;
    ui::Ease panel_in_;
    std::vector<LinkFilterRow> link_rows_;
    // The game page.
    GameView page_;
    std::string page_game_; // GameGroup::id of the open page
    bool page_favourite_ = false;
    std::uint64_t page_request_ = 0;
    int row_ = 0; // index into page_.rows
    ui::Ease detail_scroll_;
    std::vector<float> row_focus_;
    bool waiting_detail_ = false;
    GameHeaderState header_; // the page header: focus, slide, hero (game_header.hpp)
    // Panels and the flow they belong to.
    TextPrompt prompt_;
    ConfirmDialog confirm_;
    ListDialog list_;
    Pending pending_ = Pending::none;
    LinkRef pending_ref_;     // the link a confirmation or choice is about
    PackageLink pending_link_;
    int parts_mirror_ = -1;   // the multipart card whose menu is open
    PartsPlan parts_plan_;
    std::string selection_id_;
    std::string selection_service_;
    std::vector<std::string> choice_services_; // ids for the debrid choice list
    std::uint64_t seen_link_ = 0;
    std::uint64_t seen_link_action_ = 0;
    // What this app asked Pegasus DL to download, and the single start in flight.
    StartedLinks started_;
    LinkRef single_ref_;
    bool single_waiting_ = false;
    DownloadAll batch_; // "Download all parts": one start_link() at a time (download_all.hpp)
    std::string batch_toast_; // shown by the next update
};

// The queue: every job with its phase, speed and ETA, and what can be done to it.
class DownloadsScreen final : public Screen
{
  public:
    explicit DownloadsScreen(Context &context) : Screen(context)
    {
    }
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override;

    // Tests and previews.
    const std::vector<std::string> &menu_codes() const
    {
        return menu_codes_;
    }
    // Tests: type into the open text prompt.
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    void open_add_link();
    int row_count() const
    {
        return static_cast<int>(order_.size());
    }
    int index() const
    {
        return index_;
    }
    // The after-download banner has the focus (above the list); 0 Close Wings now, 1 Not now.
    bool banner_focused() const
    {
        return banner_focus_;
    }
    int banner_button() const
    {
        return banner_button_;
    }

  private:
    enum class Pending : std::uint8_t
    {
        none,
        cancel_job,
        clear_finished,
        direct_link,
        notice,
        close_wings,
    };
    enum class ListFor : std::uint8_t
    {
        none,
        menu,
        archive_choice,
    };
    void rebuild();
    void open_menu(const Job &job);
    void run_menu(const std::string &code, const Job &job, Feedback &feedback);
    // The after-download banner above the list (downloads_scan_banner.cpp).
    bool banner_shown() const;
    float banner_offset() const; // how far it pushes the list down
    void banner_input(const InputFrame &input, Feedback &feedback, Intent &intent);
    void banner_draw(Frame &frame, bool focused) const;

    std::vector<std::string> order_; // job ids, active first
    int index_ = 0;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
    TextPrompt prompt_;
    ConfirmDialog confirm_;
    ListDialog list_;
    Pending pending_ = Pending::none;
    ListFor list_for_ = ListFor::none;
    std::string menu_job_;
    std::vector<std::string> menu_codes_;
    std::string password_job_;
    // Known-password trials of the jobs on this screen (see downloads_screen.cpp). One per
    // screen: a file-static one leaked between screens and tests.
    mutable std::shared_ptr<void> auto_passwords_state_;
    bool entering_url_ = false;
    bool probing_ = false;
    std::uint64_t seen_probe_ = 0;
    std::uint64_t seen_probe_action_ = 0;
    std::uint64_t seen_job_action_ = 0;
    // The after-download banner (downloads_scan_banner.cpp).
    bool banner_focus_ = false;
    int banner_button_ = 0;
};

// Installed games (Pegasus' /appdb): list, APR Emu status, update and reinstall,
// delete (double confirmation). Background jobs are polled until they finish.
class LibraryScreen final : public Screen
{
  public:
    explicit LibraryScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override;

    // Tests and previews.
    int index() const
    {
        return index_;
    }
    // Opens the action menu of the focused game (what Cross does).
    void open_menu();
    // Tests: type into the open text prompt (the delete confirmation).
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    bool prompt_open() const
    {
        return prompt_.is_open();
    }
    bool job_active() const
    {
        return job_.active;
    }
    const std::vector<std::string> &menu_codes() const
    {
        return menu_codes_;
    }

  private:
    enum class Pending : std::uint8_t
    {
        none,
        update,
        reinstall,
        delete_first,
        notice,
    };
    enum class ListFor : std::uint8_t
    {
        none,
        menu,
        versions,
    };
    struct Work
    {
        bool active = false;
        bool inflight = false;
        bool deleting = false;
        std::string title_id;
        std::string title;
        std::string job_id;
        std::string status;
        std::string detail;
        int progress = 0;
        std::int64_t files = 0;
        float next_poll = 0.0f;
    };
    const InstalledGame *focused_game() const;
    const InstalledGame *game(const std::string &title_id) const;
    bool versions_unavailable() const;
    void run_menu(const std::string &code, const InstalledGame &game, Feedback &feedback);
    void start_apr(const InstalledGame &game, const std::string &version_id,
                   const std::string &label);
    void start_delete(const InstalledGame &game);
    void poll_job();
    void finish_job(const std::string &toast);

    int index_ = 0;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
    TextPrompt prompt_;
    ConfirmDialog confirm_;
    ListDialog list_;
    Pending pending_ = Pending::none;
    ListFor list_for_ = ListFor::none;
    std::string target_;         // title id the open panel is about
    std::string version_id_;     // the build chosen for a reinstall
    std::string version_label_;
    std::vector<std::string> menu_codes_;
    std::vector<std::string> version_ids_;
    std::vector<std::string> version_labels_;
    Work job_;
    std::string toast_;
    bool versions_failed_ = false; // the version list answered with an error (503)
    std::string versions_error_;
};

// The file manager, one list at a time: drive chips (Main drive /data, each
// /mnt/extN and /mnt/usbN, the root) and a breadcrumb above the folder, Square
// to mark several items, Triangle for what can be done to them (rename, copy,
// move, delete, extract RAR archives, hand a .pkg to the installer), "Where
// to?" for a copy or move, and a strip of background operations that is always
// on screen. Circle always does something: clears the filter, the marks, or
// goes up (falling back further up when a parent cannot be listed); at "/" it
// hands the focus to the tab bar. Nothing blocks input while Pegasus answers.
// Uploading files to the console and downloading from it are not part of this
// screen: the console has no other device to send them to.
class FilesScreen final : public Screen
{
  public:
    explicit FilesScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override;
    // Every frame, whichever tab shows: operations are polled while one is
    // active (and now and then while the tab is visible).
    void background(bool visible);
    // An operation is running: the tab bar marks the Files tab.
    bool ops_active() const
    {
        return ops_.any_active() || posts_ > 0;
    }

    // Tests and previews.
    enum class Area : std::uint8_t
    {
        list,
        crumbs,
        chips,
    };
    const std::string &path() const
    {
        return path_;
    }
    int index() const
    {
        return index_;
    }
    int entry_count() const
    {
        return static_cast<int>(entries_.size());
    }
    const std::string &entry_name(int i) const
    {
        return entries_[static_cast<std::size_t>(i)].name;
    }
    const FileItem &entry(int i) const
    {
        return entries_[static_cast<std::size_t>(i)];
    }
    int marked_count() const
    {
        return static_cast<int>(marks_.size());
    }
    int op_count() const
    {
        return static_cast<int>(ops_.active().size() + ops_.finished().size());
    }
    const files::OpsTracker &ops() const
    {
        return ops_;
    }
    bool loading() const
    {
        return loading_;
    }
    const std::string &error() const
    {
        return error_;
    }
    Area area() const
    {
        return area_;
    }
    bool ops_panel_open() const
    {
        return ops_open_;
    }
    bool cancel_asked() const
    {
        return ops_confirm_;
    }
    // The operations strip as drawn: its main line and the button it names.
    std::string strip_text() const;
    const std::string &filter() const
    {
        return filter_;
    }
    void set_filter(const std::string &text);
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    bool prompt_open() const
    {
        return prompt_.is_open();
    }
    DestinationBrowser &destination()
    {
        return dest_;
    }
    const std::vector<std::string> &menu_codes() const
    {
        return menu_codes_;
    }
    const std::deque<std::string> &trace_lines() const
    {
        return trace_.lines();
    }
    // Opens a folder (as the chips and the breadcrumb do).
    void open_path(const std::string &path);
    // Marks an entry by name (previews).
    void mark(const std::string &name);
    // Opens "Where to?" for the marked items (or the focused one): previews.
    void open_destination(bool move);
    void open_operations_panel();

  private:
    enum class Pending : std::uint8_t
    {
        none,
        delete_items,
        install_pkg,
        replace_ok,
        notice,
    };
    enum class ListFor : std::uint8_t
    {
        none,
        actions,
        sort,
    };
    enum class Typing : std::uint8_t
    {
        none,
        rename,
        new_folder,
        password,
        filter,
    };
    enum class Transfer : std::uint8_t
    {
        none,
        copy,
        move,
        extract,
    };
    enum class Nav : std::uint8_t
    {
        open,    // a folder, a chip, a crumb: an error stays on that folder
        walk_up, // going up: a folder that cannot be listed falls back to its parent
        refresh, // the same folder again, quietly (rows stay while it loads)
    };
    void trace(const std::string &line);
    void load_locations();
    void navigate(const std::string &path, Nav how, const std::string &focus_name);
    void refresh();
    void go_up(Feedback &feedback, Intent &intent);
    void rebuild_shown(const std::string &focus_name);
    void poll_ops();
    void take_ops(const std::vector<FileOp> &ops, const std::string &why);
    void ask_password(const FileOp &op, Feedback &feedback);
    std::vector<std::string> targets() const;
    std::vector<FileItem> target_items() const;
    const FileItem *focused_item() const;
    const FileItem *find_entry(const std::string &path) const;
    void open_actions();
    void run_action(const std::string &code, Feedback &feedback);
    void start_transfer(const std::string &collision);
    void post_ops(const char *starting, std::function<Result<std::vector<FileOp>>(Backend &)> work,
                  const std::string &done_toast);
    void report(const std::string &error);
    std::string default_destination(Transfer kind) const;
    void update_list(const InputFrame &input, Feedback &feedback, Intent &intent);
    void update_header(const InputFrame &input, Feedback &feedback, Intent &intent);
    void update_ops_panel(const InputFrame &input, Feedback &feedback);
    std::vector<const files::OpsTracker::Tracked *> panel_ops() const;
    int error_buttons() const;
    void press_error_button(int button, Feedback &feedback);
    void draw_header(hui::gfx::DrawList &list, bool focused) const;
    void draw_list(hui::gfx::DrawList &list, bool focused) const;
    void draw_strip(hui::gfx::DrawList &list) const;
    void draw_ops_panel(Frame &frame) const;

    // Browsing.
    std::string path_ = "/data"; // the folder shown, or being opened
    bool started_ = false;
    std::vector<FileItem> listed_;  // as Pegasus listed it
    std::vector<FileItem> entries_; // filtered and sorted, as shown
    bool writable_ = true;
    bool loading_ = false;
    bool listed_ok_ = false; // listed_ belongs to path_
    std::string error_;
    float load_started_ = 0.0f;  // ctx clock when the folder was asked for
    std::uint64_t folder_req_ = 0;    // folder listings only
    std::uint64_t locations_req_ = 0; // the locations (drive chips) only
    std::uint64_t ops_req_ = 0;       // operation polls only
    double folder_asked_at_ = 0.0;
    std::vector<StorageMount> locations_;
    std::vector<files::Drive> drives_;
    bool have_locations_ = false;
    bool locations_loading_ = false;
    float locations_at_ = -100.0f;
    std::string filter_;
    files::SortKey sort_ = files::SortKey::name;
    bool sort_descending_ = false;
    Area area_ = Area::list;
    int chip_ = 0;
    int crumb_ = 0;
    int error_button_ = 0;
    int index_ = 0;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
    std::vector<std::string> marks_;
    // Operations.
    files::OpsTracker ops_;
    bool ops_inflight_ = false;
    float next_ops_poll_ = 0.0f;
    bool visible_ = false;
    int posts_ = 0;            // copy/move/... calls not answered yet
    std::string starting_;     // what the strip says meanwhile
    std::vector<std::string> password_asked_;
    bool failed_unseen_ = false;
    bool ops_open_ = false;
    int ops_focus_ = 0;
    bool ops_confirm_ = false;
    int ops_confirm_button_ = 0; // 0 keep going, 1 stop it
    std::string ops_confirm_id_;
    std::string ops_note_; // why Square was refused, under the panel's title
    ui::Ease ops_in_;
    // Panels and the flow they belong to.
    TextPrompt prompt_;
    ConfirmDialog confirm_;
    ListDialog list_;
    DestinationBrowser dest_;
    Pending pending_ = Pending::none;
    ListFor list_for_ = ListFor::none;
    Typing typing_ = Typing::none;
    Transfer transfer_ = Transfer::none;
    std::vector<std::string> menu_codes_;
    std::vector<std::string> subject_; // what the open flow acts on
    std::vector<FileItem> subject_items_;
    std::string subject_from_;
    std::string destination_;
    std::string last_destination_[4]; // by Transfer, this session
    std::string password_op_;
    // Known-password trials of the extractions on this tab (one per screen, not file-static).
    mutable std::shared_ptr<void> auto_extract_state_;
    std::string toast_;
    files::Trace trace_;
};

// The sources Pegasus reads packages from: add, switch, refresh, delete, and
// the suggested (default) sources, including the first-run onboarding.
class SourcesScreen final : public Screen
{
  public:
    explicit SourcesScreen(Context &context) : Screen(context)
    {
    }
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override;

    // Tests and previews.
    // Tests: type into the open text prompt.
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    void open_defaults();
    void open_add_url();
    const std::vector<std::string> &menu_codes() const
    {
        return menu_codes_;
    }
    int index() const
    {
        return index_;
    }

  private:
    enum class Pending : std::uint8_t
    {
        none,
        delete_source,
        notice,
    };
    enum class ListFor : std::uint8_t
    {
        none,
        menu,
        defaults,
    };
    void open_menu(const Source &source);
    void run_menu(const std::string &code, const Source &source, Feedback &feedback);

    int index_ = 0;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
    TextPrompt prompt_;
    ConfirmDialog confirm_;
    ListDialog list_;
    Pending pending_ = Pending::none;
    ListFor list_for_ = ListFor::none;
    std::string menu_source_;
    std::vector<std::string> menu_codes_;
    std::vector<std::string> default_ids_;
    bool onboarding_offered_ = false;
    std::uint64_t seen_source_action_ = 0;
};

class SettingsScreen final : public ListScreen
{
  public:
    explicit SettingsScreen(Context &context);
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    bool modal() const override
    {
        return confirm_.is_open() || list_.is_open() || prompt_.is_open() || picker_.is_open() ||
               ctx_.store.lane_state(Store::lane_admin).busy;
    }
    // Tests and previews.
    std::vector<Row> row_list() const
    {
        return rows();
    }
    // Tests: type into the open text prompt (a token, a number).
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    bool prompt_open() const
    {
        return prompt_.is_open();
    }
    FolderPicker &picker()
    {
        return picker_;
    }
    const std::vector<std::string> &menu_codes() const
    {
        return menu_codes_;
    }
    // The two groups: what Wings keeps on the console, and what changes Pegasus DL.
    enum class Group : std::uint8_t
    {
        wings,
        pegasus,
    };
    Group group() const
    {
        return group_;
    }
    void set_group(Group group)
    {
        group_ = group;
        index_ = 0;
    }
    // Wings' own preferences (link ranking, popularity); other screens may read them.
    const WingsLocalPrefs &local_prefs() const
    {
        return prefs_;
    }
    void set_local_prefs(const WingsLocalPrefs &prefs)
    {
        prefs_ = prefs;
        store_local_prefs();
    }

  protected:
    const char *title() const override
    {
        return tr("Settings");
    }
    std::string subtitle() const override
    {
        return group_ == Group::wings ? tr("Wings: kept on this console by Wings only")
                                      : tr("Pegasus DL: applies to Pegasus DL on this console");
    }
    std::vector<Row> rows() const override;
    bool needs_pegasus() const override
    {
        return false;
    }
    void on_frame(bool focused) override;
    void on_confirm(int row, Feedback &feedback, Intent &intent) override;
    const char *confirm_label() const override
    {
        return tr("Select");
    }

  private:
    enum class Act : std::uint8_t
    {
        none,
        doh,
        clear_cache,
        retry_failed,
        capabilities,
        folder,
        connections,
        real_debrid,
        torbox,
        group,
        reset_grid,
        pref_firmware,
        pref_region,
        pref_format,
        pref_debrid,
        popularity,
        popularity_url,
        popularity_refresh,
        dns_refresh,
        providers,
        tile_details,
        pegasus_startup,
        after_download,
        export_titles,
        pegasus_check,
        pegasus_download,
        pegasus_cancel,
        pegasus_send,
        update_check,
        wings_update,
        icon_source,
        hero_source,
    };
    enum class Pending : std::uint8_t
    {
        none,
        doh_on,
        reset_grid,
        clear_token,
        notice,
        pegasus_download,
        pegasus_send,
    };
    enum class ListFor : std::uint8_t
    {
        none,
        capabilities,
        connections,
        token_menu,
        firmware,
        region,
        format,
        providers,
        wings_update,
    };
    enum class Typing : std::uint8_t
    {
        none,
        connections,
        token,
        popularity_url,
    };
    std::vector<Row> build(std::vector<Act> *acts) const;
    void build_wings(std::vector<Row> &out, std::vector<Act> *acts) const;
    void build_pegasus(std::vector<Row> &out, std::vector<Act> *acts) const;
    // Settings > Pegasus DL > Pegasus DL on this console (settings_pegasus_dl.cpp).
    void build_pegasus_manager(std::vector<Row> &out, std::vector<Act> *acts) const;
    void open_choice(ListFor which, const char *title, const char *const *choices, int count,
                     const std::string &current);
    void store_local_prefs();
    void open_token_menu(bool torbox);
    void send(const SettingsPatch &patch, const std::string &done_text);

    CoverArtStatus art_; // read once per frame (on_frame)
    ConfirmDialog confirm_;
    ListDialog list_;
    TextPrompt prompt_;
    FolderPicker picker_;
    Pending pending_ = Pending::none;
    ListFor list_for_ = ListFor::none;
    Typing typing_ = Typing::none;
    bool token_torbox_ = false;
    std::vector<std::string> menu_codes_;
    std::string toast_;
    Group group_ = Group::wings;
    WingsLocalPrefs prefs_;
};

// Pegasus DL's own log (level filter, follow, clear) and Wings' diagnostics.
class LogsScreen final : public Screen
{
  public:
    explicit LogsScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override;

    // Tests and previews.
    enum class Level : std::uint8_t
    {
        all,
        warnings,
        errors,
    };
    Level level() const
    {
        return level_;
    }
    bool following() const
    {
        return follow_;
    }
    bool showing_diagnostics() const
    {
        return diagnostics_;
    }
    int shown_count() const
    {
        return static_cast<int>(shown_.size());
    }
    void set_level(Level level);
    void show_diagnostics(bool on);
    const std::vector<std::string> &menu_codes() const
    {
        return menu_codes_;
    }

  private:
    enum class Pending : std::uint8_t
    {
        none,
        clear,
        notice,
    };
    void rebuild();
    void open_menu();
    std::vector<std::string> diagnostics_lines() const;

    Level level_ = Level::all;
    bool follow_ = true;
    bool diagnostics_ = false;
    std::vector<int> shown_; // indexes into the snapshot's logs
    std::vector<std::string> diag_;
    std::uint64_t seen_serial_ = ~0ull;
    int index_ = 0;
    ui::Ease scroll_;
    ConfirmDialog confirm_;
    ListDialog list_;
    Pending pending_ = Pending::none;
    std::vector<std::string> menu_codes_;
    std::string toast_;
};

// The whole app: top bar, tabs, status banners, the browser-capture panel and the toast.
class App
{
  public:
    explicit App(Context &context);
    void update(const InputFrame &input, float dt, Feedback &feedback);
    void draw(Frame &frame) const;
    // Preview and tests: jump straight to a place.
    void show_tab(int tab);
    int current_tab() const
    {
        return tab_;
    }
    bool top_bar_focused() const
    {
        return area_ == Area::top;
    }
    const theme::Wash &wash() const
    {
        return wash_;
    }
    void show_toast(std::string text, float seconds = 2.8f);
    // The browser-capture panel is showing (not hidden by the player).
    bool capture_panel_open() const;
    // Its focused button: 0 Hide / Close (drawn at the right), 1 Cancel capture (left of it).
    int capture_button() const
    {
        return capture_button_;
    }
    StoreScreen &store_screen()
    {
        return store_;
    }
    DownloadsScreen &downloads_screen()
    {
        return downloads_;
    }
    SourcesScreen &sources_screen()
    {
        return sources_;
    }
    LibraryScreen &library_screen()
    {
        return library_;
    }
    FilesScreen &files_screen()
    {
        return files_;
    }
    LogsScreen &logs_screen()
    {
        return logs_;
    }
    SettingsScreen &settings_screen()
    {
        return settings_;
    }
    const std::string &toast() const
    {
        return toast_;
    }
    PegasusOffer &pegasus_offer() // the preview scenes show fixed states of the first-run choices
    {
        return pegasus_offer_;
    }
    AfterInstall &after_install() // the preview scenes show fixed states of the Downloads banner
    {
        return after_install_;
    }

  private:
    enum class Area : std::uint8_t
    {
        top,
        content,
    };
    Screen &screen();
    const Screen &screen() const;
    void draw_top_bar(hui::gfx::DrawList &list) const;
    void draw_status_pill(hui::gfx::DrawList &list) const;
    void update_capture(const InputFrame &input, float dt, Feedback &feedback);
    void draw_capture(Frame &frame) const;
    void handle_intent(const Intent &intent, Feedback &feedback);

    Context &ctx_;
    StoreScreen store_;
    DownloadsScreen downloads_;
    LibraryScreen library_;
    FilesScreen files_;
    SourcesScreen sources_;
    SettingsScreen settings_;
    LogsScreen logs_;
    PegasusOffer pegasus_offer_; // first-run "load Pegasus DL" choices on the waiting card
    AfterInstall after_install_; // asks ShadowMount to scan when a download finishes
    int tab_ = tab::store;
    Area area_ = Area::content;
    int top_index_ = 0;
    std::string toast_;
    float toast_time_ = 0.0f;
    float toast_length_ = 2.8f;
    std::vector<ui::Ease> top_focus_;
    theme::Wash wash_; // the place / section colour behind every screen (the App owns it)
    // The browser-capture panel (Pegasus opened the PS5 browser for a check).
    std::string capture_hidden_; // the session the player hid
    int capture_button_ = 0;     // 0 hide / close, 1 cancel the capture
    ui::Ease capture_in_;
    std::uint64_t capture_toasted_ = 0; // capture_serial whose completion was announced
    // A newer Wings (wings_update.hpp): looked for when the network is ready, announced once per start.
    float update_poll_ = 0.0f;
    bool update_announced_ = false;
};

} // namespace orbit
