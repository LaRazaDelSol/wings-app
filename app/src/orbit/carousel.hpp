// Wings TV app - The Store's curated carousel: which games, when it shows, the pad rules, the timer and the
// grid offset it adds. Pure and tested on the host; drawing and input wiring are in store_screen.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The carousel is the project's featured games (editorial.hpp, curated.v1.json), shown above the Store grid in
// its default view only. Each slide is one game of the player's OWN catalogue (found by title id; ids they do
// not have are hidden) with a 16:9 picture, the title, an Install button (opens the game page, as Cross on the
// tile does) and a Favourite button.
//
// Pad rules (the focus is always on one of the two buttons; the slide is whatever the buttons belong to):
//   Right on Install   -> Favourite            Left on Favourite -> Install
//   Right on Favourite -> next slide, focus stays on Favourite   (the last slide wraps to the first)
//   Left on Install    -> previous slide, focus stays on Install (the first slide wraps to the last)
//   Down returns to the grid (its first row), Up leaves to the top bar. Entering from the grid (Up from row 0)
//   lands on Install. Holding Right or Left therefore walks through the slides after two steps.

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

// Geometry (virtual 1920 x 1080). The slide is as wide as the grid; its picture is 16:9 at the slide's height
// on the right and fades into the slide's plate at its left edge, where the words are.
constexpr float kCarouselHeight = 400.0f;
constexpr float kCarouselGap = 30.0f;                              // slide to the first grid row
constexpr float kCarouselExtra = kCarouselHeight + kCarouselGap;   // what the grid is pushed down by
constexpr float kCarouselArtAspect = 16.0f / 9.0f;

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
constexpr int kCarouselInstall = 0;
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

// The grid's scroll target (px) with `extra` px of carousel above row 0. The focused row sits second from the
// top once the list scrolls; with extra == 0 this is exactly the grid's own rule: max(0, (row - 1) * pitch).
float grid_scroll_target(int row, float pitch, float extra);

} // namespace orbit
