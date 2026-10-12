// Wings TV app - Popularity data: the index, the local download counter, the hub and the fetch service.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The data is a small JSON file Wings fetches over https (docs/research/popularity.md):
//   {schemaVersion:1, generatedAt, sources[], attribution{}, titles{titleId:{score, rating?,
//    ratingCount?, sources[], confidence, hero?, heroLow?}}}
// On by default (the player can turn it off). Nothing here touches the network on its own: PopularityService asks a
// DocFetcher (main.cpp wires it to the cover-art HTTPS client) and only when the Wings
// preference is on. Everything else is platform neutral and tested on the host.

#pragma once

#include "orbit/catalogue_view.hpp"
#include "orbit/game_group.hpp"
#include "orbit/thread.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

constexpr std::size_t kPopularityMaxBytes = 512u * 1024u;
constexpr double kPopularityMinConfidence = 0.6; // records below this are hidden
constexpr const char *kPopularityDefaultUrl = "https://larazadelsol.github.io/wings-app/popularity.v1.json";

struct PopRecord
{
    int score = 0;        // 0..100
    int rating = -1;      // 0..100, -1 unknown
    long rating_count = -1;
    double confidence = 0.0;
    std::vector<std::string> sources;
    std::string hero;     // optional https:// art URL for the game page header ("" none; game_header.hpp)
    std::string hero_low; // optional small, fast ~16:9 https:// URL shown until the full hero is there
};

class PopularityIndex
{
  public:
    // Schema check: schemaVersion 1, titles object, every kept record with a 0..100 score and a
    // 0..1 confidence. Oversize text (> kPopularityMaxBytes) is rejected. Records that are
    // malformed are skipped; no record at all is an error.
    static bool parse(std::string_view text, PopularityIndex *out, std::string *error);

    const PopRecord *find(const std::string &title_id) const; // raw, ignores confidence
    // The game's record: the visible one (confidence >= kPopularityMinConfidence) with the highest
    // confidence over its title ids (ties: more ratings, then the smaller title id). Null: unknown.
    const PopRecord *best(const std::vector<std::string> &title_ids) const;
    // The hero art URL of a game: the first of its title ids whose record has one (confidence is not
    // asked: art is not a ranking claim). "" when none does.
    std::string hero(const std::vector<std::string> &title_ids) const;
    // The same for the small "heroLow" image.
    std::string hero_low(const std::vector<std::string> &title_ids) const;
    std::size_t size() const { return records_.size(); }
    const std::string &generated_at() const { return generated_at_; }
    const std::vector<std::string> &attribution() const { return attribution_; } // one line per source

  private:
    std::map<std::string, PopRecord> records_;
    std::string generated_at_;
    std::vector<std::string> attribution_;
};

// "Only https://": the source URL is refused otherwise (also credentials, spaces, > 300 bytes).
bool valid_popularity_url(std::string_view url);
// Writes `data` to every path (creating the folder); `written` counts the files that worked. Shared with editorial.cpp.
void write_everywhere(const std::vector<std::string> &paths, const std::string &data, int *written);
// 4300 -> "4.3k", 1200000 -> "1.2M", 950 -> "950".
std::string short_count(long n);
// "Popularity 87 · Rating 92 (4.3k)"; "" for a null record.
std::string popularity_line(const PopRecord *record);
// The attribution shown in Settings and on the page: the file's lines, or the three
// source names when the index is not loaded.
std::string attribution_text(const PopularityIndex *index);

// This app's own queued downloads per title id, persisted (small versioned file).
class LocalDownloads
{
  public:
    // One game queued once: every one of its title ids counts.
    void add(const std::vector<std::string> &title_ids);
    long count(const std::vector<std::string> &title_ids) const; // the largest over the ids
    std::size_t size() const { return counts_.size(); }
    std::string encode() const;
    bool decode(std::string_view payload);
    bool load(const std::vector<std::string> &paths);
    int save(const std::vector<std::string> &paths) const; // files written

  private:
    std::map<std::string, long> counts_;
};

struct PopularityView
{
    const PopularityIndex *index = nullptr;
    const LocalDownloads *downloads = nullptr;
};

bool is_popularity_sort(SortMode mode);
// The sort the grid really applies: a popularity order whose data is missing falls back to A to Z.
SortMode effective_sort(SortMode mode, const PopularityView *view);
// The sort key of a game for `mode`: larger first, -1 = no data (sorts last).
long popularity_sort_key(SortMode mode, const PopularityView *view, const std::vector<std::string> &title_ids);
// The orders the Utilities panel offers: the basic ones, plus the popularity ones when the
// index is loaded and the preference is on, plus "Your most downloaded" once something was counted.
std::vector<SortMode> available_sorts(const PopularityView *view);

struct PopularityStatus
{
    std::string message;      // "Updated ...", "Could not refresh: ..." (never a popup)
    std::int64_t updated = 0; // unix time of the data in use, 0 none
    std::size_t count = 0;    // titles in use
    bool busy = false;
};

// Shared by the worker (index, status) and the frame thread (everything else).
class PopularityHub
{
  public:
    void set_enabled(bool on);
    bool enabled() const;
    void set_index(std::shared_ptr<const PopularityIndex> index); // null: forget
    // The index while the preference is on; null otherwise.
    std::shared_ptr<const PopularityIndex> index() const;
    void set_status(const PopularityStatus &status);
    PopularityStatus status() const;
    std::uint64_t serial() const; // changes with the index, the switch and the counter
    LocalDownloads &downloads() { return downloads_; }
    const LocalDownloads &downloads() const { return downloads_; }
    void touch(); // after a counter change
    void set_downloads_paths(std::vector<std::string> paths);
    // Frame thread: counts one download of the game and saves.
    void count_download(const std::vector<std::string> &title_ids);
    // Settings calls these; the app wires the hooks to its PopularityService (none in the
    // preview and in tests: then only the switch changes).
    void apply_prefs(bool on, const std::string &url)
    {
        set_enabled(on);
        if (apply_hook)
            apply_hook(on, url);
    }
    void request_refresh()
    {
        if (refresh_hook)
            refresh_hook();
    }
    std::function<void(bool, const std::string &)> apply_hook;
    std::function<void()> refresh_hook;

  private:
    mutable Mutex mutex_;
    bool enabled_ = false;
    std::shared_ptr<const PopularityIndex> index_;
    PopularityStatus status_;
    std::uint64_t serial_ = 0;
    LocalDownloads downloads_;
    std::vector<std::string> downloads_paths_;
};
PopularityHub &popularity_hub();

struct DocResult
{
    int status = 0; // HTTP status, 0 when nothing completed
    std::string body;
    std::string error; // transport failure sentence
};

class DocFetcher
{
  public:
    virtual ~DocFetcher() = default;
    virtual DocResult get(const std::string &url, std::size_t max_bytes) = 0; // worker thread; blocks
};

struct PopularityServiceConfig
{
    std::vector<std::string> cache_paths; // first writable wins; the saved copy of the file
    int ttl_s = 24 * 3600;                // no ETag support in the client: re-fetch only when older
    // Which build of the app this is ("00.001.001+20261012-203105"). The saved copy carries the tag of the build
    // that fetched it; a copy from another build (or from before the tag existed) is fetched again at the first
    // start, whatever its age, so updating the app always updates the data too. Empty: not asked (tests, preview).
    std::string app_tag;
    std::function<std::int64_t()> now;    // unix seconds (default: the system clock)
};

// Runs the cache load and the fetch on a background thread. One job at a time.
class PopularityService
{
  public:
    PopularityService(PopularityHub &hub, DocFetcher &fetcher, PopularityServiceConfig config);
    ~PopularityService();
    // Frame thread. on=false: the hub is switched off and nothing is fetched. on=true: loads the
    // cache and fetches when it is missing or older than the TTL. Only acts on a change.
    void apply(bool enabled, const std::string &url);
    // Settings > Refresh: fetch now (only when on). Ignored while a job runs.
    void refresh();
    void wait_idle(); // tests

  private:
    void start_job(bool force);
    void run(bool force, std::string url);
    PopularityHub &hub_;
    DocFetcher &fetcher_;
    PopularityServiceConfig config_;
    Thread thread_;
    bool enabled_ = false;
    bool applied_ = false;
    std::string url_;
};

} // namespace orbit
