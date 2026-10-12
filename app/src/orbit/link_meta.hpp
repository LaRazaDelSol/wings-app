// Wings TV app - What a download link's name says: region, firmware, kind, format, parts.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A catalogue link has only an index, a name and a URL. The names follow four
// layouts (one per source family, see docs/research/links-grouping.md section 1)
// and carry most of what the player needs to choose: "USA 12.xx Base - Rootz
// part 3", "EUR exFAT 01.000 Base - VIKI (exFAT)", "Fpkg - VikingFile",
// "Akirabox - 4.xx+ - pt.5". parse_link() is a step-by-step port of the
// reference parser in that document (tested there over all 35,559 links of
// recon/samples/library.json) and is pure: no allocation per character, no
// regex, no locale.
//
// The name-derived host is a label only. Downloads always follow the URL.

#pragma once

#include "orbit/model.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

// Pegasus stores link names in 63 UTF-8 bytes; longer names are cut there.
constexpr std::size_t kLinkNameBytes = 63;

enum class LinkKind : std::uint8_t
{
    base,     // the game ("Base", "Game", or no kind word at all)
    game_dlc, // the game with DLC included ("Game (v1.0)(DLC)", "BASE GAME + DLC")
    dlc,      // DLC only ("DLC", "Only DLCs", "DLC Unlocker", a named DLC)
    update,   // "Update 01.003"
    fix,      // Pippo "Fix"
};

enum class LinkFormat : std::uint8_t
{
    none, // not stated (85% of links)
    exfat,
    fpkg,
    pkg,
    dump,
    ffpfs,
};

enum class HostSource : std::uint8_t
{
    none, // neither the name nor the URL says
    name, // from the name: the URL is a wrapper (decrypt page, link vault) or unknown
    url,  // from the URL host
};

struct LinkMeta
{
    std::string region;    // "USA" "EUR" "JPN" "ASIA" "HK" "KOR", or empty
    std::string lang_from; // "USA to EUR Languages": "USA" / "EUR"
    std::string lang_to;
    std::string fw;        // the build's firmware as written: "12.xx", "7.61"; empty when unknown
    bool fw_plus = false;  // "4.xx+": explicitly "this and up"
    bool backport = false;
    std::string backport_target; // "4.xx": the lowest firmware a backport runs on; empty when unknown
    LinkKind kind = LinkKind::base;
    bool kind_inferred = false; // no kind word: base by default
    bool all_dlc = false;       // "All DLCs", "DLCs Merged"
    LinkFormat format = LinkFormat::none;
    bool standard_build = false; // "Standard", "Std"
    std::string version;         // "01.055", "1.008.000" (no leading v)
    int release = -1;            // "Release 2": 2; -1 when absent
    int part = -1;               // "part 5", "pt.5": 5; -1 when absent
    bool part_unknown = false;   // "... - Akirabox pa": a part whose number was cut off
    bool truncated = false;      // the name is 63+ bytes: a trailing number may be cut
    std::vector<std::string> tags; // "APR-EMU", "EMU", "+Arabic"
    std::string packager;          // "Psykzhack", "DUPLEX", "Pippo"
    std::string host;              // canonical label: "Akirabox", "VikingFile", or the URL host
    HostSource host_from = HostSource::none;
    bool wrapped = false;  // the URL is a wrapper (decrypt page, link vault, shortener): host is the name's
    std::string size_text; // "95.9 GB" (one family of names only)
    std::string title_id;  // "PPSA26344" embedded in the name (one family only)
    std::string label;     // free text of a DLC: "Deluxe Edition", "Multiplayer"
    std::string residue;   // anything the grammar did not explain (empty over the whole sample)

    bool has_part() const
    {
        return part >= 0 || part_unknown;
    }
    // The lowest firmware the link is said to run on: the backport target, else the build fw.
    const std::string &min_fw() const
    {
        return backport_target.empty() ? fw : backport_target;
    }
};

// The URL host as Wings stores it (PackageLink::host: lower case, no "www."),
// may be empty. Never fails; unknown text ends up in residue.
LinkMeta parse_link(std::string_view name, std::string_view url_host);

// True for hosts that only wrap another host (the Pippo decrypt page, link-vault,
// filecrypt, URL shorteners): the real host is the one the name gives.
bool is_wrapper_host(std::string_view url_host);

// Everything that identifies a release except host and part, as one comparable key.
std::string release_key(const LinkMeta &meta);

// ---- firmware ----
// Firmware as hundredths: what a link needs, and what a console runs.
// "10.xx" needs 10.00 (1000); a console on "10.xx" counts as 10.99 (1099).
// "7.61" is 761 either way. -1 for empty or unreadable text.
int fw_need(std::string_view fw);
int fw_console(std::string_view fw);
// "12.xx" -> 12, "7.61" -> 7; -1 when unknown.
int fw_family(std::string_view fw);

// ---- labels (English keys, translated with tr()) ----
const char *kind_label(LinkKind kind);     // "Base", "Game + DLC", "DLC", "Update", "Fix"
const char *format_label(LinkFormat format); // "exFAT", "FPKG", "PKG", "Dump", "FFPFS", ""

// "01.055" -> {1, 55}; empty for no version. Compare with operator< (lexicographic).
std::vector<int> version_parts(std::string_view version);

} // namespace orbit
