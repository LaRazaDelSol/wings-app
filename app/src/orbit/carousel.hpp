// Wings TV app - The Store's curated carousel: which games, when it shows, the pad rules, the timer and the
// grid offset it adds. Pure and tested on the host; drawing and input wiring are in store_screen.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The carousel is the project's featured games (editorial.hpp, curated.v1.json), the Store's hero shot in its
// default view only: the first thing on the page, full width, with the grid below it. Each slide is one game of
// the player's OWN catalogue (found by title id; ids they do not have are hidden) with a 16:9 picture, the title,
// a View game button (opens the game page, as Cross on the tile does) and a heart (Favourite).
//
// Pad rules (the focus is always on one of the two buttons; the slide is whatever the buttons belong to):
//   Right on View game -> heart                Left on heart -> View game
//   Right on heart     -> next slide, focus stays on the heart   (the last slide wraps to the first)
//   Left on View game  -> previous slide, focus stays on View game (the first slide wraps to the last)
//   Down scrolls the page up to the grid (its header and first row), Up leaves to the top bar. Up from the grid's
//   first row comes back to the hero, on View game. The Store opens with the focus on the hero. Holding Right or
//   Left therefore walks through the slides after two steps.

#pragma once

#include "core/input.hpp"
#include "orbit/game_group.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace orbit
{

constexpr std::size_t kCarouselMaxItems = 8;
constexpr float kCarouselAdvanceS = 8.0f;

// Geometry (virtual 1920 x 1080). The hero is the page's first viewport: the picture fills the whole width from
// the top of the screen (under the app bar) down to kCarouselArtH and fades into the page at its foot; the title
// and the buttons sit over it at the left. The grid is pushed down by kCarouselExtra, so its header and the top of
// its first row peek in under the hero (as Orbit's first rail does); Down scrolls the page up by that much.
constexpr float kCarouselArtH = 800.0f;
constexpr float kCarouselExtra = 590.0f; // what the grid is pushed down by

// The games of the curated ids, in the file's order: the first game of the player's catalogue that carries each
// id (a game that comes up twice is kept once), at most kCarouselMaxItems. Indexes into `games`.
std::vector<int> resolve_carousel(const std::vector<std::string> &ids, const std::vector<GameGroup> &games);

// Shown whenever the preference is on and there is something to show: at least one game, no search under way and
// something in the grid below it. Filters and the sort do not hide it.
bool carousel_shown(bool pref_on, std::size_t resolved, bool query_empty, std::size_t tiles);

// How many games the carousel falls back to when the featured list gives none (nothing fetched, or none of the
// featured games is in the catalogue).
constexpr std::size_t kCarouselFallbackItems = 5;
// The `count` games with the largest popularity key (keys[i] belongs to games[i]; -1 = no data, sorts last). With
// no data at all it is simply the first `count` games. Indexes into the list the keys came from.
std::vector<int> pick_most_popular(const std::vector<long> &keys, std::size_t count);

// The two focus stops.
constexpr int kCarouselView = 0;
constexpr int kCarouselFavourite = 1;

struct CarouselStep
{
    int slide = 0;
    int button = 0;
    bool moved = false;       // the focus or the slide changed (the pad gets a sound; false: a refusal)
    bool slide_changed = false;
};
// Left / Right on the buttons (the rules above). Other directions change nothing. count is >= 1.
CarouselStep carousel_step(int slide, int button, int count, hui::Direction nav);
// slide + delta, wrapping inside 0..count-1.
int carousel_wrap(int slide, int delta, int count);

// The auto-advance clock.
class CarouselClock
{
  public:
    // Every frame. paused: the carousel has focus, a dialog or panel is open, it is not on screen. True when the
    // slide should advance now (the clock then starts again).
    bool update(float dt, bool paused, int count);
    void restart() { elapsed_ = 0.0f; }
    float elapsed() const { return elapsed_; }

  private:
    float elapsed_ = 0.0f;
};

// The grid's scroll target (px) with `extra` px of carousel above row 0. Row 0 scrolls the carousel away (the
// header and the first row then sit where they do without one); from there the focused row sits second from the
// top once the list scrolls. With extra == 0 this is exactly the grid's own rule: max(0, (row - 1) * pitch).
float grid_scroll_target(int row, float pitch, float extra);

// How much of the hero is left at a page scroll (1 at rest, 0 once it is far enough up to be out of the way of
// the header that rides up with the page): the hero fades as it slides, so it never meets the header's words.
float carousel_fade(float scroll);

} // namespace orbit
