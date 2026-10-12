// Wings TV app - What a download link's name says: region, firmware, kind, format, parts.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A port of parse_link() from docs/research/links-grouping.md section 1.7,
// step by step and in the same order (the order is the grammar). Each regular
// expression of the reference became a small matcher below; the comment on
// each quotes the expression it replaces. Matchers find the leftmost match, as
// Python's re.search does, and take() cuts it out of the working text, leaving
// a space. app/tools/linkref/ holds the reference and a dump script; the unit
// test compares both over the recorded catalogue.

#include "orbit/link_meta.hpp"

#include "orbit/i18n.hpp"
#include "orbit/link_text.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>

namespace orbit
{
namespace
{

using text::ieq;
using text::ieq_at;
using text::is_digit;
using text::lowered;
using text::not_after_word_or_dot;
using text::npos;
using text::skip_spaces;
using text::word_at;
using text::word_before;

// ---- host tables (keys are lower case) ----

struct Alias
{
    std::string_view alias;
    std::string_view label;
};

// Host alias as written in names -> canonical label, in the reference's order
// (it matters for ties: the matcher tries the longest alias first, stable).
constexpr Alias kHostAliases[] = {
    {"akirabox", "Akirabox"},
    {"akira", "Akirabox"},
    {"akia", "Akirabox"},
    {"akirabox.com", "Akirabox"},
    {"akirabox.to", "Akirabox"},
    {"vikingfile", "VikingFile"},
    {"viki", "VikingFile"},
    {"vikingfile.com", "VikingFile"},
    {"vik1ngfile.site", "VikingFile"},
    {"rootz", "Rootz"},
    {"1fichier", "1fichier"},
    {"mediafire", "MediaFire"},
    {"buzzheavier", "BuzzHeavier"},
    {"buzz", "BuzzHeavier"},
    {"ts", "BuzzHeavier"},
    {"rapidgator", "Rapidgator"},
    {"gofile", "Gofile"},
    {"gofile.io", "Gofile"},
    {"filecrypt", "Filecrypt"},
    {"ranoz", "Ranoz"},
    {"transfer.it", "Transfer.it"},
    {"mega", "Mega"},
    {"datanodes", "DataNodes"},
    {"datanodes.to", "DataNodes"},
    {"data", "DataNodes"}, // DLPS "DATA (exFAT)"; PFS "Data - DLC" is datavaults (the URL says)
    {"filek", "FileKeeper"},
    {"filekeeper", "FileKeeper"},
    {"filed", "FileDitch"},
    {"fileditch", "FileDitch"},
    {"vault", "DataVaults"},
    {"datavaults.co", "DataVaults"},
    {"archive", "archive.org"},
    {"download", ""},
    {"fuckingfast.net", "FuckingFast"},
    {"urlvanish.com", "urlvanish"},
    {"tinyurl.com", "tinyurl"},
    {"1024terabox.com", "TeraBox"},
    {"link-vault.org", "link-vault"},
};
constexpr std::size_t kAliasCount = sizeof(kHostAliases) / sizeof(kHostAliases[0]);

// The aliases longest first; equal lengths keep the table order.
const std::array<std::uint8_t, kAliasCount> &alias_order()
{
    static const std::array<std::uint8_t, kAliasCount> order = []
    {
        std::array<std::uint8_t, kAliasCount> o{};
        for (std::size_t i = 0; i < kAliasCount; ++i)
            o[i] = static_cast<std::uint8_t>(i);
        std::stable_sort(o.begin(), o.end(), [](std::uint8_t a, std::uint8_t b)
                         { return kHostAliases[a].alias.size() > kHostAliases[b].alias.size(); });
        return o;
    }();
    return order;
}

constexpr std::string_view kWrapperHosts[] = {"pippo26442999.github.io", "link-vault.org",
                                              "tinyurl.com", "urlvanish.com", "filecrypt.cc"};

constexpr Alias kUrlHostLabels[] = {
    {"akirabox.com", "Akirabox"},
    {"akirabox.to", "Akirabox"},
    {"vikingfile.com", "VikingFile"},
    {"vik1ngfile.site", "VikingFile"},
    {"rootz.so", "Rootz"},
    {"1fichier.com", "1fichier"},
    {"mediafire.com", "MediaFire"},
    {"buzzheavier.com", "BuzzHeavier"},
    {"ts.buzzheavier.com", "BuzzHeavier"},
    {"rapidgator.net", "Rapidgator"},
    {"gofile.io", "Gofile"},
    {"filecrypt.cc", "Filecrypt"},
    {"ranoz.gg", "Ranoz"},
    {"transfer.it", "Transfer.it"},
    {"mega.nz", "Mega"},
    {"datanodes.to", "DataNodes"},
    {"datanodes.co", "DataNodes"},
    {"filekeeper.net", "FileKeeper"},
    {"fileditchfiles.st", "FileDitch"},
    {"theditch.st", "FileDitch"},
    {"datavaults.co", "DataVaults"},
    {"archive.org", "archive.org"},
    {"fuckingfast.net", "FuckingFast"},
    {"1024terabox.com", "TeraBox"},
};

constexpr Alias kPackagerAliases[] = {
    {"pkykzhack", "Psykzhack"},         {"psykzhack", "Psykzhack"},
    {"realromhunter", "realromhunter"}, {"romhunter", "realromhunter"},
    {"high-speed007", "High-Speed007"}, {"high-speed9", "High-Speed007"},
};

constexpr std::string_view kRegions[] = {"USA", "EUR", "JPN", "ASIA", "HK", "KOR"};

std::string packager_name(std::string_view name)
{
    const std::string low = lowered(name);
    for (const Alias &a : kPackagerAliases)
        if (low == a.alias)
            return std::string(a.label);
    return std::string(name);
}

bool starts_with(std::string_view s, std::string_view p)
{
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

bool contains(std::string_view s, std::string_view p)
{
    return s.find(p) != npos;
}

std::string_view strip_chars(std::string_view s, std::string_view chars)
{
    while (!s.empty() && chars.find(s.front()) != npos)
        s.remove_prefix(1);
    while (!s.empty() && chars.find(s.back()) != npos)
        s.remove_suffix(1);
    return s;
}

// [\w\-]+ from i: its end (i when nothing matches).
std::size_t word_dash_run(std::string_view s, std::size_t i)
{
    while (i < s.size())
    {
        if (s[i] == '-')
        {
            ++i;
            continue;
        }
        const std::size_t r = text::word_run(s, i);
        if (r == i)
            break;
        i = r;
    }
    return i;
}

// ---- matches ----

struct Span
{
    std::size_t b = npos;
    std::size_t e = npos;
    bool ok() const
    {
        return b != npos;
    }
};

struct Match
{
    std::size_t b = 0;
    std::size_t e = 0;
    Span g[3];
};

// What take() cut out: the whole match and its groups, as text.
struct Taken
{
    std::string all;
    std::string g[3];
    bool has[3] = {false, false, false};
};

// The leftmost i where at(s, i, m) matches.
template <class At> bool search(std::string_view s, Match *m, At at)
{
    for (std::size_t i = 0; i <= s.size(); ++i)
    {
        *m = Match{};
        if (at(s, i, m))
            return true;
    }
    return false;
}

// \b<lit>\b at i, for a lower-case literal made of word characters.
bool word_lit_at(std::string_view s, std::size_t i, std::string_view lit)
{
    return ieq_at(s, i, lit) && !word_before(s, i) && !word_at(s, i + lit.size());
}

std::size_t find_word(std::string_view s, std::string_view lit)
{
    for (std::size_t i = 0; i + lit.size() <= s.size(); ++i)
        if (word_lit_at(s, i, lit))
            return i;
    return npos;
}

// \s+ at i: the position after the spaces, or npos when there is none.
std::size_t spaces1(std::string_view s, std::size_t i)
{
    return i < s.size() && s[i] == ' ' ? skip_spaces(s, i) : npos;
}

// ---- firmware and version shapes ----

// ([1-9]|1[0-9])\.(xx|\d\d) filling exactly [b, e): major and minor spans.
bool fw_shape(std::string_view s, std::size_t b, std::size_t e, Span *major, Span *minor)
{
    std::size_t dot = npos;
    if (e - b == 4 && s[b] >= '1' && s[b] <= '9' && s[b + 1] == '.')
        dot = b + 1;
    else if (e - b == 5 && s[b] == '1' && is_digit(s[b + 1]) && s[b + 2] == '.')
        dot = b + 2;
    if (dot == npos)
        return false;
    const char x = text::lower(s[dot + 1]);
    const char y = text::lower(s[dot + 2]);
    if (!((x == 'x' && y == 'x') || (is_digit(x) && is_digit(y))))
        return false;
    *major = Span{b, dot};
    *minor = Span{dot + 1, dot + 3};
    return true;
}

// (?<![\w.])(?:FW\s*)?([1-9]|1[0-9])\.(xx|\d\d)(\+)?(?![\w.]) at i.
// The core takes only [\w.] characters and must be followed by none, so it is
// exactly the [\w.] run that starts where it starts.
bool fw_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || !not_after_word_or_dot(s, i))
        return false;
    const auto core = [&](std::size_t j)
    {
        const std::size_t r = text::word_dot_run(s, j);
        if (!fw_shape(s, j, r, &m->g[0], &m->g[1]))
            return false;
        m->b = i;
        m->e = r;
        m->g[2] = Span{};
        if (r < s.size() && s[r] == '+' &&
            !(r + 1 < s.size() && (s[r + 1] == '.' || word_at(s, r + 1))))
        {
            m->g[2] = Span{r, r + 1};
            m->e = r + 1;
        }
        return true;
    };
    if (ieq_at(s, i, "fw") && core(skip_spaces(s, i + 2)))
        return true;
    return core(i);
}

// (?<![\w.])([1-9])xx\b(\+)? at i.
bool fw_compact_at(std::string_view s, std::size_t i, Match *m)
{
    if (i + 3 > s.size() || s[i] < '1' || s[i] > '9' || !ieq_at(s, i + 1, "xx") ||
        word_at(s, i + 3) || !not_after_word_or_dot(s, i))
        return false;
    m->b = i;
    m->e = i + 3;
    m->g[0] = Span{i, i + 1};
    if (i + 3 < s.size() && s[i + 3] == '+')
    {
        m->g[1] = Span{i + 3, i + 4};
        m->e = i + 4;
    }
    return true;
}

// Digit segments separated by '.', exactly filling [b, e): the first has 1-2
// digits, the second second_lo..second_hi, the others 1-3; segment count in
// [min_segments, max_segments].
bool dotted_shape(std::string_view s, std::size_t b, std::size_t e, std::size_t second_lo,
                  std::size_t second_hi, std::size_t min_segments, std::size_t max_segments)
{
    std::size_t segments = 0;
    std::size_t at = b;
    while (true)
    {
        std::size_t d = at;
        while (d < e && is_digit(s[d]))
            ++d;
        const std::size_t n = d - at;
        const std::size_t lo = segments == 0 ? 1 : segments == 1 ? second_lo : 1;
        const std::size_t hi = segments == 0 ? 2 : segments == 1 ? second_hi : 3;
        if (n < lo || n > hi)
            return false;
        ++segments;
        if (d == e)
            break;
        if (s[d] != '.' || segments >= max_segments)
            return false;
        at = d + 1;
    }
    return segments >= min_segments;
}

// (?<![\w.])v(\d{1,2}(?:\.\d{1,3}){1,3})(?![\w.]) at i.
bool version_v_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || text::lower(s[i]) != 'v' || !not_after_word_or_dot(s, i))
        return false;
    const std::size_t r = text::word_dot_run(s, i + 1);
    if (!dotted_shape(s, i + 1, r, 1, 3, 2, 4))
        return false;
    m->b = i;
    m->e = r;
    m->g[0] = Span{i + 1, r};
    return true;
}

// (?<![\w.])(\d{1,2}\.\d{2,4}(?:\.\d{1,3}){0,2})(?![\w.]) at i.
bool version_bare_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || !is_digit(s[i]) || !not_after_word_or_dot(s, i))
        return false;
    const std::size_t r = text::word_dot_run(s, i);
    if (!dotted_shape(s, i, r, 2, 4, 2, 4))
        return false;
    m->b = i;
    m->e = r;
    m->g[0] = Span{i, r};
    return true;
}

// (\d+(?:\.\d+)?)\s*(gb|mb|tb)\b at i.
bool size_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || !is_digit(s[i]))
        return false;
    const auto unit = [&](std::size_t num_end)
    {
        const std::size_t p = skip_spaces(s, num_end);
        for (const char *u : {"gb", "mb", "tb"})
        {
            if (ieq_at(s, p, u) && !word_at(s, p + 2))
            {
                m->b = i;
                m->e = p + 2;
                m->g[0] = Span{i, num_end};
                m->g[1] = Span{p, p + 2};
                return true;
            }
        }
        return false;
    };
    std::size_t j = i;
    while (j < s.size() && is_digit(s[j]))
        ++j;
    if (j + 1 < s.size() && s[j] == '.' && is_digit(s[j + 1]))
    {
        std::size_t k = j + 1;
        while (k < s.size() && is_digit(s[k]))
            ++k;
        if (unit(k))
            return true;
    }
    return unit(j);
}

// \b([A-Z]{4}\d{5})\b (case-sensitive) at i.
bool title_id_at(std::string_view s, std::size_t i, Match *m)
{
    if (i + 9 > s.size() || word_before(s, i) || word_at(s, i + 9))
        return false;
    for (std::size_t k = 0; k < 9; ++k)
        if (k < 4 ? !(s[i + k] >= 'A' && s[i + k] <= 'Z') : !is_digit(s[i + k]))
            return false;
    m->b = i;
    m->e = i + 9;
    m->g[0] = Span{i, i + 9};
    return true;
}

// \bby\s+@?([\w\-]+) at i.
bool by_at(std::string_view s, std::size_t i, Match *m)
{
    if (!ieq_at(s, i, "by") || word_before(s, i))
        return false;
    const std::size_t j = spaces1(s, i + 2);
    if (j == npos)
        return false;
    for (const bool at_sign : {true, false})
    {
        std::size_t k = j;
        if (at_sign)
        {
            if (k >= s.size() || s[k] != '@')
                continue;
            ++k;
        }
        const std::size_t r = word_dash_run(s, k);
        if (r > k)
        {
            m->b = i;
            m->e = r;
            m->g[0] = Span{k, r};
            return true;
        }
    }
    return false;
}

// \b(USA|EUR|JPN|ASIA|HK|KOR)\s+to\s+(EUR|JPN|USA)\s+lang(?:uage)?s\b (ignore case) at i.
bool languages_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || word_before(s, i))
        return false;
    for (const std::string_view region : {"usa", "eur", "jpn", "asia", "hk", "kor"})
    {
        if (!ieq_at(s, i, region))
            continue;
        const std::size_t j = spaces1(s, i + region.size());
        if (j == npos || !ieq_at(s, j, "to"))
            return false;
        const std::size_t k = spaces1(s, j + 2);
        if (k == npos)
            return false;
        for (const char *target : {"eur", "jpn", "usa"})
        {
            if (!ieq_at(s, k, target))
                continue;
            const std::size_t l = spaces1(s, k + 3);
            if (l == npos || !ieq_at(s, l, "lang"))
                return false;
            const std::size_t p = l + 4;
            std::size_t e = npos;
            if (ieq_at(s, p, "uages") && !word_at(s, p + 5))
                e = p + 5;
            else if (ieq_at(s, p, "s") && !word_at(s, p + 1))
                e = p + 1;
            if (e == npos)
                return false;
            m->b = i;
            m->e = e;
            m->g[0] = Span{i, i + region.size()};
            m->g[1] = Span{k, k + 3};
            return true;
        }
        return false;
    }
    return false;
}

// \b(USA|EUR|JPN|ASIA|HK|KOR)\b (case-sensitive) at i.
bool region_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || word_before(s, i))
        return false;
    for (const std::string_view region : kRegions)
    {
        if (s.substr(i, region.size()) == region && !word_at(s, i + region.size()))
        {
            m->b = i;
            m->e = i + region.size();
            m->g[0] = Span{m->b, m->e};
            return true;
        }
    }
    return false;
}

// \brelease\s+(\d+)\b at i.
bool release_at(std::string_view s, std::size_t i, Match *m)
{
    if (!word_lit_at(s, i, "release"))
        return false;
    const std::size_t j = spaces1(s, i + 7);
    if (j == npos)
        return false;
    std::size_t d = j;
    while (d < s.size() && is_digit(s[d]))
        ++d;
    if (d == j || word_at(s, d))
        return false;
    m->b = i;
    m->e = d;
    m->g[0] = Span{j, d};
    return true;
}

// \bf?fpkg\b at i.
bool fpkg_at(std::string_view s, std::size_t i, Match *m)
{
    for (const char *lit : {"ffpkg", "fpkg"})
    {
        if (word_lit_at(s, i, lit))
        {
            m->b = i;
            m->e = i + std::string_view(lit).size();
            return true;
        }
    }
    return false;
}

// \bback\s*port\b(?:\s+(?:([1-9]|1[0-9])\.(xx|\d\d)|([1-9])xx|backport))? at i.
bool backport_at(std::string_view s, std::size_t i, Match *m)
{
    if (!ieq_at(s, i, "back") || word_before(s, i))
        return false;
    const std::size_t j = skip_spaces(s, i + 4);
    if (!ieq_at(s, j, "port") || word_at(s, j + 4))
        return false;
    m->b = i;
    m->e = j + 4;
    const std::size_t k = spaces1(s, j + 4);
    if (k == npos)
        return true;
    const auto minor_at = [&](std::size_t p)
    {
        return (ieq_at(s, p, "xx")) ||
               (p + 2 <= s.size() && is_digit(s[p]) && is_digit(s[p + 1]));
    };
    if (k + 4 <= s.size() && s[k] >= '1' && s[k] <= '9' && s[k + 1] == '.' && minor_at(k + 2))
    {
        m->g[0] = Span{k, k + 1};
        m->g[1] = Span{k + 2, k + 4};
        m->e = k + 4;
    }
    else if (k + 5 <= s.size() && s[k] == '1' && is_digit(s[k + 1]) && s[k + 2] == '.' &&
             minor_at(k + 3))
    {
        m->g[0] = Span{k, k + 2};
        m->g[1] = Span{k + 3, k + 5};
        m->e = k + 5;
    }
    else if (k + 3 <= s.size() && s[k] >= '1' && s[k] <= '9' && ieq_at(s, k + 1, "xx"))
    {
        m->g[2] = Span{k, k + 1};
        m->e = k + 3;
    }
    else if (ieq_at(s, k, "backport"))
    {
        m->e = k + 8;
    }
    return true;
}

// \b(?:standard|std)\b at i.
bool standard_at(std::string_view s, std::size_t i, Match *m)
{
    for (const char *lit : {"standard", "std"})
    {
        if (word_lit_at(s, i, lit))
        {
            m->b = i;
            m->e = i + std::string_view(lit).size();
            return true;
        }
    }
    return false;
}

// \bfull\s+game\s+unlock\b at i.
bool full_unlock_at(std::string_view s, std::size_t i, Match *m)
{
    if (!ieq_at(s, i, "full") || word_before(s, i))
        return false;
    const std::size_t j = spaces1(s, i + 4);
    if (j == npos || !ieq_at(s, j, "game"))
        return false;
    const std::size_t k = spaces1(s, j + 4);
    if (k == npos || !ieq_at(s, k, "unlock") || word_at(s, k + 6))
        return false;
    m->b = i;
    m->e = k + 6;
    return true;
}

// \b(?:download\s+)?(?:base\s+)?game\b|\bbase\b at i.
bool game_at(std::string_view s, std::size_t i, Match *m)
{
    if (i >= s.size() || word_before(s, i))
        return false;
    // (?:base\s+)?game\b from j
    const auto tail = [&](std::size_t j) -> std::size_t
    {
        if (ieq_at(s, j, "base"))
        {
            const std::size_t k = spaces1(s, j + 4);
            if (k != npos && ieq_at(s, k, "game") && !word_at(s, k + 4))
                return k + 4;
        }
        if (ieq_at(s, j, "game") && !word_at(s, j + 4))
            return j + 4;
        return npos;
    };
    std::size_t e = npos;
    if (ieq_at(s, i, "download"))
    {
        const std::size_t k = spaces1(s, i + 8);
        if (k != npos)
            e = tail(k);
    }
    if (e == npos)
        e = tail(i);
    if (e == npos && ieq_at(s, i, "base") && !word_at(s, i + 4))
        e = i + 4;
    if (e == npos)
        return false;
    m->b = i;
    m->e = e;
    return true;
}

// \bbase\b|\bgame\b at i.
bool base_or_game_at(std::string_view s, std::size_t i, Match *m)
{
    for (const char *lit : {"base", "game"})
    {
        if (word_lit_at(s, i, lit))
        {
            m->b = i;
            m->e = i + 4;
            return true;
        }
    }
    return false;
}

// (?:\+\s*)?(?:\bonly\s+)?(?:\ball\s+)?\bdlcs?\b(?:\s+merged)?(?:\s+unlocker)? at i.
bool dlc_at(std::string_view s, std::size_t i, Match *m)
{
    // \bdlcs?\b(?:\s+merged)?(?:\s+unlocker)? from j: the end, or npos.
    const auto core = [&](std::size_t j) -> std::size_t
    {
        if (!ieq_at(s, j, "dlc") || word_before(s, j))
            return npos;
        std::size_t k = j + 3;
        if (k < s.size() && text::lower(s[k]) == 's' && !word_at(s, k + 1))
            k += 1;
        else if (word_at(s, k))
            return npos;
        for (const char *more : {"merged", "unlocker"})
        {
            const std::size_t p = spaces1(s, k);
            if (p != npos && ieq_at(s, p, more))
                k = p + std::string_view(more).size();
        }
        return k;
    };
    // (?:\b<word>\s+)? from j: where the rest starts with it, or npos.
    const auto prefix = [&](std::size_t j, const char *word) -> std::size_t
    {
        if (!ieq_at(s, j, word) || word_before(s, j))
            return npos;
        return spaces1(s, j + std::string_view(word).size());
    };
    for (const bool plus : {true, false})
    {
        std::size_t j = i;
        if (plus)
        {
            if (i >= s.size() || s[i] != '+')
                continue;
            j = skip_spaces(s, i + 1);
        }
        for (const bool only : {true, false})
        {
            std::size_t k = j;
            if (only && (k = prefix(j, "only")) == npos)
                continue;
            for (const bool all : {true, false})
            {
                std::size_t l = k;
                if (all && (l = prefix(k, "all")) == npos)
                    continue;
                const std::size_t e = core(l);
                if (e != npos)
                {
                    m->b = i;
                    m->e = e;
                    return true;
                }
            }
        }
    }
    return false;
}

// \bdlcs?\b at i.
bool dlc_word_at(std::string_view s, std::size_t i, Match *m)
{
    for (const char *lit : {"dlcs", "dlc"})
    {
        if (word_lit_at(s, i, lit))
        {
            m->b = i;
            m->e = i + std::string_view(lit).size();
            return true;
        }
    }
    return false;
}

// \bpre[\s-]?order\s+bonus\b|\bpre[\s-]?order\b at i.
bool preorder_at(std::string_view s, std::size_t i, Match *m)
{
    if (!ieq_at(s, i, "pre") || word_before(s, i))
        return false;
    for (const bool bonus : {true, false})
    {
        for (const bool sep : {true, false})
        {
            std::size_t j = i + 3;
            if (sep)
            {
                if (j >= s.size() || (s[j] != ' ' && s[j] != '-'))
                    continue;
                ++j;
            }
            if (!ieq_at(s, j, "order"))
                continue;
            std::size_t e = j + 5;
            if (bonus)
            {
                const std::size_t k = spaces1(s, e);
                if (k == npos || !ieq_at(s, k, "bonus") || word_at(s, k + 5))
                    continue;
                e = k + 5;
            }
            else if (word_at(s, e))
            {
                continue;
            }
            m->b = i;
            m->e = e;
            return true;
        }
    }
    return false;
}

// ---- the reference's helpers ----

// split_top(): split on " - " only outside brackets ("1fichier- 5.xx" too).
std::vector<std::string> split_top(std::string_view s)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    std::size_t i = 0;
    while (i < s.size())
    {
        const char c = s[i];
        if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            depth = std::max(0, depth - 1);
        if (depth == 0 && s.substr(i, 3) == " - ")
        {
            out.push_back(std::move(cur));
            cur.clear();
            i += 3;
            continue;
        }
        if (depth == 0 && s.substr(i, 2) == "- " && cur.size() >= 7 &&
            std::string_view(cur).substr(cur.size() - 7) == "fichier")
        {
            out.push_back(std::move(cur));
            cur.clear();
            i += 2;
            continue;
        }
        cur += c;
        ++i;
    }
    out.push_back(std::move(cur));
    std::vector<std::string> kept;
    for (const std::string &x : out)
    {
        const std::string_view t = strip_chars(x, " ");
        if (!t.empty())
            kept.emplace_back(t);
    }
    if (kept.empty())
        kept.emplace_back();
    return kept;
}

// match_host_word(): (label, rest) when seg starts with a host alias. When the
// name was cut at 63 bytes a lone word may be the start of an alias ("Buzzheavie").
bool match_host_word(std::string_view seg, bool allow_prefix, std::string *label,
                     std::string *rest)
{
    const std::string low = lowered(seg);
    for (const std::uint8_t k : alias_order())
    {
        const std::string_view a = kHostAliases[k].alias;
        if (low == a || (starts_with(low, a) && low.size() > a.size() &&
                         (low[a.size()] == ' ' || low[a.size()] == '(' || low[a.size()] == '-')))
        {
            *label = std::string(kHostAliases[k].label);
            *rest = std::string(strip_chars(seg.substr(a.size()), " -"));
            return true;
        }
    }
    if (allow_prefix && low.size() >= 2)
    {
        const std::string_view first = std::string_view(low).substr(0, low.find(' '));
        const bool one_word = strip_chars(seg, " ").find(' ') == npos;
        if (first.size() >= 4 && one_word)
        {
            for (const std::uint8_t k : alias_order())
            {
                if (starts_with(kHostAliases[k].alias, first))
                {
                    *label = std::string(kHostAliases[k].label);
                    rest->clear();
                    return true;
                }
            }
        }
    }
    return false;
}

std::string_view url_host_label(std::string_view host)
{
    for (const Alias &a : kUrlHostLabels)
        if (host == a.alias)
            return a.label;
    return {};
}

// (?:\bpart\s*|\bpt\.?\s*)(\d{1,3})\s*$ : the start of the marker, or npos.
std::size_t find_part(std::string_view s, int *part)
{
    std::size_t e = s.size();
    while (e > 0 && s[e - 1] == ' ')
        --e;
    std::size_t d = e;
    while (d > 0 && is_digit(s[d - 1]))
        --d;
    if (e - d < 1 || e - d > 3)
        return npos;
    std::size_t j = d;
    while (j > 0 && s[j - 1] == ' ')
        --j;
    std::size_t start = npos;
    if (j >= 4 && ieq_at(s, j - 4, "part") && !word_before(s, j - 4))
        start = j - 4;
    else if (j >= 3 && s[j - 1] == '.' && ieq_at(s, j - 3, "pt") && !word_before(s, j - 3))
        start = j - 3;
    else if (j >= 2 && ieq_at(s, j - 2, "pt") && !word_before(s, j - 2))
        start = j - 2;
    if (start == npos)
        return npos;
    int n = 0;
    for (std::size_t k = d; k < e; ++k)
        n = n * 10 + (s[k] - '0');
    *part = n;
    return start;
}

// \s(?:w1|w2|...)\s*$ : the index of the space before the last word when that
// word is one of `words` (lower case), or npos.
std::size_t find_last_word_in(std::string_view s, std::initializer_list<std::string_view> words)
{
    std::size_t e = s.size();
    while (e > 0 && s[e - 1] == ' ')
        --e;
    std::size_t b = e;
    while (b > 0 && s[b - 1] != ' ')
        --b;
    if (b == 0 || b == e)
        return npos;
    const std::string_view w = s.substr(b, e - b);
    for (const std::string_view x : words)
        if (ieq(w, x))
            return b - 1;
    return npos;
}

// One bracketed group: [\[(]\s*([^\[\]()]*?)\s*[\])].
struct Tag
{
    std::string whole; // group(0)
    std::string inner; // group(1)
};

std::vector<Tag> find_tags(std::string_view s)
{
    std::vector<Tag> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        if (s[i] != '[' && s[i] != '(')
        {
            ++i;
            continue;
        }
        const std::size_t j = skip_spaces(s, i + 1);
        std::size_t k = j;
        while (k < s.size() && s[k] != '[' && s[k] != ']' && s[k] != '(' && s[k] != ')')
            ++k;
        if (k < s.size() && (s[k] == ']' || s[k] == ')'))
        {
            std::size_t ie = k;
            while (ie > j && s[ie - 1] == ' ')
                --ie;
            out.push_back(Tag{std::string(s.substr(i, k + 1 - i)), std::string(s.substr(j, ie - j))});
            i = k + 1;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

// (all\s+)?dlcs?(\s+merged)? as a full match of lower-case, space-normalised text.
bool dlc_words(std::string_view t)
{
    if (starts_with(t, "all "))
        t.remove_prefix(4);
    if (!starts_with(t, "dlc"))
        return false;
    t.remove_prefix(3);
    const auto tail_ok = [](std::string_view r) { return r.empty() || r == " merged"; };
    if (starts_with(t, "s") && tail_ok(t.substr(1)))
        return true;
    return tail_ok(t);
}

// re.split(r"\s+-\s+|\s*&\s*|\s+", text) without the empty pieces.
std::vector<std::string_view> split_names(std::string_view t)
{
    std::vector<std::string_view> out;
    std::size_t piece = 0;
    std::size_t p = 0;
    while (p < t.size())
    {
        std::size_t sep_end = npos;
        const std::size_t q = skip_spaces(t, p);
        if (q > p && q + 1 < t.size() && t[q] == '-' && t[q + 1] == ' ')
            sep_end = skip_spaces(t, q + 1);
        else if (q < t.size() && t[q] == '&')
            sep_end = skip_spaces(t, q + 1);
        else if (q > p)
            sep_end = q;
        if (sep_end == npos)
        {
            ++p;
            continue;
        }
        if (p > piece)
            out.push_back(t.substr(piece, p - piece));
        piece = sep_end;
        p = sep_end;
    }
    if (piece < t.size())
        out.push_back(t.substr(piece));
    return out;
}

// ---- the parser ----

class Parser
{
  public:
    explicit Parser(LinkMeta *out) : f(*out)
    {
    }
    void run(std::string_view name, std::string_view url_host);

  private:
    LinkMeta &f;
    std::string work;
    bool kind_set = false; // the reference's kind is None until a step sets it

    // take(): cut the leftmost match out of the working text.
    template <class At> bool take(At at, Taken *t = nullptr)
    {
        Match m;
        if (!search(work, &m, at))
            return false;
        if (t)
        {
            t->all = work.substr(m.b, m.e - m.b);
            for (int k = 0; k < 3; ++k)
            {
                t->has[k] = m.g[k].ok();
                if (t->has[k])
                    t->g[k] = work.substr(m.g[k].b, m.g[k].e - m.g[k].b);
            }
        }
        work.replace(m.b, m.e - m.b, " ");
        return true;
    }
    bool take_word(std::string_view lit)
    {
        const std::size_t at = find_word(work, lit);
        if (at == npos)
            return false;
        work.replace(at, lit.size(), " ");
        return true;
    }
    void set_kind(LinkKind kind)
    {
        f.kind = kind;
        kind_set = true;
    }

    void host_segment(std::string_view s, std::string_view url_host);
    void tags();
    void words();
    void kind_and_label();
};

void Parser::host_segment(std::string_view s, std::string_view url_host)
{
    // 2. host segment: last top-level segment, else the first (PFS puts the host first)
    std::vector<std::string> segs = split_top(s);
    std::string label;
    std::string rest;
    long host_seg = -1;
    if (segs.size() > 1)
    {
        if (match_host_word(segs.back(), f.truncated, &label, &rest))
            host_seg = static_cast<long>(segs.size()) - 1;
        else if (match_host_word(segs.front(), false, &label, &rest))
            host_seg = 0;
    }
    else if (match_host_word(segs.front(), false, &label, &rest))
    {
        host_seg = 0;
    }
    if (host_seg >= 0)
    {
        f.host = label;
        f.host_from = HostSource::name;
        segs[static_cast<std::size_t>(host_seg)] = rest;
    }
    work = " ";
    bool first = true;
    for (const std::string &x : segs)
    {
        if (x.empty())
            continue;
        if (!first)
            work += ' ';
        work += x;
        first = false;
    }
    work += ' ';
    // The URL host wins unless the URL is a wrapper.
    const bool wrapper = is_wrapper_host(url_host);
    const std::string_view url_label = url_host_label(url_host);
    if (!url_host.empty() && !wrapper && !url_label.empty())
    {
        f.host = std::string(url_label);
        f.host_from = HostSource::url;
    }
    else if (!url_host.empty() && f.host.empty())
    {
        f.host = std::string(url_host);
        f.host_from = HostSource::url;
    }
    f.wrapped = wrapper && f.host_from == HostSource::name;
}

// 4. tags in brackets: packagers, APR-EMU, languages, DLC markers.
void Parser::tags()
{
    const std::vector<Tag> found = find_tags(work);
    for (const Tag &tag : found)
    {
        const std::string inner = text::norm_space(tag.inner);
        const std::string low = lowered(inner);
        bool consumed = true;
        Match m;
        Match compact;
        if (low == "apr-emu")
        {
            f.tags.emplace_back("APR-EMU");
        }
        else if (contains(low, "arabic"))
        {
            f.tags.emplace_back("+Arabic");
        }
        else if (starts_with(low, "v") && version_v_at(inner, 0, &m) && m.e == inner.size())
        {
            f.version = inner.substr(1);
        }
        else if (dlc_words(low) || starts_with(low, "dlc ") || low == "dlcs" || low == "bundle dlcs" ||
                 low == "game + dlc" || starts_with(low, "all dlc"))
        {
            if (!kind_set)
                set_kind(LinkKind::game_dlc);
            if (contains(low, "all") || contains(low, "merged") || contains(low, "bundle"))
                f.all_dlc = true;
            if (starts_with(low, "dlc ") && low != "dlc merged")
                f.label = inner.substr(4);
        }
        else if (starts_with(low, "backport"))
        {
            f.backport = true;
            Match fm;
            if (search(inner, &fm, fw_at))
                f.backport_target = std::string(inner.substr(fm.g[0].b, fm.g[0].e - fm.g[0].b)) +
                                    "." + lowered(inner.substr(fm.g[1].b, 2));
        }
        else if (low == "exfat")
        {
            f.format = LinkFormat::exfat;
        }
        else if (starts_with(low, "only work"))
        {
            Match fm;
            if (search(inner, &fm, fw_at))
                f.fw = std::string(inner.substr(fm.g[0].b, fm.g[0].e - fm.g[0].b)) + "." +
                       lowered(inner.substr(fm.g[1].b, 2));
        }
        else if ((fw_at(inner, 0, &m) && m.e == inner.size()) ||
                 (inner.size() == 3 && fw_compact_at(inner, 0, &compact)))
        {
            // FW_RE.fullmatch keeps only the family: "(7.61)" -> "7.xx", as the reference does.
            Match full;
            if (fw_at(inner, 0, &full) && full.e == inner.size())
                f.fw = inner.substr(full.g[0].b, full.g[0].e - full.g[0].b) + ".xx";
            else
                f.fw = std::string(1, inner[0]) + ".xx";
        }
        else if (find_word(low, "pkg") != npos || find_word(low, "emu") != npos || contains(inner, "@") ||
                 low == "duplex" || low == "pkg")
        {
            if (find_word(low, "pkg") != npos && f.format == LinkFormat::none)
                f.format = LinkFormat::pkg;
            if (find_word(low, "emu") != npos)
                f.tags.emplace_back("EMU");
            std::string first_name;
            // re.findall(r"@\s*([\w\-]+)", inner): the first one
            for (std::size_t p = inner.find('@'); p != npos && first_name.empty();
                 p = inner.find('@', p + 1))
            {
                const std::size_t q = skip_spaces(inner, p + 1);
                const std::size_t r = word_dash_run(inner, q);
                if (r > q)
                    first_name = inner.substr(q, r - q);
            }
            if (first_name.empty())
            {
                for (const std::string_view piece : split_names(inner))
                {
                    const std::string pl = lowered(piece);
                    if (pl != "pkg" && pl != "emu")
                    {
                        first_name = std::string(piece);
                        break;
                    }
                }
            }
            if (!first_name.empty())
                f.packager = packager_name(first_name);
        }
        else
        {
            consumed = false;
        }
        if (consumed)
        {
            const std::size_t at = work.find(tag.whole);
            if (at != npos)
                work.replace(at, tag.whole.size(), " ");
        }
    }
}

// 4b to 10: packager words, region, release, format words, backport, build, firmware, version.
void Parser::words()
{
    Taken t;
    // bare "by Psykzhack"
    if (take(by_at, &t))
        f.packager = packager_name(t.g[0]);
    // 5. region and "X to Y Languages"
    if (take(languages_at, &t))
    {
        f.region = t.g[0];
        for (char &c : f.region)
            c = text::upper(c);
        f.lang_from = f.region;
        f.lang_to = t.g[1];
        for (char &c : f.lang_to)
            c = text::upper(c);
    }
    if (take(region_at, &t) && f.region.empty())
        f.region = t.g[0];
    // 6. release number
    if (take(release_at, &t))
    {
        long n = 0;
        for (const char c : t.g[0])
            n = std::min(n * 10 + (c - '0'), 1000000L);
        f.release = static_cast<int>(n);
    }
    // 7. format words
    if (take_word("exfat"))
        f.format = LinkFormat::exfat;
    if (take(fpkg_at))
        f.format = LinkFormat::fpkg;
    if (take_word("ffpfs"))
        f.format = LinkFormat::ffpfs;
    if (take_word("pkg") && f.format == LinkFormat::none)
        f.format = LinkFormat::pkg;
    if (take_word("dump") && f.format == LinkFormat::none)
        f.format = LinkFormat::dump;
    // 8. backport (+ target); 'BackPort backport - AKIA Backport (exFAT)' says it twice
    while (take(backport_at, &t))
    {
        f.backport = true;
        if (t.has[0])
            f.backport_target = t.g[0] + "." + lowered(t.g[1]);
        else if (t.has[2])
            f.backport_target = t.g[2] + ".xx";
    }
    if (f.truncated)
    {
        // a 'Backport' cut off at the 63-byte limit
        const std::size_t cut =
            find_last_word_in(work, {"b", "ba", "bac", "back", "backp", "backpo", "backpor"});
        if (cut != npos)
        {
            work.erase(cut);
            work += ' ';
            f.backport = true;
        }
    }
    if (take(standard_at))
        f.standard_build = true;
    // 9. firmware tokens (left of the backport keyword = original build fw)
    struct Fw
    {
        std::string text;
        bool plus;
    };
    std::vector<Fw> fws;
    while (true)
    {
        if (take(fw_at, &t))
        {
            fws.push_back(Fw{t.g[0] + "." + lowered(t.g[1]), t.has[2]});
            continue;
        }
        if (take(fw_compact_at, &t))
        {
            fws.push_back(Fw{t.g[0] + ".xx", t.has[1]});
            continue;
        }
        break;
    }
    if (!fws.empty())
    {
        if (f.fw.empty())
            f.fw = fws.front().text;
        f.fw_plus = std::any_of(fws.begin(), fws.end(), [](const Fw &x) { return x.plus; });
        if (f.backport && f.backport_target.empty() && f.region.empty())
        {
            // Pippo '4.xx BackPort', PFS '4.xx Backport', 'Backport - 4.xx+': the one fw IS the target.
            // DLPS 'USA 12.xx BackPort' (region first) names the ORIGINAL build; target unknown.
            f.backport_target = fws.back().text;
            if (fws.size() == 1)
                f.fw.clear();
        }
    }
    // 10. version: vNN.NNN or bare NN.NNN(.NNN)
    if (take(version_v_at, &t) && f.version.empty())
        f.version = t.g[0];
    if (take(version_bare_at, &t) && f.version.empty())
        f.version = t.g[0];
    if (take_word("update"))
        set_kind(LinkKind::update);
}

// 11 and 12: kind words, then whatever is left is a free label (a DLC's name).
void Parser::kind_and_label()
{
    if (take(full_unlock_at)) // PFS 'FULL GAME UNLOCK (DUPLEX - PKG) - DLC'
        f.label = "Full game unlock";
    const bool has_game = take(game_at);
    if (has_game)
        while (take(base_or_game_at))
        {
        }
    Taken dlc;
    const bool dlcm = take(dlc_at, &dlc);
    while (dlcm && take(dlc_word_at))
    {
        // PFS repeats it: 'Data - DLC by Psykzhack - DLC'
    }
    if (dlcm)
    {
        const std::string low = lowered(dlc.all);
        if (contains(low, "all") || contains(low, "merged"))
            f.all_dlc = true;
    }
    if (kind_set && f.kind == LinkKind::update)
    {
    }
    else if (has_game && (dlcm || (kind_set && f.kind == LinkKind::game_dlc)))
    {
        set_kind(LinkKind::game_dlc);
    }
    else if (dlcm)
    {
        set_kind(LinkKind::dlc);
    }
    else if (kind_set && f.kind == LinkKind::game_dlc)
    {
    }
    else if (has_game)
    {
        set_kind(LinkKind::base);
    }
    else if (take_word("fix"))
    {
        set_kind(LinkKind::fix);
    }
    else
    {
        set_kind(LinkKind::base);
        f.kind_inferred = true;
    }
    // 12. leftovers: filler words, then a free label
    take_word("link");
    take(preorder_at);
    // re.sub(r"[()\[\]+,]|(?<=\s)-(?=\s)", " ", work)
    std::string left = work;
    for (std::size_t i = 0; i < work.size(); ++i)
    {
        const char c = work[i];
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '+' || c == ',')
            left[i] = ' ';
        else if (c == '-' && i > 0 && work[i - 1] == ' ' && i + 1 < work.size() && work[i + 1] == ' ')
            left[i] = ' ';
    }
    left = text::norm_space(left);
    // re.sub(r"(?i)\b(?:by|the)\b$", "", left)
    for (const char *tail : {"by", "the"})
    {
        const std::size_t n = std::string_view(tail).size();
        if (left.size() >= n && ieq_at(left, left.size() - n, tail) && !word_before(left, left.size() - n))
        {
            left.erase(left.size() - n);
            break;
        }
    }
    left = text::norm_space(left);
    if (!left.empty())
    {
        if (f.kind == LinkKind::dlc || f.kind == LinkKind::game_dlc)
            f.label = std::string(strip_chars(f.label.empty() ? left : f.label + " " + left, " "));
        else
            f.residue = left;
    }
}

void Parser::run(std::string_view name, std::string_view url_host)
{
    f.truncated = name.size() >= kLinkNameBytes;
    std::string s = text::norm_space(name);
    // 1. multipart suffix (always last)
    int part = 0;
    const std::size_t part_at = find_part(s, &part);
    if (part_at != npos)
    {
        f.part = part;
        s.erase(part_at);
        while (!s.empty() && (s.back() == ' ' || s.back() == '-'))
            s.pop_back();
    }
    else if (f.truncated)
    {
        const std::size_t cut = find_last_word_in(s, {"p", "pa", "par", "part"});
        if (cut != npos)
        {
            f.part_unknown = true;
            s.erase(cut);
        }
    }
    host_segment(s, url_host);
    // 3. size / embedded title id (only the PPSA26344 family)
    Taken t;
    if (take(size_at, &t))
    {
        f.size_text = t.g[0] + " " + t.g[1];
        for (char &c : f.size_text)
            c = text::upper(c);
    }
    if (take(title_id_at, &t))
        f.title_id = t.g[0];
    tags();
    words();
    kind_and_label();
}

} // namespace

LinkMeta parse_link(std::string_view name, std::string_view url_host)
{
    LinkMeta meta;
    Parser(&meta).run(name, url_host);
    return meta;
}

bool is_wrapper_host(std::string_view url_host)
{
    for (const std::string_view w : kWrapperHosts)
        if (url_host == w)
            return true;
    return false;
}

std::string release_key(const LinkMeta &m)
{
    std::string key;
    key.reserve(64);
    const auto add = [&](std::string_view part)
    {
        key += part;
        key += '\x1f';
    };
    add(m.region);
    add(m.lang_to);
    add(m.fw);
    add(m.backport ? "1" : "0");
    add(m.backport_target);
    add(std::to_string(static_cast<int>(m.kind)));
    add(m.all_dlc ? "1" : "0");
    add(std::to_string(static_cast<int>(m.format)));
    add(m.standard_build ? "1" : "0");
    add(m.version);
    add(std::to_string(m.release));
    add(m.packager);
    add(m.label);
    for (const std::string &tag : m.tags)
    {
        key += tag;
        key += ',';
    }
    return key;
}

namespace
{
// "12.xx" -> (12, -1), "7.61" -> (7, 61); false when unreadable.
bool split_fw(std::string_view fw, int *major, int *minor)
{
    const std::size_t dot = fw.find('.');
    if (dot == npos || dot == 0 || dot > 2 || fw.size() != dot + 3)
        return false;
    int a = 0;
    for (std::size_t i = 0; i < dot; ++i)
    {
        if (!is_digit(fw[i]))
            return false;
        a = a * 10 + (fw[i] - '0');
    }
    const std::string_view b = fw.substr(dot + 1);
    if (ieq(b, "xx"))
        *minor = -1;
    else if (is_digit(b[0]) && is_digit(b[1]))
        *minor = (b[0] - '0') * 10 + (b[1] - '0');
    else
        return false;
    *major = a;
    return true;
}
} // namespace

int fw_need(std::string_view fw)
{
    int major = 0;
    int minor = 0;
    if (!split_fw(fw, &major, &minor))
        return -1;
    return major * 100 + (minor < 0 ? 0 : minor);
}

int fw_console(std::string_view fw)
{
    int major = 0;
    int minor = 0;
    if (!split_fw(fw, &major, &minor))
        return -1;
    return major * 100 + (minor < 0 ? 99 : minor);
}

int fw_family(std::string_view fw)
{
    int major = 0;
    int minor = 0;
    return split_fw(fw, &major, &minor) ? major : -1;
}

const char *kind_label(LinkKind kind)
{
    switch (kind)
    {
    case LinkKind::base:
        return tr("Base");
    case LinkKind::game_dlc:
        return tr("Game + DLC");
    case LinkKind::dlc:
        return tr("DLC");
    case LinkKind::update:
        return tr("Update");
    case LinkKind::fix:
        return tr("Fix");
    }
    return "";
}

const char *format_label(LinkFormat format)
{
    switch (format)
    {
    case LinkFormat::none:
        return "";
    case LinkFormat::exfat:
        return "exFAT";
    case LinkFormat::fpkg:
        return "FPKG";
    case LinkFormat::pkg:
        return "PKG";
    case LinkFormat::dump:
        return "Dump";
    case LinkFormat::ffpfs:
        return "FFPFS";
    }
    return "";
}

std::vector<int> version_parts(std::string_view version)
{
    std::vector<int> out;
    if (version.empty())
        return out;
    int cur = 0;
    for (const char c : version)
    {
        if (c == '.')
        {
            out.push_back(cur);
            cur = 0;
        }
        else if (is_digit(c))
        {
            cur = std::min(cur * 10 + (c - '0'), 1000000);
        }
    }
    out.push_back(cur);
    return out;
}

} // namespace orbit
