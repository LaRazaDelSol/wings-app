// Wings TV app - Bad-link flags on this console: links that failed for the player.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a link fails (host error, file removed, wrong file, password never
// works, broken archive) Wings flags it here, or the player marks it by hand.
// A flag is keyed by link_id(): a hash of the title id and the normalised URL
// (the raw URL is never stored), keeps its reason and date, and expires after
// kFlagDays so a link comes back. Best never picks a flagged link while
// another exists (ranking.hpp). Saved as "link_flags.v1" in the checked save
// container of core/save_file.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

struct GamePage;

enum class FlagReason : std::uint8_t
{
    none = 0,       // no flag / not a link failure
    host_error,     // the provider or host refused or errored
    removed,        // 404, file deleted or taken down
    wrong_file,     // not the game, or nothing to install
    password,       // no password worked
    broken_archive, // CRC error, damaged or incomplete archive
    by_hand,        // the player marked it bad
};
constexpr int kFlagReasonCount = 7;

// "host error", "file removed", "wrong file", "password never worked",
// "broken archive", "marked bad by you"; "" for none.
const char *flag_reason_text(FlagReason reason);

constexpr std::size_t kMaxFlags = 500;          // oldest dropped beyond this
constexpr std::int64_t kFlagDays = 60;          // a flag older than this is gone
constexpr std::int64_t kFlagSeconds = kFlagDays * 86400;

// The URL as compared: scheme and host lower case, "www." and default ports
// dropped, no fragment, no tracking or expiring query parameters (utm_*,
// fbclid, token, expires, signature, X-Amz-*, ...). Path and other
// parameters kept as given.
std::string normalise_link_url(std::string_view url);

// 16 hex digits: FNV-1a 64 of "<TITLE ID upper>|<normalised url>". An empty
// URL gives "".
std::string link_id(std::string_view url, std::string_view title_id);

// The reason a failure means, from Pegasus' error text and/or an HTTP status
// (0 = none). Cancelled, network or disk problems on our side give none.
FlagReason reason_from_error(std::string_view error, int http_status = 0);

struct LinkFlag
{
    FlagReason reason = FlagReason::none;
    std::int64_t when = 0; // unix seconds
    std::string title;     // the game's name for Settings > Wings ("" unknown)
};

class LinkFlags
{
  public:
    // Adds or replaces a flag (the newest reason wins); none is ignored.
    // Drops the oldest beyond kMaxFlags.
    // `title` is the game's name shown in Settings (one line, cut at 80 bytes;
    // empty keeps the name an earlier flag of the same id had).
    void add(const std::string &id, FlagReason reason, std::int64_t now, std::string_view title = {});
    bool clear(const std::string &id); // false if it was not flagged
    void clear_all();
    // The live flag, or nullptr (expired flags count as absent).
    const LinkFlag *find(const std::string &id, std::int64_t now) const;
    bool flagged(const std::string &id, std::int64_t now) const
    {
        return find(id, now) != nullptr;
    }
    // Forgets flags older than kFlagDays; returns how many went.
    std::size_t expire(std::int64_t now);
    std::size_t size() const
    {
        return flags_.size();
    }
    // Newest first, for Settings > Wings.
    std::vector<std::pair<std::string, LinkFlag>> list(std::int64_t now) const;

    // "<id> <reason> <when>\n" per flag.
    std::string encode() const;
    bool decode(std::string_view payload);
    // First readable path wins; false if none.
    bool load(const std::vector<std::string> &paths);
    int save(const std::vector<std::string> &paths) const; // files written

  private:
    std::map<std::string, LinkFlag> flags_;
};

// "14 Oct" (UTC).
std::string flag_date(std::int64_t when);
// "Failed for you on 14 Oct: file removed"; "Marked bad on 14 Oct" by hand.
std::string flag_line(const LinkFlag &flag);

// Fills GameLink::flag of every link of the page from `flags` (title id of
// the game; links without a URL are keyed by "pkg:<id>/<index>").
void apply_link_flags(GamePage *page, const LinkFlags &flags, std::string_view title_id, std::int64_t now);
// The id of one link of a page, as apply_link_flags() keys it.
std::string page_link_id(const GamePage &page, int link, std::string_view title_id);

// The app-wide flags of this console (main.cpp sets them, frame thread only);
// nullptr until set. save_link_flags() writes them to the paths given here
// (none: nothing written) and is called after every change.
LinkFlags *link_flags();
void set_link_flags(LinkFlags *flags, std::vector<std::string> paths);
void save_link_flags();
// Flags (or clears, reason none) the given links of the page in the app-wide
// flags, saves, and refreshes the page's flag fields. No-op without flags.
void mark_page_links(GamePage *page, const std::vector<int> &links, FlagReason reason, std::string_view title_id,
                     std::int64_t now, std::string_view title = {});

} // namespace orbit
