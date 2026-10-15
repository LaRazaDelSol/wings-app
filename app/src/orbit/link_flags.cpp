// Wings TV app - Bad-link flags: see link_flags.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/link_flags.hpp"

#include "core/save_file.hpp"
#include "orbit/game_page.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace orbit
{

namespace
{

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

char upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = lower(c);
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool has(std::string_view text, std::string_view mark)
{
    return text.find(mark) != std::string_view::npos;
}

// Query parameters that track the click or expire: never part of the link's identity.
bool volatile_param(std::string_view key)
{
    const std::string k = lowered(key);
    if (starts_with(k, "utm_") || starts_with(k, "x-amz-") || starts_with(k, "response-"))
        return true;
    static const char *const kDrop[] = {"fbclid",  "gclid",     "dclid",     "msclkid",        "ref",
                                        "referrer", "_ga",      "mc_cid",    "mc_eid",         "token",
                                        "access_token", "download_token", "dl_token", "expires", "expire",
                                        "exp",     "signature", "sig",       "st",             "ts",
                                        "timestamp", "session", "sessionid", "sid"};
    for (const char *d : kDrop)
        if (k == d)
            return true;
    return false;
}

bool valid_id(std::string_view id)
{
    if (id.size() != 16)
        return false;
    for (const char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

} // namespace

const char *flag_reason_text(FlagReason reason)
{
    switch (reason)
    {
    case FlagReason::none:
        break;
    case FlagReason::host_error:
        return tr("host error");
    case FlagReason::removed:
        return tr("file removed");
    case FlagReason::wrong_file:
        return tr("wrong file");
    case FlagReason::password:
        return tr("password never worked");
    case FlagReason::broken_archive:
        return tr("broken archive");
    case FlagReason::by_hand:
        return tr("marked bad by you");
    }
    return "";
}

std::string normalise_link_url(std::string_view url)
{
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t'))
        url.remove_prefix(1);
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r' || url.back() == '\n'))
        url.remove_suffix(1);
    if (url.empty())
        return {};
    // Fragment: never sent to the server (MEGA keys live there, the file id does not).
    const std::size_t hash = url.find('#');
    if (hash != std::string_view::npos)
        url = url.substr(0, hash);
    std::string out;
    std::string scheme;
    const std::size_t sep = url.find("://");
    if (sep != std::string_view::npos)
    {
        scheme = lowered(url.substr(0, sep));
        out = scheme + "://";
        url.remove_prefix(sep + 3);
    }
    // Authority: up to the path or query.
    std::size_t end = url.find_first_of("/?");
    if (end == std::string_view::npos)
        end = url.size();
    std::string host = lowered(url.substr(0, end));
    url.remove_prefix(end);
    const std::size_t at = host.rfind('@');
    if (at != std::string::npos)
        host.erase(0, at + 1);
    if (starts_with(host, "www."))
        host.erase(0, 4);
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos)
    {
        const std::string port = host.substr(colon + 1);
        if ((port == "80" && scheme == "http") || (port == "443" && scheme == "https") || port.empty())
            host.erase(colon);
    }
    out += host;
    // Path as given, without trailing slashes.
    const std::size_t q = url.find('?');
    std::string_view path = url.substr(0, q);
    while (!path.empty() && path.back() == '/')
        path.remove_suffix(1);
    out += std::string(path);
    if (q == std::string_view::npos)
        return out;
    std::string_view query = url.substr(q + 1);
    std::string kept;
    while (!query.empty())
    {
        std::size_t amp = query.find('&');
        if (amp == std::string_view::npos)
            amp = query.size();
        const std::string_view param = query.substr(0, amp);
        query.remove_prefix(std::min(amp + 1, query.size()));
        if (param.empty())
            continue;
        if (volatile_param(param.substr(0, param.find('='))))
            continue;
        kept += (kept.empty() ? "" : "&") + std::string(param);
    }
    if (!kept.empty())
        out += "?" + kept;
    return out;
}

std::string link_id(std::string_view url, std::string_view title_id)
{
    const std::string norm = normalise_link_url(url);
    if (norm.empty())
        return {};
    std::string key;
    for (const char c : title_id)
        key += upper(c);
    key += "|" + norm;
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : key)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

FlagReason reason_from_error(std::string_view error, int http_status)
{
    if (http_status == 404 || http_status == 410)
        return FlagReason::removed;
    if (http_status == 401 || http_status == 403 || (http_status >= 500 && http_status < 600))
        return FlagReason::host_error;
    const std::string e = lowered(error);
    if (e.empty())
        return FlagReason::none;
    // Ours or passing, not the link's: cancelled, storage, network, rate limits.
    static const char *const kNotTheLink[] = {"cancel", "no space", "disk full", "not enough space", "timed out",
                                              "timeout", "network", "connection reset", "dns", "rate limit",
                                              "too many requests", "paused"};
    for (const char *m : kNotTheLink)
        if (has(e, m))
            return FlagReason::none;
    if (has(e, "password"))
        return FlagReason::password;
    static const char *const kBroken[] = {"crc", "corrupt", "damaged", "unexpected end", "broken archive",
                                          "checksum", "bad archive", "truncated", "incomplete archive"};
    for (const char *m : kBroken)
        if (has(e, m))
            return FlagReason::broken_archive;
    // install_hint.cpp's "could not decide what to install" family.
    static const char *const kWrong[] = {"no pkg", "nothing to install", "no installable", "not installable",
                                         "too many", "multiple pkg", "multiple install", "wrong file", "not a game"};
    for (const char *m : kWrong)
        if (has(e, m))
            return FlagReason::wrong_file;
    static const char *const kRemoved[] = {"404", "not found", "removed", "deleted", "no longer available",
                                           "does not exist", "doesn't exist", "file unavailable", "taken down",
                                           "expired"};
    for (const char *m : kRemoved)
        if (has(e, m))
            return FlagReason::removed;
    static const char *const kHost[] = {"provider", "resolve", "forbidden", "access denied", "premium",
                                        "server error", "bad gateway", "service unavailable", "host error",
                                        "http 403", "http 500", "http 502", "http 503"};
    for (const char *m : kHost)
        if (has(e, m))
            return FlagReason::host_error;
    return FlagReason::none;
}

// ---- the store ----

void LinkFlags::add(const std::string &id, FlagReason reason, std::int64_t now, std::string_view title)
{
    if (reason == FlagReason::none || id.empty())
        return;
    // One line in the file: no line breaks, kept short.
    std::string name;
    for (char ch : title.substr(0, 80))
        name += ch == '\n' || ch == '\r' || ch == '\t' ? ' ' : ch;
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    if (name.empty())
    {
        // A flag added again without a name keeps the one it had.
        const auto old = flags_.find(id);
        if (old != flags_.end())
            name = old->second.title;
    }
    flags_[id] = LinkFlag{reason, now, std::move(name)};
    while (flags_.size() > kMaxFlags)
    {
        auto oldest = flags_.begin();
        for (auto it = flags_.begin(); it != flags_.end(); ++it)
            if (it->second.when < oldest->second.when)
                oldest = it;
        flags_.erase(oldest);
    }
}

bool LinkFlags::clear(const std::string &id)
{
    return flags_.erase(id) > 0;
}

void LinkFlags::clear_all()
{
    flags_.clear();
}

const LinkFlag *LinkFlags::find(const std::string &id, std::int64_t now) const
{
    const auto it = flags_.find(id);
    if (it == flags_.end() || now - it->second.when >= kFlagSeconds)
        return nullptr;
    return &it->second;
}

std::size_t LinkFlags::expire(std::int64_t now)
{
    std::size_t gone = 0;
    for (auto it = flags_.begin(); it != flags_.end();)
    {
        if (now - it->second.when >= kFlagSeconds)
        {
            it = flags_.erase(it);
            ++gone;
        }
        else
            ++it;
    }
    return gone;
}

std::vector<std::pair<std::string, LinkFlag>> LinkFlags::list(std::int64_t now) const
{
    std::vector<std::pair<std::string, LinkFlag>> out;
    for (const auto &entry : flags_)
        if (now - entry.second.when < kFlagSeconds)
            out.push_back(entry);
    std::stable_sort(out.begin(), out.end(),
                     [](const auto &a, const auto &b) { return a.second.when > b.second.when; });
    return out;
}

std::string LinkFlags::encode() const
{
    std::string out;
    for (const auto &entry : flags_)
        out += entry.first + " " + std::to_string(static_cast<int>(entry.second.reason)) + " " +
               std::to_string(entry.second.when) + (entry.second.title.empty() ? "" : " " + entry.second.title) +
               "\n";
    return out;
}

bool LinkFlags::decode(std::string_view payload)
{
    LinkFlags parsed;
    std::size_t pos = 0;
    while (pos < payload.size())
    {
        std::size_t end = payload.find('\n', pos);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string line(payload.substr(pos, end - pos));
        pos = end + 1;
        const std::size_t a = line.find(' ');
        const std::size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (b == std::string::npos || !valid_id(std::string_view(line).substr(0, a)))
            continue;
        const std::string reason_text = line.substr(a + 1, b - a - 1);
        // "<id> <reason> <when>[ <title>]": the title is optional (older files have none).
        const std::size_t c = line.find(' ', b + 1);
        const std::string when_text = line.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
        const std::string title = c == std::string::npos ? std::string() : line.substr(c + 1);
        char *stop = nullptr;
        const long reason = std::strtol(reason_text.c_str(), &stop, 10);
        if (stop == reason_text.c_str() || *stop != '\0' || reason <= 0 || reason >= kFlagReasonCount)
            continue;
        const long long when = std::strtoll(when_text.c_str(), &stop, 10);
        if (stop == when_text.c_str() || *stop != '\0' || when <= 0)
            continue;
        parsed.add(line.substr(0, a), static_cast<FlagReason>(reason), static_cast<std::int64_t>(when), title);
    }
    flags_ = std::move(parsed.flags_);
    return true;
}

bool LinkFlags::load(const std::vector<std::string> &paths)
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

int LinkFlags::save(const std::vector<std::string> &paths) const
{
    const std::string data = hui::save::encode(hui::save::Kind::stats, 1, encode());
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

// ---- text ----

std::string flag_date(std::int64_t when)
{
    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
    std::int64_t z = (when >= 0 ? when : when - 86399) / 86400 + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9); // 1..12
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::to_string(day) + " " + tr(kMonths[month - 1]);
}

std::string flag_line(const LinkFlag &flag)
{
    if (flag.reason == FlagReason::none)
        return {};
    if (flag.reason == FlagReason::by_hand)
        return tr("Marked bad on {date}", {{"date", flag_date(flag.when)}});
    return tr("Failed for you on {date}: {reason}",
              {{"date", flag_date(flag.when)}, {"reason", flag_reason_text(flag.reason)}});
}

// ---- the game page ----

std::string page_link_id(const GamePage &page, int link, std::string_view title_id)
{
    if (link < 0 || link >= static_cast<int>(page.links.size()))
        return {};
    const GameLink &l = page.links[static_cast<std::size_t>(link)];
    if (!l.url.empty())
        return link_id(l.url, title_id);
    const std::string &package = page.packages[static_cast<std::size_t>(l.package)].id;
    return link_id("pkg:" + package + "/" + std::to_string(l.index), title_id);
}

void apply_link_flags(GamePage *page, const LinkFlags &flags, std::string_view title_id, std::int64_t now)
{
    for (std::size_t i = 0; i < page->links.size(); ++i)
    {
        GameLink &l = page->links[i];
        const LinkFlag *f = flags.find(page_link_id(*page, static_cast<int>(i), title_id), now);
        l.flag_reason = f != nullptr ? static_cast<std::uint8_t>(f->reason) : 0;
        l.flag_when = f != nullptr ? f->when : 0;
    }
}

// ---- the app-wide flags ----

namespace
{
LinkFlags *g_flags = nullptr;
std::vector<std::string> &flag_paths()
{
    static std::vector<std::string> paths;
    return paths;
}
} // namespace

LinkFlags *link_flags()
{
    return g_flags;
}

void set_link_flags(LinkFlags *flags, std::vector<std::string> paths)
{
    g_flags = flags;
    flag_paths() = std::move(paths);
}

void save_link_flags()
{
    if (g_flags != nullptr && !flag_paths().empty())
        g_flags->save(flag_paths());
}

void mark_page_links(GamePage *page, const std::vector<int> &links, FlagReason reason, std::string_view title_id,
                     std::int64_t now, std::string_view title)
{
    if (g_flags == nullptr || page == nullptr)
        return;
    for (int link : links)
    {
        const std::string id = page_link_id(*page, link, title_id);
        if (reason == FlagReason::none)
            g_flags->clear(id);
        else
            g_flags->add(id, reason, now, title);
    }
    save_link_flags();
    apply_link_flags(page, *g_flags, title_id, now);
}

} // namespace orbit
