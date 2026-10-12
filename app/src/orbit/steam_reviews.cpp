// Wings TV app - Live Steam review fallback (see steam_reviews.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/steam_reviews.hpp"

#include "core/save_file.hpp"
#include "orbit/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

#include <unistd.h>

namespace orbit
{
namespace
{

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool in_list(const std::string &w, std::initializer_list<const char *> list)
{
    for (const char *s : list)
        if (w == s)
            return true;
    return false;
}

bool is_edition_word(const std::string &w)
{
    return in_list(w, {"remastered", "remaster", "remake", "deluxe", "ultimate", "definitive", "complete", "goty",
                       "edition", "collection", "directors", "cut", "final", "enhanced", "anniversary", "premium",
                       "standard", "gold", "legendary", "special", "digital", "bundle", "upgrade"});
}

bool is_format_word(const std::string &w)
{
    return in_list(w, {"usa", "eur", "jpn", "asia", "backport", "fpkg", "pkg", "update", "dlc", "demo", "exfat", "ps5",
                       "ps4", "ppsa", "cusa", "patch", "repack", "multi", "region", "free"});
}

// "v1.05", "1.000", "v1.2.3": dropped whole (matcher.py's version pattern).
bool is_version_token(const std::string &w)
{
    std::size_t i = 0;
    if (i < w.size() && w[i] == 'v')
        ++i;
    int groups = 0;
    while (i < w.size())
    {
        const std::size_t start = i;
        while (i < w.size() && is_digit(w[i]))
            ++i;
        if (i == start)
            return false;
        ++groups;
        if (i < w.size())
        {
            if (w[i] != '.')
                return false;
            ++i;
            if (i == w.size())
                return false;
        }
    }
    return groups >= 2;
}

std::string roman(const std::string &w)
{
    static const char *const from[] = {"ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};
    static const char *const to[] = {"2", "3", "4", "5", "6", "7", "8", "9"};
    for (int i = 0; i < 8; ++i)
        if (w == from[i])
            return to[i];
    return w;
}

bool byte_is(std::string_view s, std::size_t i, unsigned char value)
{
    return i < s.size() && static_cast<unsigned char>(s[i]) == value;
}

// Removes [..] and (..) groups, @packager names and the (R) (C) TM signs; keeps everything else.
std::string strip_noise(std::string_view in)
{
    std::string out;
    int depth = 0;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '[' || c == '(')
        {
            ++depth;
            out += ' ';
        }
        else if (c == ']' || c == ')')
        {
            if (depth > 0)
                --depth;
            out += ' ';
        }
        else if (depth > 0)
            continue;
        else if (c == '@')
        {
            while (i + 1 < in.size() && (std::isalnum(static_cast<unsigned char>(in[i + 1])) || in[i + 1] == '_'))
                ++i;
            out += ' ';
        }
        else if (c == 0xC2 && (byte_is(in, i + 1, 0xAE) || byte_is(in, i + 1, 0xA9)))
            ++i;
        else if (c == 0xE2 && byte_is(in, i + 1, 0x84) && byte_is(in, i + 2, 0xA2))
            i += 2;
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::vector<std::string> words(std::string_view title)
{
    const std::string clean = strip_noise(title);
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&]
    {
        if (!cur.empty())
            out.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < clean.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(clean[i]);
        if (c == '&')
        {
            flush();
            out.push_back("and");
        }
        else if (c == '\'' || c == '`')
            continue;
        else if (c == 0xE2 && byte_is(clean, i + 1, 0x80) && byte_is(clean, i + 2, 0x99))
            i += 2; // curly apostrophe
        else if (c < 0x80 && std::isalnum(c))
            cur += static_cast<char>(std::tolower(c));
        else if (c == '.' && !cur.empty() && is_digit(cur.back()) && i + 1 < clean.size() && is_digit(clean[i + 1]))
            cur += '.'; // "1.05" stays one token so the version check sees it
        else
            flush();
    }
    flush();
    return out;
}

std::string percent_encode(std::string_view s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string search_term(std::string_view title)
{
    std::string out;
    for (const std::string &w : words(title))
        if (!is_version_token(w))
            out += (out.empty() ? "" : " ") + w;
    return out;
}

} // namespace

// ---- matching ----

NormTitle normalize_title(std::string_view title)
{
    std::vector<std::string> toks;
    for (std::string &w : words(title))
        if (!is_version_token(w) && !is_format_word(w))
            toks.push_back(std::move(w));
    if (!toks.empty() && toks[0] == "the")
        toks.erase(toks.begin());
    for (std::size_t i = 1; i < toks.size(); ++i)
        toks[i] = roman(toks[i]);
    std::vector<std::string> core, editions;
    for (const std::string &w : toks)
        (is_edition_word(w) ? editions : core).push_back(w);
    if (core.empty())
        core = toks;
    std::sort(editions.begin(), editions.end());
    editions.erase(std::unique(editions.begin(), editions.end()), editions.end());
    NormTitle out;
    for (const std::string &w : core)
        out.key += (out.key.empty() ? "" : " ") + w;
    for (const std::string &w : editions)
        out.editions += (out.editions.empty() ? "" : " ") + w;
    return out;
}

std::string steam_search_url(std::string_view title)
{
    return "https://store.steampowered.com/api/storesearch/?term=" + percent_encode(search_term(title)) +
           "&cc=us&l=en";
}

std::string steam_reviews_url(long appid)
{
    return "https://store.steampowered.com/appreviews/" + std::to_string(appid) +
           "?json=1&num_per_page=0&language=all&purchase_type=all";
}

bool parse_store_search(std::string_view text, std::vector<SteamCandidate> *out)
{
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object() || !root["items"].is_array())
        return false;
    out->clear();
    for (const json::Value &item : root["items"].items())
    {
        if (!item.is_object() || item.text("type") != "app")
            continue;
        const std::int64_t id = item["id"].as_int(0);
        if (id <= 0 || item.text("name").empty())
            continue;
        out->push_back({static_cast<long>(id), item.text("name")});
    }
    return true;
}

bool choose_appid(std::string_view title, const std::vector<SteamCandidate> &candidates, long *appid)
{
    const NormTitle want = normalize_title(title);
    if (want.key.empty())
        return false;
    long found = 0;
    int hits = 0;
    for (const SteamCandidate &c : candidates)
    {
        const NormTitle got = normalize_title(c.name);
        if (got.key == want.key && got.editions == want.editions && (hits == 0 || c.appid != found))
        {
            found = c.appid;
            ++hits;
        }
    }
    if (hits != 1)
        return false; // none, or ambiguous: never a guess
    *appid = found;
    return true;
}

bool parse_review_summary(std::string_view text, SteamSummary *out)
{
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object() || root["success"].as_int(0) != 1)
        return false;
    const json::Value &q = root["query_summary"];
    if (!q.is_object() || !q["total_positive"].is_number() || !q["total_negative"].is_number())
        return false;
    const long pos = static_cast<long>(q["total_positive"].as_int(0));
    const long neg = static_cast<long>(q["total_negative"].as_int(0));
    if (pos < 0 || neg < 0 || pos + neg < kSteamMinReviews)
        return false;
    out->rating = static_cast<int>(std::lround(100.0 * static_cast<double>(pos) / static_cast<double>(pos + neg)));
    out->count = pos + neg;
    return true;
}

std::string live_rating_line(const SteamSummary &s)
{
    return "Steam reviews, live: " + std::to_string(s.rating) + "% positive (" + short_count(s.count) + ")";
}

// ---- cache ----

LiveReviewCache::State LiveReviewCache::state(const std::string &id, std::int64_t now, SteamSummary *summary) const
{
    Lock lock(mutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return State::unknown;
    const LiveEntry &e = it->second;
    if (e.fetched > now) // the clock went back: ask again rather than trust it
        return State::unknown;
    const std::int64_t age = now - e.fetched;
    if (e.ok)
    {
        if (age >= kSteamOkTtlS)
            return State::unknown;
        if (summary != nullptr)
            *summary = e.summary;
        return State::good;
    }
    return age >= kSteamFailTtlS ? State::unknown : State::failed;
}

void LiveReviewCache::put_ok(const std::string &id, const SteamSummary &s, long appid, std::int64_t now)
{
    Lock lock(mutex_);
    entries_[id] = LiveEntry{true, s, appid, now};
    trim_locked();
}

void LiveReviewCache::put_fail(const std::string &id, std::int64_t now)
{
    Lock lock(mutex_);
    entries_[id] = LiveEntry{false, {}, 0, now};
    trim_locked();
}

void LiveReviewCache::trim_locked()
{
    while (entries_.size() > kSteamMaxEntries)
    {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.fetched < oldest->second.fetched)
                oldest = it;
        entries_.erase(oldest);
    }
}

std::size_t LiveReviewCache::size() const
{
    Lock lock(mutex_);
    return entries_.size();
}

std::string LiveReviewCache::encode() const
{
    Lock lock(mutex_);
    std::string out;
    for (const auto &kv : entries_)
    {
        const LiveEntry &e = kv.second;
        out += kv.first + "|" + (e.ok ? "1" : "0") + "|" + std::to_string(e.summary.rating) + "|" +
               std::to_string(e.summary.count) + "|" + std::to_string(e.appid) + "|" + std::to_string(e.fetched) +
               "\n";
    }
    return out;
}

bool LiveReviewCache::decode(std::string_view payload)
{
    std::map<std::string, LiveEntry> next;
    std::size_t at = 0;
    while (at < payload.size() && next.size() < kSteamMaxEntries)
    {
        std::size_t end = payload.find('\n', at);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string line(payload.substr(at, end - at));
        at = end + 1;
        std::vector<std::string> f;
        std::size_t p = 0;
        while (true)
        {
            const std::size_t bar = line.find('|', p);
            f.push_back(line.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
            if (bar == std::string::npos)
                break;
            p = bar + 1;
        }
        if (f.size() != 6 || f[0].empty() || f[0].size() > 64 || (f[1] != "0" && f[1] != "1"))
            continue; // damaged line: skipped
        LiveEntry e;
        e.ok = f[1] == "1";
        e.summary.rating = std::atoi(f[2].c_str());
        e.summary.count = std::atol(f[3].c_str());
        e.appid = std::atol(f[4].c_str());
        e.fetched = std::atoll(f[5].c_str());
        if (e.ok && (e.summary.rating < 0 || e.summary.rating > 100 || e.summary.count < kSteamMinReviews))
            continue;
        next[f[0]] = e;
    }
    Lock lock(mutex_);
    entries_ = std::move(next);
    return true;
}

bool LiveReviewCache::load(const std::vector<std::string> &paths)
{
    for (const std::string &path : paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 256u * 1024u))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        if (d.ok && d.version == 1 && d.payload.compare(0, 6, "steam\n") == 0)
            return decode(std::string_view(d.payload).substr(6));
    }
    return false;
}

int LiveReviewCache::save(const std::vector<std::string> &paths) const
{
    const std::string data = hui::save::encode(hui::save::Kind::stats, 1, "steam\n" + encode());
    int written = 0;
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            ++written;
    }
    return written;
}

// ---- rate limit ----

double RateLimiter::wait_needed(double now) const
{
    if (!used_)
        return 0.0;
    const double left = interval_ - (now - last_);
    return left > 0.0 && left <= interval_ ? left : 0.0; // a clock that went back must not stall
}

// ---- service ----

SteamReviewService::SteamReviewService(PopularityHub &hub, DocFetcher &fetcher, SteamReviewConfig config)
    : hub_(hub), fetcher_(fetcher), config_(std::move(config)), limiter_(kSteamMinIntervalS)
{
    if (!config_.now)
        config_.now = [] { return static_cast<std::int64_t>(unix_seconds()); };
    if (!config_.mono)
        config_.mono = [] { return now_seconds(); };
    if (!config_.sleep_s)
        config_.sleep_s = [](double s) { usleep(static_cast<useconds_t>(s * 1e6)); };
}

SteamReviewService::~SteamReviewService()
{
    thread_.join();
}

void SteamReviewService::wait_idle()
{
    thread_.join();
}

bool SteamReviewService::lookup(const std::vector<std::string> &ids, SteamSummary *out) const
{
    const std::int64_t now = config_.now();
    for (const std::string &id : ids)
    {
        SteamSummary s;
        if (cache_.state(id, now, &s) == LiveReviewCache::State::good)
        {
            *out = s;
            return true;
        }
    }
    return false;
}

bool SteamReviewService::request(const std::vector<std::string> &ids, const std::string &title)
{
    if (ids.empty() || title.empty() || !hub_.enabled() || !idle_.load())
        return false;
    const std::shared_ptr<const PopularityIndex> index = hub_.index();
    if (index == nullptr || index->best(ids) != nullptr)
        return false; // no index yet (wait for it), or the index knows this game
    if (!loaded_)
    {
        loaded_ = true;
        cache_.load(config_.cache_paths);
    }
    const std::int64_t now = config_.now();
    for (const std::string &id : ids)
    {
        if (cache_.state(id, now) != LiveReviewCache::State::unknown)
            return false;
        const auto b = backoff_.find(id);
        if (b != backoff_.end() && now < b->second && b->second - now <= kSteamNetBackoffS)
            return false;
    }
    thread_.join(); // the previous worker has set idle_: this returns at once
    for (const std::string &id : ids)
        backoff_[id] = now + kSteamNetBackoffS; // a real answer lands in the cache; an offline one just waits
    idle_.store(false);
    if (!thread_.start([this, ids, title] { run(ids, title); }, 256u << 10))
    {
        idle_.store(true);
        return false;
    }
    return true;
}

void SteamReviewService::run(std::vector<std::string> ids, std::string title)
{
    const auto get = [&](const std::string &url, std::size_t cap) -> DocResult
    {
        const double wait = limiter_.wait_needed(config_.mono());
        if (wait > 0.0)
            config_.sleep_s(wait);
        limiter_.note_request(config_.mono());
        return fetcher_.get(url, cap);
    };
    // answered: a definite result (good or "no usable data") is cached; an unreachable server is not.
    const auto finish = [&](bool ok, const SteamSummary &s, long appid, bool answered)
    {
        const std::int64_t now = config_.now();
        for (const std::string &id : ids)
        {
            if (ok)
                cache_.put_ok(id, s, appid, now);
            else if (answered)
                cache_.put_fail(id, now);
        }
        if (answered)
            cache_.save(config_.cache_paths);
        idle_.store(true);
    };
    const SteamSummary none;
    // 1. the app id: store search by the cleaned title
    const DocResult search = get(steam_search_url(title), kSteamSearchMaxBytes);
    if (!search.error.empty() || search.status == 0)
        return finish(false, none, 0, false);
    std::vector<SteamCandidate> candidates;
    long appid = 0;
    if (search.status != 200 || !parse_store_search(search.body, &candidates) ||
        !choose_appid(title, candidates, &appid))
        return finish(false, none, 0, true);
    // 2. the review summary
    const DocResult reviews = get(steam_reviews_url(appid), kSteamReviewsMaxBytes);
    if (!reviews.error.empty() || reviews.status == 0)
        return finish(false, none, 0, false);
    SteamSummary summary;
    if (reviews.status != 200 || !parse_review_summary(reviews.body, &summary))
        return finish(false, none, 0, true);
    finish(true, summary, appid, true);
}

namespace
{
SteamReviewService *g_service = nullptr;
}
SteamReviewService *steam_reviews()
{
    return g_service;
}
void set_steam_reviews(SteamReviewService *service)
{
    g_service = service;
}

} // namespace orbit
