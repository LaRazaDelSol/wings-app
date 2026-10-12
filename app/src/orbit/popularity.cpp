// Wings TV app - Popularity data: see popularity.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/popularity.hpp"

#include "core/save_file.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace orbit
{

namespace
{
constexpr std::size_t kMaxCounted = 5000; // titles the local counter remembers

long clamp_count(const json::Value &v)
{
    if (!v.is_number())
        return -1;
    const double n = v.as_number(-1.0);
    return n < 0.0 || n > 1e9 ? -1 : static_cast<long>(n);
}

void write_everywhere(const std::vector<std::string> &paths, const std::string &data, int *written)
{
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty() && written != nullptr)
            ++*written;
    }
}
} // namespace

// ---- the index ----

bool PopularityIndex::parse(std::string_view text, PopularityIndex *out, std::string *error)
{
    const auto fail = [&](const char *why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (text.size() > kPopularityMaxBytes)
        return fail("the file is too large");
    json::Value root;
    std::string why;
    if (!json::parse(text, &root, &why) || !root.is_object())
        return fail("the file is not valid JSON");
    if (!root["schemaVersion"].is_number())
        return fail("unsupported data version");
    // A newer format goes to a new address and old apps keep the old file; if a source still serves one,
    // the way out is a newer Wings.
    if (root["schemaVersion"].as_int(0) > 1)
        return fail(tr("this data needs a newer Wings (Settings > Wings > Updates)"));
    if (root["schemaVersion"].as_int(0) != 1)
        return fail("unsupported data version");
    const json::Value &titles = root["titles"];
    if (!titles.is_object())
        return fail("the file has no titles");
    PopularityIndex index;
    index.generated_at_ = root.text("generatedAt");
    const json::Value &attr = root["attribution"];
    if (attr.is_object())
        for (const std::string &key : attr.keys())
            if (attr[key].is_string() && !attr[key].as_string().empty() && attr[key].as_string().size() < 240)
                index.attribution_.push_back(attr[key].as_string());
    for (const std::string &id : titles.keys())
    {
        const json::Value &r = titles[id];
        if (!r.is_object() || id.empty() || id.size() > 16 || !r["score"].is_number() ||
            !r["confidence"].is_number())
            continue;
        PopRecord rec;
        const double score = r.number("score", -1.0);
        rec.confidence = r.number("confidence", -1.0);
        if (score < 0.0 || score > 100.0 || rec.confidence < 0.0 || rec.confidence > 1.0)
            continue;
        rec.score = static_cast<int>(score + 0.5);
        const double rating = r["rating"].is_number() ? r.number("rating", -1.0) : -1.0;
        rec.rating = rating >= 0.0 && rating <= 100.0 ? static_cast<int>(rating + 0.5) : -1;
        rec.rating_count = clamp_count(r["ratingCount"]);
        for (const json::Value &s : r["sources"].items())
            if (s.is_string() && rec.sources.size() < 8)
                rec.sources.push_back(s.as_string());
        // Optional "hero": https only and length-capped; anything else is ignored.
        if (r["hero"].is_string() && r["hero"].as_string().size() <= 300 &&
            valid_popularity_url(r["hero"].as_string()))
            rec.hero = r["hero"].as_string();
        if (r["heroLow"].is_string() && r["heroLow"].as_string().size() <= 300 &&
            valid_popularity_url(r["heroLow"].as_string()))
            rec.hero_low = r["heroLow"].as_string();
        index.records_[id] = std::move(rec);
    }
    if (index.records_.empty())
        return fail("the file has no usable titles");
    *out = std::move(index);
    return true;
}

const PopRecord *PopularityIndex::find(const std::string &title_id) const
{
    const auto it = records_.find(title_id);
    return it == records_.end() ? nullptr : &it->second;
}

const PopRecord *PopularityIndex::best(const std::vector<std::string> &title_ids) const
{
    const PopRecord *best = nullptr;
    for (const std::string &id : title_ids)
    {
        const PopRecord *r = find(id);
        if (r == nullptr || r->confidence < kPopularityMinConfidence)
            continue;
        if (best == nullptr || r->confidence > best->confidence ||
            (r->confidence == best->confidence && r->rating_count > best->rating_count))
            best = r;
    }
    return best;
}

std::string PopularityIndex::hero(const std::vector<std::string> &title_ids) const
{
    for (const std::string &id : title_ids)
    {
        const PopRecord *r = find(id);
        if (r != nullptr && !r->hero.empty())
            return r->hero;
    }
    return {};
}

std::string PopularityIndex::hero_low(const std::vector<std::string> &title_ids) const
{
    for (const std::string &id : title_ids)
    {
        const PopRecord *r = find(id);
        if (r != nullptr && !r->hero_low.empty())
            return r->hero_low;
    }
    return {};
}

bool valid_popularity_url(std::string_view url)
{
    if (url.size() > 300 || url.size() < 12 || url.substr(0, 8) != "https://")
        return false;
    const std::size_t host_end = url.find_first_of("/?#", 8);
    const std::string_view host =
        url.substr(8, host_end == std::string_view::npos ? std::string_view::npos : host_end - 8);
    if (host.empty() || host.find('@') != std::string_view::npos)
        return false;
    for (const char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f)
            return false;
    return true;
}

std::string short_count(long n)
{
    char buf[32];
    if (n < 1000)
    {
        std::snprintf(buf, sizeof buf, "%ld", n < 0 ? 0 : n);
        return buf;
    }
    const bool mega = n >= 1000000;
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(n) / (mega ? 1e6 : 1e3));
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0)
        s.resize(s.size() - 2);
    return s + (mega ? "M" : "k");
}

std::string popularity_line(const PopRecord *r)
{
    if (r == nullptr)
        return {};
    std::string line = std::string(tr("Popularity")) + " " + std::to_string(r->score);
    if (r->rating >= 0)
    {
        line += "  \xC2\xB7  " + std::string(tr("Rating")) + " " + std::to_string(r->rating);
        if (r->rating_count > 0)
            line += " (" + short_count(r->rating_count) + ")";
    }
    return line;
}

std::string attribution_text(const PopularityIndex *index)
{
    std::string out;
    if (index != nullptr)
        for (const std::string &line : index->attribution())
            out += (out.empty() ? "" : " ") + line;
    if (out.empty())
        out = "IGDB.com, Steam, Wikimedia.";
    return out;
}

// ---- the local counter ----

void LocalDownloads::add(const std::vector<std::string> &title_ids)
{
    for (const std::string &id : title_ids)
    {
        if (id.empty() || id.find_first_of("=\r\n") != std::string::npos)
            continue;
        if (counts_.size() >= kMaxCounted && counts_.find(id) == counts_.end())
            continue;
        long &n = counts_[id];
        if (n < 1000000)
            ++n;
    }
}

long LocalDownloads::count(const std::vector<std::string> &title_ids) const
{
    long best = 0;
    for (const std::string &id : title_ids)
    {
        const auto it = counts_.find(id);
        if (it != counts_.end())
            best = std::max(best, it->second);
    }
    return best;
}

std::string LocalDownloads::encode() const
{
    std::string out;
    for (const auto &entry : counts_)
        out += entry.first + "=" + std::to_string(entry.second) + "\n";
    return out;
}

bool LocalDownloads::decode(std::string_view payload)
{
    std::map<std::string, long> parsed;
    std::size_t pos = 0;
    while (pos < payload.size() && parsed.size() < kMaxCounted)
    {
        std::size_t end = payload.find('\n', pos);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string_view line = payload.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq > 16)
            continue;
        const std::string num(line.substr(eq + 1));
        char *stop = nullptr;
        const long n = std::strtol(num.c_str(), &stop, 10);
        if (stop == num.c_str() || *stop != '\0' || n <= 0 || n > 1000000)
            continue;
        parsed[std::string(line.substr(0, eq))] = n;
    }
    counts_ = std::move(parsed);
    return true;
}

bool LocalDownloads::load(const std::vector<std::string> &paths)
{
    for (const std::string &path : paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 1u << 20))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        if (d.ok && d.version == 1)
            return decode(d.payload);
    }
    return false;
}

int LocalDownloads::save(const std::vector<std::string> &paths) const
{
    int written = 0;
    write_everywhere(paths, hui::save::encode(hui::save::Kind::stats, 1, encode()), &written);
    return written;
}

// ---- sorting ----

bool is_popularity_sort(SortMode mode)
{
    return mode == SortMode::popular || mode == SortMode::rated || mode == SortMode::reviewed ||
           mode == SortMode::my_downloads;
}

SortMode effective_sort(SortMode mode, const PopularityView *view)
{
    if (!is_popularity_sort(mode))
        return mode;
    if (mode == SortMode::my_downloads)
        return view != nullptr && view->downloads != nullptr ? mode : SortMode::az;
    return view != nullptr && view->index != nullptr ? mode : SortMode::az;
}

long popularity_sort_key(SortMode mode, const PopularityView *view, const std::vector<std::string> &title_ids)
{
    if (view == nullptr)
        return -1;
    if (mode == SortMode::my_downloads)
    {
        const long n = view->downloads != nullptr ? view->downloads->count(title_ids) : 0;
        return n > 0 ? n : -1;
    }
    const PopRecord *r = view->index != nullptr ? view->index->best(title_ids) : nullptr;
    if (r == nullptr)
        return -1;
    switch (mode)
    {
    case SortMode::popular:
        return r->score;
    case SortMode::rated:
        return r->rating;
    case SortMode::reviewed:
        return r->rating_count;
    default:
        return -1;
    }
}

std::vector<SortMode> available_sorts(const PopularityView *view)
{
    std::vector<SortMode> out;
    for (int i = 0; i < kBasicSortCount; ++i)
        out.push_back(static_cast<SortMode>(i));
    if (view != nullptr && view->index != nullptr)
    {
        out.push_back(SortMode::popular);
        out.push_back(SortMode::rated);
        out.push_back(SortMode::reviewed);
    }
    if (view != nullptr && view->downloads != nullptr && view->downloads->size() > 0)
        out.push_back(SortMode::my_downloads);
    return out;
}

// ---- the hub ----

void PopularityHub::set_enabled(bool on)
{
    Lock lock(mutex_);
    if (enabled_ != on)
    {
        enabled_ = on;
        ++serial_;
    }
}

bool PopularityHub::enabled() const
{
    Lock lock(mutex_);
    return enabled_;
}

void PopularityHub::set_index(std::shared_ptr<const PopularityIndex> index)
{
    Lock lock(mutex_);
    index_ = std::move(index);
    ++serial_;
}

std::shared_ptr<const PopularityIndex> PopularityHub::index() const
{
    Lock lock(mutex_);
    return enabled_ ? index_ : nullptr;
}

void PopularityHub::set_status(const PopularityStatus &status)
{
    Lock lock(mutex_);
    status_ = status;
    ++serial_;
}

PopularityStatus PopularityHub::status() const
{
    Lock lock(mutex_);
    return status_;
}

std::uint64_t PopularityHub::serial() const
{
    Lock lock(mutex_);
    return serial_;
}

void PopularityHub::touch()
{
    Lock lock(mutex_);
    ++serial_;
}

void PopularityHub::set_downloads_paths(std::vector<std::string> paths)
{
    downloads_paths_ = std::move(paths);
    if (!downloads_paths_.empty())
        downloads_.load(downloads_paths_);
    touch();
}

void PopularityHub::count_download(const std::vector<std::string> &title_ids)
{
    if (title_ids.empty())
        return;
    downloads_.add(title_ids);
    if (!downloads_paths_.empty())
        downloads_.save(downloads_paths_);
    touch();
}

PopularityHub &popularity_hub()
{
    static PopularityHub hub;
    return hub;
}

// ---- the service ----

PopularityService::PopularityService(PopularityHub &hub, DocFetcher &fetcher, PopularityServiceConfig config)
    : hub_(hub), fetcher_(fetcher), config_(std::move(config))
{
    if (!config_.now)
        config_.now = [] { return static_cast<std::int64_t>(unix_seconds()); };
}

PopularityService::~PopularityService()
{
    thread_.join();
}

void PopularityService::apply(bool enabled, const std::string &url)
{
    const std::string chosen = url.empty() ? std::string(kPopularityDefaultUrl) : url;
    const bool changed = !applied_ || enabled != enabled_ || chosen != url_;
    applied_ = true;
    enabled_ = enabled;
    url_ = chosen;
    hub_.set_enabled(enabled);
    if (!changed || !enabled)
        return;
    start_job(false);
}

void PopularityService::refresh()
{
    if (enabled_)
        start_job(true);
}

void PopularityService::wait_idle()
{
    thread_.join();
}

void PopularityService::start_job(bool force)
{
    if (hub_.status().busy)
        return;
    thread_.join();
    PopularityStatus status = hub_.status();
    status.busy = true;
    status.message = tr("Updating popularity data...");
    hub_.set_status(status);
    const std::string url = url_;
    if (!thread_.start([this, force, url] { run(force, url); }, 256u << 10))
    {
        status.busy = false;
        status.message = tr("Could not start the update");
        hub_.set_status(status);
    }
}

void PopularityService::run(bool force, std::string url)
{
    PopularityStatus status;
    status.busy = true;
    bool cache_ok = false;
    bool cache_current = false; // the saved copy was fetched by this very build of the app
    std::int64_t fetched = 0;
    // 1. The saved copy. Missing or damaged is fine.
    for (const std::string &path : config_.cache_paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, kPopularityMaxBytes + 4096))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        if (!d.ok || d.version != 1 || d.payload.compare(0, 8, "fetched=") != 0)
            continue;
        const std::size_t nl = d.payload.find('\n');
        if (nl == std::string::npos)
            continue;
        // "fetched=<unix>\napp=<build tag>\n<file>"; a copy from before the tag has no app line.
        std::size_t body_at = nl + 1;
        std::string saved_tag;
        if (d.payload.compare(body_at, 4, "app=") == 0)
        {
            const std::size_t nl2 = d.payload.find('\n', body_at);
            if (nl2 == std::string::npos)
                continue;
            saved_tag = d.payload.substr(body_at + 4, nl2 - body_at - 4);
            body_at = nl2 + 1;
        }
        auto index = std::make_shared<PopularityIndex>();
        std::string why;
        if (!PopularityIndex::parse(std::string_view(d.payload).substr(body_at), index.get(), &why))
            continue;
        fetched = std::atoll(d.payload.substr(8, nl - 8).c_str());
        status.updated = fetched;
        status.count = index->size();
        status.message = tr("Using the saved copy");
        hub_.set_index(index);
        cache_ok = true;
        cache_current = saved_tag == config_.app_tag;
        break;
    }
    const std::int64_t now = config_.now();
    const bool fresh = cache_ok && cache_current && fetched <= now && now - fetched < config_.ttl_s;
    if (fresh && !force)
    {
        status.busy = false;
        hub_.set_status(status);
        return;
    }
    status.busy = true;
    hub_.set_status(status);
    // 2. The fetch. A failure keeps whatever the saved copy gave.
    std::string problem;
    std::shared_ptr<PopularityIndex> loaded;
    if (!valid_popularity_url(url))
        problem = tr("the source URL must start with https://");
    else
    {
        const DocResult doc = fetcher_.get(url, kPopularityMaxBytes);
        if (!doc.error.empty() || doc.status == 0)
            problem = doc.error.empty() ? std::string(tr("no answer")) : doc.error;
        else if (doc.status != 200)
            problem = "HTTP " + std::to_string(doc.status);
        else
        {
            loaded = std::make_shared<PopularityIndex>();
            if (!PopularityIndex::parse(doc.body, loaded.get(), &problem))
                loaded.reset();
            else
                write_everywhere(config_.cache_paths,
                                 hui::save::encode(hui::save::Kind::stats, 1,
                                                   "fetched=" + std::to_string(now) + "\napp=" + config_.app_tag + "\n" +
                                                       doc.body),
                                 nullptr);
        }
    }
    if (loaded)
    {
        status.updated = now;
        status.count = loaded->size();
        status.message = tr("Updated");
        hub_.set_index(loaded);
    }
    else
    {
        status.message = std::string(tr("Could not refresh")) + ": " + problem +
                         (cache_ok ? std::string(". ") + tr("Using the saved copy") : std::string());
    }
    status.busy = false;
    hub_.set_status(status);
}

} // namespace orbit
