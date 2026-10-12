// Wings TV app - A game's links, folded: sections, releases, mirrors, part sets.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A merged game has a median of 22 links (up to 266). The page shows them as
// sections (Base, Update, DLC, Other) of releases (one package's links that
// share a descriptor: region, fw, kind, version, ...), each with its mirrors:
// one card per host, a single link or a multipart set on that host
// (docs/research/links-grouping.md section 4.3). Pure data: ranking and
// filters are in ranking.hpp and link_facets.hpp.

#pragma once

#include "orbit/link_meta.hpp"
#include "orbit/link_sets.hpp"
#include "orbit/model.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// What the existing queue call takes: Backend download of (packageId, linkIndex).
struct LinkRef
{
    std::string package_id;
    int link_index = 0;
    bool operator==(const LinkRef &o) const
    {
        return link_index == o.link_index && package_id == o.package_id;
    }
};

enum class Section : std::uint8_t
{
    base,   // base and game+dlc ("incl. DLC")
    update,
    dlc,
    other,  // fix, and anything else
};
constexpr int kSectionCount = 4;
const char *section_label(Section section); // "Game", "Updates", "DLC", "Other" (English keys)
Section section_of(LinkKind kind);

struct GamePackage
{
    std::string id;
    std::string title; // its own title: where "Deluxe Edition" or "(DLC)" shows
    std::string source_name;
    std::string version;
    double size_bytes = 0.0;
    int source_order = 0; // position of its source in the page's source order
};

struct GameLink
{
    int package = 0; // index into GamePage::packages
    int index = 0;   // Pegasus linkIndex
    std::string name;
    std::string url; // PackageLink::url (empty when the backend does not give it)
    LinkMeta meta;
    LinkTier tier = LinkTier::unknown;
    bool needs_browser = false;
    std::vector<std::string> services; // debrid services that take it
    // The same URL listed elsewhere in the game (DLPS reposts Pippo links):
    // shown once, every copy kept.
    bool duplicate = false;      // another link shows this URL; this one is hidden
    std::vector<LinkRef> copies; // on the shown link: the hidden copies
};

struct Mirror
{
    int release = 0;       // index into GamePage::releases
    std::vector<int> links; // indexes into GamePage::links, download (catalogue) order
    std::string host;
    PartStatus status = PartStatus::single;
    std::vector<int> numbers;           // part numbers (parts only)
    std::vector<std::vector<int>> runs; // where the numbering restarts
    std::vector<int> missing;           // gaps: the missing numbers
    LinkTier tier = LinkTier::unknown;  // the worst tier of its links
    bool needs_browser = false;         // any of its links opens the PS5 browser

    bool multipart() const
    {
        return status != PartStatus::single;
    }
    int parts() const
    {
        return multipart() ? static_cast<int>(links.size()) : 0;
    }
    // "8 parts (1-8)", "14 links (parts 1-10, 1-4)", "6 parts (numbers cut off)", "1 file".
    std::string parts_text() const;
    // The warning line: "numbering restarts: these may be two different archives",
    // "part 4 missing", "numbers cut off in the catalogue", or "".
    std::string warning() const;
};

struct GamePage;

struct Release
{
    int package = 0;     // index into GamePage::packages
    std::string key;     // release_key()
    LinkMeta meta;       // of its first link: region, fw, kind, version, packager, tags, label
    Section section = Section::base;
    std::vector<int> mirrors; // indexes into GamePage::mirrors; rank_page() sorts them best first
    // "USA · 12.xx · Base · v01.055 · DLPS"
    std::string header(const GamePage &page) const;
    // "@Psykzhack · APR-EMU · Deluxe Edition", or "".
    std::string subheader() const;
};

struct GamePage
{
    std::vector<GamePackage> packages;
    std::vector<GameLink> links;
    std::vector<Mirror> mirrors;
    std::vector<Release> releases;
    std::vector<int> sections[kSectionCount]; // release indexes per section; rank_page() orders them
    int unique_links = 0;    // links shown (after URL de-duplication)
    int duplicate_links = 0; // links hidden as copies of a shown URL

    // The best mirror of the Base section and why (filled by rank_page()); -1 none.
    int best = -1;
    std::vector<std::string> best_chips; // "Debrid", "USA", "BP 4.xx", "exFAT", "1 file"
    std::string best_why;                // "debrid: fast, no captcha, runs on your 10.xx, ..."

    // The (packageId, linkIndex) to queue for one mirror: "Download all parts"
    // (or the one link), in catalogue order, skipping what `skip` says is
    // already queued or done.
    std::vector<LinkRef> queue_refs(int mirror,
                                    const std::function<bool(const LinkRef &)> &skip = {}) const;
    // The regions of all shown links ("?" never listed), first-seen order.
    std::vector<std::string> regions() const;
};

// One PackageDetail per package of the game, in the game's package order.
// `source_order` lists source names in the user's order (others follow, A to Z).
GamePage build_game_page(const std::vector<PackageDetail> &details,
                         const std::vector<std::string> &source_order = {});

} // namespace orbit
