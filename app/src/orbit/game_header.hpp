// Wings TV app - The game page's header: hero band, big cover, title, info cards, buttons.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The top of a game's page in the Orbit-hero style: art fills the top of the
// screen under a dark veil, the cover is large at the left with the title
// typeset beside it, then info cards (region, firmware, ...), the popularity,
// and the buttons: Install (slides the page down to the downloads), Favourite
// and "more". The pure parts (which cards, hero URL validation, layout, focus
// steps) are here and tested on the host; drawing is in game_header.cpp.
//
// Hero art is optional per title: the popularity index may carry
// "hero":"https://..." (popularity.hpp). It is registered here, fetched only
// while a game page is open (the art cache asks the resolver for ArtKind::hero)
// and never for grid tiles. Without it, or while it loads or after it failed,
// the header builds a hero from the cover (scaled up, ambient wash, tinted).

#pragma once

#include "orbit/game_view.hpp"
#include "orbit/popularity.hpp"
#include "orbit/ui.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

struct Context;

// ---- hero URLs ----

constexpr std::size_t kHeroUrlMax = 300;
// https:// only, at most kHeroUrlMax bytes, no credentials, no spaces or control characters.
bool valid_hero_url(std::string_view url);

// Art key ("pkg:<id>") -> hero URL for the page that is open. Any thread:
// PosterIndex::resolve (art worker) reads it, the frame thread sets it.
// Holds a handful of entries: the open page's (and none once it closes).
class HeroUrls
{
  public:
    // False (and nothing stored) for an invalid URL or an empty key.
    bool set(const std::string &art_key, const std::string &url);
    // The small stand-in shown while the full one loads (same rules; stored beside it).
    bool set_low(const std::string &art_key, const std::string &url);
    std::string find(std::string_view art_key) const;
    std::string find_low(std::string_view art_key) const;
    void clear();
    std::size_t size() const;

  private:
    mutable Mutex mutex_;
    std::map<std::string, std::string, std::less<>> urls_;
};
HeroUrls &hero_urls();

// ---- hero placement ----

// Where an image of width x height goes in the band, by its shape (a = width / height). The art is only
// ever scaled uniformly (its proportions never change) until it covers the whole band, and the overflow is
// cropped: nothing is stretched and the band is always full. A very wide strip would have to zoom so far
// that a few letters fill the screen, and a square logo would show almost nothing of itself, so those are
// not used and the page keeps the cover wash:
//   1.5 .. 3.3   used (Steam's 3.1:1 art loses its sides),
//   anything else is not used.
constexpr float kHeroAspectMin = 1.5f;
constexpr float kHeroAspectMax = 3.3f;
struct HeroPlacement
{
    bool usable = false;
    ui::Rect dest;                 // inside the band
    ui::Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
};
HeroPlacement place_hero(int width, int height, const ui::Rect &band);

// ---- holding the cover wash back for a cached hero (B-14 step 1) ----

constexpr float kHeroWashHoldS = 0.3f;
// True while the blurred cover wash should NOT be drawn (only the dark gradient), because the
// 16:9 picture is on disk and is about to fade in. False when there is no hero address, the file
// is not cached, the picture was refused (unusable shape), or the hold (300 ms from page open) ran out.
bool hold_cover_wash(bool has_hero_address, bool cached, float since_open_s, bool hero_refused);

// Hooks the app wires once (main.cpp); both optional. cached answers "is this URL on disk";
// log writes one line to the title log.
struct HeroHooks
{
    std::function<bool(const std::string &url)> cached;
    std::function<void(const std::string &line)> log;
};
HeroHooks &hero_hooks();
// The cached fact for a url ("" or no hook: false). Call where hero_url is set.
bool hero_is_cached(const std::string &url);

// ---- the info cards ----

enum class CardTone : std::uint8_t
{
    plain, // white value
    tier,  // coloured by CardTone's tier (the link support tiers)
    fit,   // coloured by the firmware fit
};

struct HeaderCard
{
    std::string label; // "Region", "Firmware", "Format", "Size", "Version", "Source", "Links"
    std::string value;
    CardTone tone = CardTone::plain;
    LinkTier tier = LinkTier::unknown;
    FwFit fit = FwFit::runs;
};

// What the cards are built from: the best download's card (MirrorCard) when
// there is one, else the game's own lists.
struct HeaderFacts
{
    std::vector<std::string> regions; // the game's, known order
    std::string region;               // the shown download's
    std::string firmware;             // "12.xx"
    std::string firmware_note;        // "runs on 4.xx+", "needs 12.xx"
    FwFit fit = FwFit::runs;
    bool fit_known = false;
    std::string format;
    std::string version;
    std::string source;      // the shown download's source
    std::vector<std::string> sources;
    std::string size_text;   // "61.2 GB" (a listed size), "" when none
    std::string size_source; // whose listing it is
    LinkTier tier = LinkTier::unknown;
    bool have_download = false; // a mirror was shown: its tier is the "Links" card
    std::string parts;          // the shown download's "22 parts (1-22)" / "1 file"
    int link_count = 0;         // links on the page (after de-duplication)
};

// In display order; a card is left out when it has nothing to say. Values are
// shortened to what fits a card (kValueMax bytes, ellipsis).
constexpr std::size_t kHeaderValueMax = 26;
std::vector<HeaderCard> build_header_cards(const HeaderFacts &facts);
// The facts of the page: the Best mirror (else the first shown) and the game's lists.
HeaderFacts header_facts(const GameView &view, const LinkPrefs &prefs, const StartedLinks &started);

// ---- popularity ----

struct HeaderRating
{
    bool shown = false; // false: no record, or one below the confidence floor (popularity.hpp)
    int score = 0;      // 0..100
    int rating = -1;    // 0..100, -1 unknown
    std::string rating_text; // "92 (4.3k)", "92", ""
    long count = -1;         // how many ratings, -1 unknown
};
// `record` is PopularityIndex::best(): null hides everything.
HeaderRating header_rating(const PopRecord *record);

// ---- buttons ----

enum class HeaderButton : std::uint8_t
{
    install,
    favourite,
    more,
};
constexpr int kHeaderButtons = 3;
// Left / right on the row: the button after the step, or `button` at an edge.
int header_step(int button, hui::Direction nav);

// The three-dots menu: the page's secondary actions.
enum class HeaderMore : std::uint8_t
{
    filters,       // open the link filters
    clear_filters, // only while a filter is set
    cycle_order,   // the cards' order: best / ...
};
std::vector<HeaderMore> header_more_items(bool has_downloads, bool filter_active);

// ---- layout ----

struct HeaderLayout
{
    float text_x = 0.0f;
    float text_w = 0.0f;
    float title_size = 0.0f;
    float title_baseline[2] = {0.0f, 0.0f};
    int title_lines = 1;
    float meta_baseline = 0.0f;
    float cards_top = 0.0f;
    float card_w = 0.0f;
    int card_rows = 0;
    float rating_top = 0.0f; // 0 when no rating row
    float buttons_top = 0.0f;
    float list_top = 0.0f;   // where the downloads list starts while the header is shown
};
// width is the screen's (1920). cards: how many; two_line_title: the title does
// not fit one line at the large size.
HeaderLayout header_layout(float width, int cards, bool rating, bool two_line_title);

// ---- state ----

struct GameHeaderState
{
    bool on_header = true; // the focus is on the header's buttons (else on the list)
    int button = 0;        // HeaderButton while on_header
    ui::Ease slide;        // 0: header shown, 1: slid up and the list is at the top
    float focus[kHeaderButtons] = {0.0f, 0.0f, 0.0f};
    std::string hero_url;       // registered for this page ("" none)
    std::string hero_low_url;   // the small stand-in registered for this page ("" none)
    bool hero_resolved = false; // the popularity index has been asked for this page
    bool hero_cached = false;   // the hero_url's file is already on disk (hero_is_cached, set with hero_url)
    float open_s = 0.0f;        // seconds since the page opened (update)
    mutable bool hero_logged = false; // the once-per-open "first hero frame" line has been written
    std::vector<HeaderMore> more; // the open "more" menu's rows

    void reset();                        // a page opens
    void update(float dt, bool focused); // eases slide and focus
};

// What draw needs, built by the screen.
struct HeaderView
{
    const GameView *page = nullptr;
    std::vector<HeaderCard> cards;
    HeaderFacts facts;      // the hero's chips and facts panel (game_hero.hpp)
    HeaderRating rating;
    std::string rating_live; // the live Steam-review line, when the index has no record ("" none)
    bool favourite = false;
    bool loading = false;
    bool failed = false;
    std::string status;     // "Loading the links...", or the error
    std::string meta_line;  // "22 links · 5 releases · 2 sources"
    std::string hero_key;   // art key to ask for ArtKind::hero / hero_low ("" none registered)
    bool hero_full = false; // a full hero is registered
    bool hero_low = false;  // a small one is registered
};

// The measured layout (the title's width decides one or two lines).
HeaderLayout game_header_layout(Context &ctx, const HeaderView &view);
// Draws the header at its current slide: the hero band, the cover, the title,
// the cards, the rating and the buttons. `focused`: the screen has the focus.
void draw_game_header(Context &ctx, hui::gfx::DrawList &list, const HeaderView &view,
                      const HeaderLayout &layout, const GameHeaderState &state, bool focused);

} // namespace orbit
