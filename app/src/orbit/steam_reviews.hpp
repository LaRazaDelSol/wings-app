// Wings TV app - Live Steam review fallback for games missing from the popularity index (backlog B-1, option 1).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keyless and lazy: only when a game page opens, only when the popularity preference is on and the index has no
// record for the game, one request at a time, rate limited, cached per title id. Two public endpoints:
//   store.steampowered.com/api/storesearch/?term=..&cc=us&l=en      (undocumented; id + name of apps)
//   store.steampowered.com/appreviews/<appid>?json=1&num_per_page=0 (query_summary: positive / negative counts)
// The answer is a percentage and a count, shown as "Steam reviews, live" and never as the 0-100 score used for
// sorting. Matching accepts only one app whose normalised name equals the title's (same idea as
// wings-popularity/matcher.py, without the fuzzy part: no guess). Everything except SteamReviewService's thread is
// pure and host tested.

#pragma once

#include "orbit/popularity.hpp"
#include "orbit/thread.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

constexpr std::size_t kSteamSearchMaxBytes = 64u * 1024u;
constexpr std::size_t kSteamReviewsMaxBytes = 32u * 1024u;
constexpr long kSteamMinReviews = 10;                // fewer than this is noise: treated as "no data"
constexpr std::int64_t kSteamOkTtlS = 7 * 24 * 3600; // a good answer
constexpr std::int64_t kSteamFailTtlS = 24 * 3600;   // no match / no reviews / HTTP error: ask again after a day
constexpr std::int64_t kSteamNetBackoffS = 10 * 60;  // no answer at all (offline): this session only, not saved
constexpr double kSteamMinIntervalS = 2.0;           // between two requests
constexpr std::size_t kSteamMaxEntries = 400;

// Comparison key of a title: lower case ASCII words, "the" / format words / edition words removed.
struct NormTitle
{
    std::string key;      // "elden ring"
    std::string editions; // sorted edition words joined by a space ("deluxe remastered")
};
NormTitle normalize_title(std::string_view title);

struct SteamCandidate
{
    long appid = 0;
    std::string name;
};
struct SteamSummary
{
    int rating = -1; // percent positive 0..100
    long count = 0;  // positive + negative
};

std::string steam_search_url(std::string_view title);
std::string steam_reviews_url(long appid);
// type "app" items with a positive id and a name; false when the document is not the expected shape.
bool parse_store_search(std::string_view text, std::vector<SteamCandidate> *out);
// Exactly one candidate whose normalised key and edition set equal the title's. Anything else: false.
bool choose_appid(std::string_view title, const std::vector<SteamCandidate> &candidates, long *appid);
// success == 1 and at least kSteamMinReviews positive + negative votes.
bool parse_review_summary(std::string_view text, SteamSummary *out);
// "Steam reviews, live: 92% positive (4.3k)"
std::string live_rating_line(const SteamSummary &summary);

struct LiveEntry
{
    bool ok = false;
    SteamSummary summary;
    long appid = 0;
    std::int64_t fetched = 0;
};

// Per title id answers with expiry. Thread safe (worker writes, frame thread reads).
class LiveReviewCache
{
  public:
    enum class State : std::uint8_t
    {
        unknown, // never asked or expired: ask
        good,    // `summary` is valid
        failed,  // asked recently without a usable answer: do not ask
    };
    State state(const std::string &title_id, std::int64_t now, SteamSummary *summary = nullptr) const;
    void put_ok(const std::string &title_id, const SteamSummary &summary, long appid, std::int64_t now);
    void put_fail(const std::string &title_id, std::int64_t now);
    std::size_t size() const;
    std::string encode() const; // "id|ok|rating|count|appid|fetched" lines
    bool decode(std::string_view payload);
    bool load(const std::vector<std::string> &paths);
    int save(const std::vector<std::string> &paths) const;

  private:
    void trim_locked();
    mutable Mutex mutex_;
    std::map<std::string, LiveEntry> entries_;
};

// At most one request per interval. now is any monotonic seconds value.
class RateLimiter
{
  public:
    explicit RateLimiter(double min_interval_s) : interval_(min_interval_s) {}
    double wait_needed(double now) const; // seconds to wait before the next request (0 = go)
    void note_request(double now)
    {
        last_ = now;
        used_ = true;
    }

  private:
    double interval_;
    double last_ = 0.0;
    bool used_ = false;
};

struct SteamReviewConfig
{
    std::vector<std::string> cache_paths;
    std::function<std::int64_t()> now;   // unix seconds (default: the system clock)
    std::function<double()> mono;        // monotonic seconds (default: now_seconds)
    std::function<void(double)> sleep_s; // default usleep; tests record instead
};

// The frame thread asks, one worker thread answers. The hub supplies the preference switch and the index.
class SteamReviewService
{
  public:
    SteamReviewService(PopularityHub &hub, DocFetcher &fetcher, SteamReviewConfig config);
    ~SteamReviewService();
    // Frame thread, cheap and idempotent: starts a job when the preference is on, the index is loaded and has no
    // usable record for any of the ids, the cache has no answer for them and no job runs. True when one started.
    bool request(const std::vector<std::string> &title_ids, const std::string &title);
    // The cached live answer for any of the ids (good, not expired). Does not consult the switch.
    bool lookup(const std::vector<std::string> &title_ids, SteamSummary *out) const;
    void wait_idle(); // tests
    const LiveReviewCache &cache() const { return cache_; }

  private:
    void run(std::vector<std::string> ids, std::string title);
    PopularityHub &hub_;
    DocFetcher &fetcher_;
    SteamReviewConfig config_;
    LiveReviewCache cache_;
    RateLimiter limiter_;
    Thread thread_;
    std::atomic<bool> idle_{true};
    bool loaded_ = false;
    std::map<std::string, std::int64_t> backoff_; // frame thread only: title id -> retry after (unix s)
};

// The service the app wired (null in the preview and tests): the game page asks it, never owns it.
SteamReviewService *steam_reviews();
void set_steam_reviews(SteamReviewService *service);

} // namespace orbit
