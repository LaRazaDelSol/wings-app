// Wings TV app - The game page's hero: tag chips, facts panel rows and the page geometry.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the hero shows besides the cover and the title, as pure data so it is tested on the host:
// the outlined tag chips (region, firmware fit, format, link tier), the rows of the quiet glass facts
// panel at the right (source, size, version, firmware, region, files, links) and the vertical layout
// of the whole first viewport at 1920 x 1080. Drawing is in game_header.cpp, the shape icons in
// game_icons.hpp. Every value comes from HeaderFacts (the best download's card and the game's own
// lists); a row with nothing to say is left out, nothing is invented.

#pragma once

#include "orbit/game_header.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orbit::hero
{

enum class Icon : std::uint8_t
{
    source,
    size,
    version,
    firmware,
    region,
    files,
    links,
};

enum class Tone : std::uint8_t
{
    plain,
    tier, // coloured by the link tier
    fit,  // coloured by the firmware fit
};

// One outlined tag. Semantic tones colour the text and the outline, always with a word.
struct Chip
{
    std::string text;
    Tone tone = Tone::plain;
    LinkTier tier = LinkTier::unknown;
    FwFit fit = FwFit::runs;
};

constexpr std::size_t kMaxChips = 4;
// Region, firmware fit, format, link tier: in that order, each only when known.
std::vector<Chip> chips(const HeaderFacts &facts);
// How many of the chips (in order) fit `max_width` with `gap` between them; chips never wrap.
int chips_that_fit(const std::vector<float> &widths, float max_width, float gap);

// One row of the facts panel: icon, label, value, then an optional coloured word.
struct Fact
{
    Icon icon = Icon::source;
    std::string label;
    std::string value;
    std::string accent; // "Fits", "Needs newer", "Supported": coloured by tone, "" none
    Tone tone = Tone::plain;
    LinkTier tier = LinkTier::unknown;
    FwFit fit = FwFit::runs;
};

constexpr std::size_t kMaxFactRows = 6; // above the divider; the links row sits under it
// Rows in display order. The last row is the links row (Icon::links) when there is one: the panel
// draws it under a divider.
std::vector<Fact> fact_rows(const HeaderFacts &facts);
// How many rows sit above the divider, and whether the links row follows.
int rows_above_divider(const std::vector<Fact> &rows);
bool has_links_row(const std::vector<Fact> &rows);

// ---- geometry (virtual 1920 x 1080) ----

constexpr float kScreenH = 1080.0f;
constexpr float kIconSize = 340.0f;      // the cover: bigger, and lower so it sits in the page's middle
constexpr float kIconTop = 170.0f;
constexpr float kIconGap = 40.0f;        // cover to title
constexpr float kTitleBlockBottom = 420.0f; // lowest meta line: a panel starting above it narrows the title
constexpr float kButtonH = 64.0f;
constexpr float kButtonsTop = 774.0f;    // the stack is anchored here, upwards
constexpr float kPanelW = 440.0f;
constexpr float kPanelPad = 22.0f;
constexpr float kRowPitch = 58.0f;
constexpr float kDividerGap = 28.0f;
constexpr float kPanelGap = 40.0f;       // left stack to the panel
constexpr float kLabelBaseline = 950.0f; // "Downloads", peeking at the bottom edge
constexpr float kListTop = 1000.0f;      // the list starts under the first viewport

struct Layout
{
    ui::Rect icon;
    float title_x = 0.0f;
    float title_w = 0.0f;
    float title_size = 0.0f;
    float title_baseline[2] = {0.0f, 0.0f};
    int title_lines = 1;
    float meta_baseline = 0.0f;
    float stack_x = 0.0f;         // chips, rating, offer, buttons: the left stack
    float stack_w = 0.0f;         // up to the panel
    float chips_cy = 0.0f;        // 0 when no chips
    float rating_baseline = 0.0f; // 0 when none
    float offer_baseline = 0.0f;  // 0 when none
    float why_baseline = 0.0f;    // 0 when none
    float buttons_top = 0.0f;
    ui::Rect panel;               // empty (h == 0) when there are no rows
    float divider_y = 0.0f;       // 0 when no links row
    float first_row_cy = 0.0f;
    float label_baseline = 0.0f;
    float list_top = 0.0f;
};

// The title's width: the page minus the gutters and the cover, and, when the facts panel is tall
// enough to rise beside the title block, stopped short of the panel.
float title_width(float width, int rows, bool links_row);

// rows: the fact rows above the divider; links_row: one under it.
Layout layout(float width, bool two_line_title, bool chips, bool rating, bool offer, bool why, int rows,
              bool links_row);

} // namespace orbit::hero
