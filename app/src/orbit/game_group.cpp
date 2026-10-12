// Wings TV app - One game, many packages: grouping the catalogue into games.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A port of norm_title / loose_title / group_games from
// docs/research/links-grouping.md section 3.3. Unicode NFKD is replaced by a
// generated folding table (title_fold.inc) covering Latin, punctuation, number
// forms and full-width letters; any other non-ASCII character becomes a space,
// which is what the reference ends up doing at its step 7.

#include "orbit/game_group.hpp"

#include "orbit/link_meta.hpp"
#include "orbit/link_text.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace orbit
{
namespace
{

using text::is_digit;
using text::npos;

struct Fold
{
    std::uint32_t cp;
    const char *ascii;
};

constexpr Fold kFold[] = {
#include "orbit/title_fold.inc"
};

bool combining(std::uint32_t cp)
{
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F);
}

// Steps 1 and 2: trademark signs to spaces, NFKD, combining marks dropped, lower case.
std::string fold(std::string_view t)
{
    std::string out;
    out.reserve(t.size());
    for (std::size_t i = 0; i < t.size();)
    {
        const auto b = static_cast<unsigned char>(t[i]);
        if (b < 0x80)
        {
            out += text::lower(t[i]);
            ++i;
            continue;
        }
        std::size_t n = 1;
        const std::uint32_t cp = text::decode(t, i, &n);
        i += n;
        if (combining(cp))
            continue;
        if (cp == 0x2122 || cp == 0xAE)
        {
            out += ' ';
            continue;
        }
        const Fold *end = kFold + sizeof(kFold) / sizeof(kFold[0]);
        const Fold *f = std::lower_bound(kFold, end, cp, [](const Fold &x, std::uint32_t c) { return x.cp < c; });
        if (f != end && f->cp == cp)
            out += f->ascii;
        else
            out += ' ';
    }
    return out;
}

bool is_word(char c)
{
    return is_digit(c) || text::is_alpha(c) || c == '_';
}

bool word_at(const std::string &s, std::size_t i)
{
    return i < s.size() && is_word(s[i]);
}

// \bv?\d{1,2}\.\d{2,3}(?:\.\d{1,3})*\b at i (ASCII text): the end, or npos.
std::size_t version_token_at(const std::string &s, std::size_t i)
{
    if (i >= s.size() || (i > 0 && is_word(s[i - 1])) || !is_word(s[i]))
        return npos;
    const auto digits = [&](std::size_t at, std::size_t n)
    {
        if (at + n > s.size())
            return false;
        for (std::size_t k = 0; k < n; ++k)
            if (!is_digit(s[at + k]))
                return false;
        return true;
    };
    // (?:\.\d{1,3})*\b from p, greedy with backtracking.
    struct Rep
    {
        const std::string &s;
        decltype(digits) &dig;
        std::size_t operator()(std::size_t p) const
        {
            if (p < s.size() && s[p] == '.')
            {
                for (std::size_t c = 3; c >= 1; --c)
                {
                    if (dig(p + 1, c))
                    {
                        const std::size_t r = (*this)(p + 1 + c);
                        if (r != npos)
                            return r;
                    }
                }
            }
            return word_at(s, p) ? npos : p;
        }
    };
    const Rep rep{s, digits};
    const auto core = [&](std::size_t j) -> std::size_t
    {
        for (std::size_t a = 2; a >= 1; --a)
        {
            if (!digits(j, a) || j + a >= s.size() || s[j + a] != '.')
                continue;
            for (std::size_t b = 3; b >= 2; --b)
            {
                if (!digits(j + a + 1, b))
                    continue;
                const std::size_t r = rep(j + a + 1 + b);
                if (r != npos)
                    return r;
            }
        }
        return npos;
    };
    if (s[i] == 'v' || s[i] == 'V')
    {
        const std::size_t r = core(i + 1);
        if (r != npos)
            return r;
    }
    return core(i);
}

std::string drop_version_tokens(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();)
    {
        const std::size_t e = version_token_at(s, i);
        if (e != npos)
        {
            out += ' ';
            i = e;
        }
        else
        {
            out += s[i];
            ++i;
        }
    }
    return out;
}

// \[[^\]]*\]|\([^)]*\) -> ' '
std::string drop_brackets(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();)
    {
        if (s[i] == '[' || s[i] == '(')
        {
            const std::size_t close = s.find(s[i] == '[' ? ']' : ')', i + 1);
            if (close != npos)
            {
                out += ' ';
                i = close + 1;
                continue;
            }
        }
        out += s[i];
        ++i;
    }
    return out;
}

std::vector<std::string> tokens(std::string_view s)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && s[i] == ' ')
            ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ')
            ++j;
        if (j > i)
            out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string join(const std::vector<std::string> &toks, std::size_t n)
{
    std::string out;
    for (std::size_t k = 0; k < n && k < toks.size(); ++k)
    {
        if (k)
            out += ' ';
        out += toks[k];
    }
    return out;
}

const char *roman(std::string_view w)
{
    static constexpr std::pair<std::string_view, const char *> kRoman[] = {
        {"ii", "2"},  {"iii", "3"}, {"iv", "4"},   {"vi", "6"},  {"vii", "7"},
        {"viii", "8"}, {"ix", "9"}, {"xi", "11"},  {"xii", "12"}, {"xiii", "13"},
        {"xiv", "14"}, {"xv", "15"}, {"xvi", "16"}, {"ll", "2"},  {"lll", "3"},
    };
    for (const auto &r : kRoman)
        if (w == r.first)
            return r.second;
    return nullptr;
}

// numbers(): the numbers of a loose title, years 1900-2100 left out ("007" = "7").
std::set<std::string> numbers(const std::vector<std::string> &toks)
{
    std::set<std::string> out;
    for (const std::string &w : toks)
    {
        if (!text::all_digits(w))
            continue;
        std::size_t z = 0;
        while (z + 1 < w.size() && w[z] == '0')
            ++z;
        const std::string n = w.substr(z);
        if (n.size() == 4)
        {
            const int v = ((n[0] - '0') * 10 + (n[1] - '0')) * 100 + (n[2] - '0') * 10 + (n[3] - '0');
            if (v >= 1900 && v <= 2100)
                continue;
        }
        out.insert(n);
    }
    return out;
}

constexpr std::string_view kRisk[] = {"bundle", "trilogy", "collection", "demo",    "trial",
                                      "soundtrack", "ost", "dlc",         "dlcs",    "season",
                                      "vol",    "chapter", "episode",    "unlock",  "pack",
                                      "bonus",  "+"};

std::set<std::string> risk_words(const std::vector<std::string> &toks)
{
    std::set<std::string> out;
    for (const std::string &w : toks)
        for (const std::string_view r : kRisk)
            if (w == r)
                out.insert(w);
    return out;
}

// Why two loose titles under one titleId are kept apart, or "" when they are not.
std::string split_reason(std::string_view a, std::string_view b)
{
    const std::vector<std::string> ta = tokens(a);
    const std::vector<std::string> tb = tokens(b);
    if (numbers(ta) != numbers(tb))
        return "different numbers";
    const std::set<std::string> ra = risk_words(ta);
    const std::set<std::string> rb = risk_words(tb);
    if (ra != rb)
    {
        std::string which;
        for (const std::string &w : ra)
            if (!rb.count(w))
                which = w;
        for (const std::string &w : rb)
            if (!ra.count(w))
                which = w;
        return "bundle word: " + which;
    }
    for (const std::string &w : ta)
        if (std::find(tb.begin(), tb.end(), w) != tb.end())
            return {};
    return "no shared word";
}

struct UnionFind
{
    std::vector<int> parent;
    explicit UnionFind(std::size_t n) : parent(n)
    {
        std::iota(parent.begin(), parent.end(), 0);
    }
    int find(int x)
    {
        while (parent[static_cast<std::size_t>(x)] != x)
        {
            parent[static_cast<std::size_t>(x)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
            x = parent[static_cast<std::size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b)
    {
        parent[static_cast<std::size_t>(find(a))] = find(b);
    }
};

std::string upper(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = text::upper(c);
    return out;
}

std::string without_spaces(std::string_view s)
{
    std::string out;
    for (const char c : s)
        if (c != ' ')
            out += c;
    return out;
}

// Stage 1 (titleId, split on evidence): a cluster root per package.
std::vector<int> stage_one(const std::vector<Package> &packages, const std::vector<std::string> &loose,
                           UnionFind *uf, std::map<std::string, std::vector<int>> *by_tid)
{
    for (std::size_t i = 0; i < packages.size(); ++i)
    {
        std::string key = upper(packages[i].title_id);
        if (key.empty())
            key = "\x1fnotid:" + std::to_string(i);
        (*by_tid)[key].push_back(static_cast<int>(i));
    }
    for (const auto &entry : *by_tid)
    {
        const std::vector<int> &ps = entry.second;
        for (std::size_t i = 0; i < ps.size(); ++i)
            for (std::size_t j = i + 1; j < ps.size(); ++j)
                if (same_title_id_compatible(loose[static_cast<std::size_t>(ps[i])],
                                             loose[static_cast<std::size_t>(ps[j])]))
                    uf->unite(ps[i], ps[j]);
    }
    std::vector<int> root(packages.size());
    for (std::size_t i = 0; i < packages.size(); ++i)
        root[i] = uf->find(static_cast<int>(i));
    return root;
}

int punctuation(std::string_view s)
{
    int n = 0;
    for (const char c : s)
        n += (c != ' ' && !is_word(c)) ? 1 : 0;
    return n;
}

} // namespace

std::string norm_title(std::string_view title)
{
    std::string t = drop_version_tokens(drop_brackets(fold(title)));
    // P.I. -> pi: a dot between two letters goes, other dots become spaces
    std::string u;
    u.reserve(t.size());
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] == '.')
        {
            const bool between = i > 0 && t[i - 1] >= 'a' && t[i - 1] <= 'z' && i + 1 < t.size() &&
                                 t[i + 1] >= 'a' && t[i + 1] <= 'z';
            if (!between)
                u += ' ';
            continue;
        }
        if (t[i] == '&')
        {
            u += " and ";
            continue;
        }
        if (t[i] == '\'')
            continue; // "'s" -> "s", other apostrophes dropped
        u += t[i];
    }
    // [^a-z0-9+]+ -> ' '
    for (char &c : u)
        if (!((c >= 'a' && c <= 'z') || is_digit(c) || c == '+'))
            c = ' ';
    std::vector<std::string> toks;
    for (std::string &w : tokens(u))
    {
        if (w == "ps4" || w == "ps5")
            continue;
        const char *r = roman(w);
        toks.push_back(r ? std::string(r) : std::move(w));
    }
    return join(toks, toks.size());
}

std::string loose_title(std::string_view title)
{
    const std::string n = norm_title(title);
    const std::vector<std::string> t = tokens(n);
    const std::size_t k = t.size();
    for (std::size_t j = 1; j < k; ++j)
    {
        const std::size_t left = k - j;
        bool cut = false;
        if (t.back() == "edition")
        {
            // (?:the\s+)?(?:digital deluxe|game of the year|complete|definitive) edition
            for (const bool the : {true, false})
            {
                std::size_t p = j;
                if (the)
                {
                    if (t[p] != "the")
                        continue;
                    ++p;
                }
                const std::size_t rest = k - 1 - p;
                if ((rest == 2 && t[p] == "digital" && t[p + 1] == "deluxe") ||
                    (rest == 4 && t[p] == "game" && t[p + 1] == "of" && t[p + 2] == "the" && t[p + 3] == "year") ||
                    (rest == 1 && (t[p] == "complete" || t[p] == "definitive")))
                    cut = true;
            }
            // [a-z0-9]+ edition
            if (!cut && left == 2)
                cut = std::all_of(t[j].begin(), t[j].end(), [](char c) { return c != '+'; });
        }
        if (!cut && left == 2 && (t[j] == "directors" || t[j] == "final") && t.back() == "cut")
            cut = true;
        if (!cut && ((left == 2 && t[j] == "digital" && t[j + 1] == "deluxe") || (left == 1 && t[j] == "deluxe")))
            cut = true;
        if (!cut && left == 1 && t[j] == "goty")
            cut = true;
        if (cut)
            return join(t, j);
    }
    return n;
}

bool same_title_id_compatible(std::string_view a, std::string_view b)
{
    return split_reason(a, b).empty();
}

std::string clean_title(std::string_view title)
{
    std::string t;
    t.reserve(title.size());
    for (std::size_t i = 0; i < title.size(); ++i)
    {
        if (title.substr(i, 3) == "\xE2\x84\xA2" || title.substr(i, 2) == "\xC2\xAE")
        {
            t += ' ';
            i += title[i] == '\xE2' ? 2 : 1;
            continue;
        }
        t += title[i];
    }
    // [..] groups
    std::string u;
    for (std::size_t i = 0; i < t.size();)
    {
        const std::size_t close = t[i] == '[' ? t.find(']', i + 1) : npos;
        if (close != npos)
        {
            u += ' ';
            i = close + 1;
            continue;
        }
        u += t[i];
        ++i;
    }
    u = text::norm_space(drop_version_tokens(u));
    while (!u.empty() && (u.back() == '-' || u.back() == ' '))
        u.pop_back();
    while (!u.empty() && (u.front() == '-' || u.front() == ' '))
        u.erase(0, 1);
    return u;
}

std::vector<std::string> description_regions(std::string_view d)
{
    std::vector<std::string> out;
    const auto add = [&](std::string_view word)
    {
        static constexpr std::pair<std::string_view, std::string_view> kMap[] = {
            {"USA", "USA"}, {"US", "USA"}, {"EUR", "EUR"}, {"JPN", "JPN"}, {"JP", "JPN"},
            {"ASIA", "ASIA"}, {"HK", "HK"}, {"KOR", "KOR"}};
        for (const auto &m : kMap)
        {
            if (word == m.first)
            {
                if (std::find(out.begin(), out.end(), m.second) == out.end())
                    out.emplace_back(m.second);
                return true;
            }
        }
        return false;
    };
    const auto first_word = [](std::string_view s)
    {
        std::size_t i = 0;
        while (i < s.size() && s[i] == ' ')
            ++i;
        std::size_t j = i;
        while (j < s.size() && text::is_alpha(s[j]))
            ++j;
        return s.substr(i, j - i);
    };
    const auto line_after = [&](std::size_t at)
    {
        const std::size_t e = d.find_first_of("\n<", at);
        return d.substr(at, (e == npos ? d.size() : e) - at);
    };
    for (std::size_t at = d.find("Region:"); at != npos; at = d.find("Region:", at + 1))
        add(first_word(line_after(at + 7)));
    for (std::size_t at = d.find("Tags:"); at != npos; at = d.find("Tags:", at + 1))
    {
        std::string_view line = line_after(at + 5);
        while (!line.empty())
        {
            const std::size_t comma = line.find(',');
            add(first_word(line.substr(0, comma)));
            if (comma == npos)
                break;
            line.remove_prefix(comma + 1);
        }
    }
    return out;
}

GameIndex group_games(const std::vector<Package> &packages)
{
    GameIndex out;
    const std::size_t n = packages.size();
    std::vector<std::string> loose(n);
    for (std::size_t i = 0; i < n; ++i)
        loose[i] = loose_title(packages[i].title);
    UnionFind uf(n);
    std::map<std::string, std::vector<int>> by_tid;
    const std::vector<int> root1 = stage_one(packages, loose, &uf, &by_tid);
    // Stage 1 clusters in order of their first package.
    std::vector<int> cluster_roots;
    std::unordered_map<int, std::vector<int>> members;
    for (std::size_t i = 0; i < n; ++i)
    {
        auto &m = members[root1[i]];
        if (m.empty())
            cluster_roots.push_back(root1[i]);
        m.push_back(static_cast<int>(i));
    }
    // Stage 2: join clusters whose canonical loose title is equal (spaces ignored).
    std::unordered_map<int, std::string> canon_of;
    std::unordered_map<std::string, int> first_with;
    for (const int r : cluster_roots)
    {
        // The most frequent loose title; ties: shorter, then first seen.
        std::vector<std::pair<std::string, int>> counts;
        for (const int p : members[r])
        {
            const std::string &l = loose[static_cast<std::size_t>(p)];
            auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &c) { return c.first == l; });
            if (it == counts.end())
                counts.emplace_back(l, 1);
            else
                ++it->second;
        }
        std::stable_sort(counts.begin(), counts.end(),
                         [](const auto &a, const auto &b)
                         { return a.second != b.second ? a.second > b.second : a.first.size() < b.first.size(); });
        const std::string canon = counts.front().first;
        canon_of[r] = canon;
        if (canon.empty())
            continue;
        const std::string key = without_spaces(canon);
        const auto it = first_with.find(key);
        if (it == first_with.end())
            first_with.emplace(key, r);
        else
            uf.unite(r, it->second);
    }
    // Final games in order of first package.
    std::unordered_map<int, int> game_of_root;
    out.game_of.assign(n, -1);
    for (std::size_t i = 0; i < n; ++i)
    {
        const int r = uf.find(static_cast<int>(i));
        auto it = game_of_root.find(r);
        if (it == game_of_root.end())
        {
            it = game_of_root.emplace(r, static_cast<int>(out.games.size())).first;
            out.games.emplace_back();
            out.games.back().canon = canon_of[root1[i]];
        }
        out.game_of[i] = it->second;
        out.games[static_cast<std::size_t>(it->second)].packages.push_back(static_cast<int>(i));
    }
    // Merged fields.
    for (GameGroup &g : out.games)
    {
        const Package &first = packages[static_cast<std::size_t>(g.packages.front())];
        g.id = g.canon.empty() ? "p:" + first.id : "g:" + without_spaces(g.canon);
        std::vector<std::pair<std::string, int>> titles;
        for (const int pi : g.packages)
        {
            const Package &p = packages[static_cast<std::size_t>(pi)];
            const std::string tid = upper(p.title_id);
            if (!tid.empty() && std::find(g.title_ids.begin(), g.title_ids.end(), tid) == g.title_ids.end())
                g.title_ids.push_back(tid);
            if (g.cover.empty() && !p.poster_url.empty())
                g.cover = p.poster_url;
            if (p.description.size() > g.description.size())
                g.description = p.description;
            if (std::find(g.sources.begin(), g.sources.end(), p.source_name) == g.sources.end())
                g.sources.push_back(p.source_name);
            if (!p.version.empty() && std::find(g.versions.begin(), g.versions.end(), p.version) == g.versions.end())
                g.versions.push_back(p.version);
            for (const std::string &r : description_regions(p.description))
                if (std::find(g.regions.begin(), g.regions.end(), r) == g.regions.end())
                    g.regions.push_back(r);
            g.link_count += p.link_count;
            g.size_bytes = std::max(g.size_bytes, p.size_bytes);
            g.has_pkg = g.has_pkg || p.has_pkg;
            g.has_automated = g.has_automated || p.has_automated;
            g.has_supported = g.has_supported || p.has_supported;
            const std::string clean = clean_title(p.title);
            auto it = std::find_if(titles.begin(), titles.end(), [&](const auto &t) { return t.first == clean; });
            if (it == titles.end())
                titles.emplace_back(clean, 1);
            else
                ++it->second;
        }
        std::sort(g.title_ids.begin(), g.title_ids.end());
        std::stable_sort(g.versions.begin(), g.versions.end(),
                         [](const std::string &a, const std::string &b)
                         { return version_parts(b) < version_parts(a); });
        std::stable_sort(titles.begin(), titles.end(),
                         [](const auto &a, const auto &b)
                         {
                             if (a.second != b.second)
                                 return a.second > b.second;
                             return punctuation(a.first) > punctuation(b.first);
                         });
        g.title = titles.front().first.empty() ? first.title : titles.front().first;
    }
    return out;
}

std::vector<ReviewItem> review_list(const std::vector<Package> &packages, const GameIndex &index)
{
    std::vector<ReviewItem> out;
    const auto add_title = [](std::vector<std::string> *v, const std::string &t)
    {
        if (std::find(v->begin(), v->end(), t) == v->end())
            v->push_back(t);
    };
    // Splits: one titleId whose packages ended in several games.
    std::map<std::string, std::vector<int>> by_tid;
    for (std::size_t i = 0; i < packages.size(); ++i)
        if (!packages[i].title_id.empty())
            by_tid[upper(packages[i].title_id)].push_back(static_cast<int>(i));
    for (const auto &entry : by_tid)
    {
        std::vector<int> games;
        for (const int p : entry.second)
            if (std::find(games.begin(), games.end(), index.game_of[static_cast<std::size_t>(p)]) == games.end())
                games.push_back(index.game_of[static_cast<std::size_t>(p)]);
        if (games.size() < 2)
            continue;
        ReviewItem item;
        item.kind = ReviewItem::Kind::split;
        item.title_ids.push_back(entry.first);
        for (const int p : entry.second)
            add_title(&item.titles, packages[static_cast<std::size_t>(p)].title);
        for (const int g : games)
            item.games.push_back(index.games[static_cast<std::size_t>(g)].id);
        const std::string a = loose_title(packages[static_cast<std::size_t>(entry.second.front())].title);
        for (const int p : entry.second)
        {
            const std::string why = split_reason(a, loose_title(packages[static_cast<std::size_t>(p)].title));
            if (!why.empty())
            {
                item.reason = why;
                break;
            }
        }
        if (item.reason.empty())
            item.reason = "split by another package";
        out.push_back(std::move(item));
    }
    // Joins: one game made of several titleIds.
    for (const GameGroup &g : index.games)
    {
        if (g.title_ids.size() < 2)
            continue;
        ReviewItem item;
        item.kind = ReviewItem::Kind::join;
        item.title_ids = g.title_ids;
        item.games.push_back(g.id);
        std::set<std::string> norms;
        for (const int p : g.packages)
        {
            add_title(&item.titles, packages[static_cast<std::size_t>(p)].title);
            norms.insert(norm_title(packages[static_cast<std::size_t>(p)].title));
        }
        item.reason = norms.size() > 1 ? "edition" : "same title";
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace orbit
