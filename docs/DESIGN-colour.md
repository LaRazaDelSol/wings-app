# Wings colour system (spec for implementation)

Status: design, 2026-10-12. Replaces the ad-hoc washes from S1/G1/F1. Implementation lives in ONE place
(`app/src/orbit/theme.hpp`); screens ask it for colours and never pick hues themselves.

## Principle
Colour answers exactly one of four questions. If a colour does not answer one, it is removed.

| Layer | Question it answers | Where it appears | Colour source |
|---|---|---|---|
| 1. Place | "Where am I?" | A wide, very soft wash across the top of the whole screen and the dot of the active tab | One hue per tab (table below) |
| 2. Section | "Which part of this place?" | The same wash drifts a short way along that tab's hue family; small dot on section headers | Tab hue shifted by a fixed step per section (max +/- 24 degrees) |
| 3. Meaning | "Is this good, careful, or a problem?" | Text, dots and a thin edge on a card, always with a word | Fixed semantic colours, never decorative |
| 4. Content | "What is this game like?" | Game page hero and tile art tint | Derived from the cover; chrome stays neutral glass |

Rules that make it feel designed, not random:
1. **Chrome is neutral.** Cards, rows, buttons and panels are neutral glass (white at 6-14 percent over the dark base).
   They never carry a hue of their own. Hue lives only in the wash, the dots and semantic text.
2. **At most two hues on screen**: the place/section wash and one semantic colour at a time per element.
3. **The focus ring is always white.** It is the one thing that never changes colour (colour-blind safe, constant).
4. **Semantic colours are reserved.** Green, amber and red/pink are never used as ambient or for decoration.
   Blue is the "selected / changed / active" accent and is also the family of the ambient hues, so it is distinguishable
   by shape (a bar or dot, always with a label), not by hue alone.
5. **Every coloured thing has a text label.** Colour reinforces; it is never the only signal.

## Layer 1: place (tab hues, cool family 190 to 265 degrees)
| Tab | Hue | Meaning of the choice |
|---|---|---|
| Store | indigo 245 | discovery, the "front of shop" |
| Downloads | teal 190 | flow and activity (water, in motion) |
| Library | violet 270 | "yours", things you own |
| Files | slate-cyan 205 | utility, neutral tooling |
| Sources | blue 225 | where things come from |
| Settings | cobalt 235 | configuration, calm and precise |
| Logs | grey-blue 215, low chroma | technical, quiet |

Wash shape: vertical gradient, alpha 0.20 at the very top fading to 0 by 45 percent of the height, plus a faint
radial bloom behind the page title (alpha 0.10). It never goes under body text lines above 0.12 alpha (contrast >= 4.5:1
for text/2 on the worst case). The base remains `night0..night2`.

## Layer 2: section (inside a place)
Used by Settings groups, filter menus and sort menus. Each section header owns a hue = tab hue + step, steps are
fixed by position in the list (0, +12, +24, -12, -24 degrees), not by random assignment, so the same section always
looks the same:

* Settings (cobalt 235): Cover art 235, Store grid 247, Link ranking 259, Popularity 223, About 211.
* Pegasus group: Downloads 235, Accounts 247, DNS and providers 259, Runtime 223.
* Filters (Store tab, indigo 245): Show 245, Region and firmware 257, Format and host 233, Source 269, Sort 221.
* Sort menu: Order 245, Popularity 257, Library state 233.

Section colour appears as: the 8 px dot in the header, the 3 px accent bar on a changed row, and the wash target while
focus is inside that section. Nothing else.

## Layer 3: meaning (semantic palette, fixed)
| Token | Colour | Means | Used for |
|---|---|---|---|
| good | #5ce0bc (existing tier_green) | works / fits / ready | link tier supported, firmware fits, installed, "Tested" Pegasus |
| caution | #f4c95d (tier_yellow) | needs something extra | debrid needed, browser capture, incomplete set, untested Pegasus version |
| bad | #ff6b9d (tier_red) | will not work / failed | unsupported, failed job, Pegasus unreachable |
| info | #7aa8ff | selected, changed, active filter | filter-on bar, summary strip, selected sort |
| muted | text3 | not applicable | "not installed", unknown |

Semantic colour is applied to the **value text and a 6 px dot**, with at most a 12 percent tint on the card edge
(hairline), never a filled card.

## Layer 4: content tint (game page)
The hero is the cover's own colour, scaled and blurred, under a fixed veil. The veil colour is the Store place hue at
alpha 0.55 so every game page sits in the same family regardless of art. Info cards stay neutral glass.

## Transitions
| Event | Duration | Curve | What moves |
|---|---|---|---|
| Switch tab (L1/R1, tap) | 400 ms | ease-out cubic | wash hue and tab dot crossfade; content slides as today |
| Focus moves to another section in the same screen | 250 ms | ease-out | wash hue drifts to the section hue; header dot pulses once (scale 1.0 to 1.25 to 1.0, 200 ms) |
| Focus moves within a section | none | | colour does not change |
| Semantic state changes (e.g. job fails) | 120 ms | linear | text colour fades; no pulse |
| Open a panel or dialog | 180 ms | ease-out | the screen behind dims to 55 percent, wash keeps its hue |
| Reduced motion (future setting) | 0 ms | | all of the above snap |

Never animate more than one hue at once. A transition in progress is interrupted and retargeted, not queued.

## Accessibility and TV rules
* Minimum contrast 4.5:1 for body text, 3:1 for large text and focus indicators, measured against the worst wash.
* Wash alpha is capped at 0.22 anywhere; TVs (HDR tone mapping, overscan) crush low-alpha gradients, so the gradient
  needs at least 8 bit dithering or a visible 3 percent noise-free band. Check on the console, not only the preview.
* No pure saturated colours above 70 percent HSV value; use the table values.

## Implementation notes (for the agent)
* One header: `theme.hpp`. API sketch: `theme::tab_hue(Tab)`, `theme::section_hue(Place, section_index)`,
  `theme::Semantic {good, caution, bad, info, muted}`, `theme::Wash` (existing class, retargeted by place then
  section), `theme::wash_color(hue, alpha)`, constants for durations.
* Remove per-screen hard-coded colours added by S1/G1/F1 (`theme_game.hpp` constants are replaced by layer 3/4).
* Keep `ui.hpp` tokens untouched except to alias tier colours to the semantic names.
* Unit-test: hue lookup is stable, contrast helper >= 4.5 for text over the worst wash, wash retargeting never
  animates two hues.
