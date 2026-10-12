// Wings TV app - "Best": which mirror to download, and why.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The order of docs/research/links-grouping.md section 4.5, lowest key first:
//   1. too new for my firmware (never Best while anything else exists),
//   2. tier: native .pkg, debrid, automated, browser capture, unknown, unsupported
//      ("prefer debrid" off: debrid between automated and browser),
//   3. firmware fit: runs, unknown,
//   4. region: preferred (or no preference), unknown, other,
//   5. format: preferred (or no preference), other,
//   6. parts: single, complete set, restarts/unnumbered, gaps; then fewer parts,
//   7. package size known first,
//   8. newest version (link, else package); unknown versions after known ones,
//   9. source order, then link index (stable).
// LinkTier and its helpers in model.hpp are unchanged (colours, grid); this
// order is for "Best" only.

#pragma once

#include "orbit/game_group.hpp"
#include "orbit/game_page.hpp"
#include "orbit/link_meta.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orbit
{

// Settings > Wings > Downloads.
struct LinkPrefs
{
    std::string firmware; // the console's: "10.xx" or "10.01"; empty = not set (shown, not ranked)
    std::string region;   // "USA", "EUR", ...; empty = any
    LinkFormat format = LinkFormat::none; // none = any
    bool prefer_debrid = true;
    bool prefer_single_file = true;
    std::vector<std::string> hidden_hosts; // host labels (LinkMeta::host) never offered
};

// 0 native .pkg, 1 debrid, 2 automated, 3 browser capture, 4 unknown, 5 unsupported.
int tier_rank(LinkTier tier);
// "PKG to PS5", "Debrid", "Automatic", "Browser", "Unknown host", "Not supported".
const char *best_tier_label(LinkTier tier);

enum class FwFit : std::uint8_t
{
    runs,    // the link's minimum firmware is at or below the console's (or no firmware set)
    unknown, // the link does not say
    too_new, // it needs a newer firmware than the console's
};
// `link_fw` is LinkMeta::min_fw(); `console_fw` LinkPrefs::firmware.
FwFit fw_fit(std::string_view link_fw, std::string_view console_fw);

// One thing to rank: a single link or a whole multipart set on one host.
struct Candidate
{
    LinkTier tier = LinkTier::unknown;
    std::string fw; // effective minimum firmware (LinkMeta::min_fw())
    bool backport = false;
    std::string region;
    LinkFormat format = LinkFormat::none;
    int parts = 0; // 0 single file, else the number of parts
    PartStatus status = PartStatus::single;
    bool size_known = false;
    std::string version; // link version, else package version
    int source_order = 0;
    int index = 0; // link index (first part)
    std::string host;
};

struct Ranking
{
    std::vector<int> order; // candidate indexes, best first (hidden hosts left out)
    int best = -1;          // order.front(), or -1
    std::vector<std::string> chips; // "Debrid", "USA", "BP 4.xx", "exFAT", "1 file"
    std::string why;        // "debrid: fast, no captcha, runs on your 10.xx, your region, single file"
    int too_new = 0;        // candidates that need a newer firmware than the console's
    // "Debrid · USA · 12.xx · exFAT · 1 file" (separator as given).
    std::string summary(const char *separator = " \xC2\xB7 ") const;
    // "Best: Debrid · USA · 12.xx · 1 file — debrid: fast, no captcha, ..."
    std::string text() const;
};

// rank_links(): the research's Best over any candidates.
Ranking rank_links(const std::vector<Candidate> &candidates, const LinkPrefs &prefs);

// The candidate for one mirror of a page.
Candidate candidate_of(const GamePage &page, int mirror);

// Ranks the page: mirrors inside each release, releases inside each section
// (by their best mirror, then newest version, then source order), and the
// page's Best over the Base section (GamePage::best, best_chips, best_why).
void rank_page(GamePage *page, const LinkPrefs &prefs);

// The grid's tier for a game: the best of its packages' tiers (catalogue_view's
// tier_of(): Snapshot::tiers, else the package flags) in the order above, as a
// packed byte (model.hpp pack_tier); "via debrid" when any package has a debrid link.
std::uint8_t game_tier(const GameGroup &game, const std::vector<Package> &packages,
                       const std::vector<std::uint8_t> &package_tiers, const LinkPrefs &prefs);

} // namespace orbit
