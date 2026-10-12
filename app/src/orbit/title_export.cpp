// Wings TV app - Title list export (see title_export.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/title_export.hpp"

#include "core/save_file.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace orbit
{

namespace
{

bool word_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::size_t digit_run(std::string_view s, std::size_t i)
{
    std::size_t n = 0;
    while (i + n < s.size() && s[i + n] >= '0' && s[i + n] <= '9')
        ++n;
    return n;
}

// Length of a version token at s[i] (v?\d{1,2}\.\d{2,3}(\.\d+)* between word boundaries), or 0.
std::size_t version_at(std::string_view s, std::size_t i)
{
    if (i > 0 && word_char(s[i - 1]))
        return 0;
    std::size_t p = i;
    if (p < s.size() && (s[p] == 'v' || s[p] == 'V'))
        ++p;
    std::size_t n = digit_run(s, p);
    if (n < 1 || n > 2 || p + n >= s.size() || s[p + n] != '.')
        return 0;
    p += n + 1;
    n = digit_run(s, p);
    if (n < 2 || n > 3)
        return 0;
    p += n;
    std::size_t best = 0;
    for (;;)
    {
        if (p >= s.size() || !word_char(s[p]))
            best = p;
        const std::size_t more = p < s.size() && s[p] == '.' ? digit_run(s, p + 1) : 0;
        if (more == 0)
            break;
        p += 1 + more;
    }
    return best == 0 ? 0 : best - i;
}

void append_json_string(std::string &out, std::string_view text)
{
    static const char *const kHex = "0123456789abcdef";
    out += '"';
    for (const char ch : text)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += ch;
        }
        else if (c < 0x20)
        {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
        else
            out += ch;
    }
    out += '"';
}

} // namespace

std::string clean_export_title(std::string_view title)
{
    // Trademark, registered and copyright signs (UTF-8 E2 84 A2, C2 AE, C2 A9).
    std::string t;
    for (std::size_t i = 0; i < title.size();)
    {
        if (title.compare(i, 3, "\xE2\x84\xA2") == 0)
            i += 3;
        else if (title.compare(i, 2, "\xC2\xAE") == 0 || title.compare(i, 2, "\xC2\xA9") == 0)
            i += 2;
        else
            t += title[i++];
    }
    // [..] and (..) groups become a space.
    std::string u;
    for (std::size_t i = 0; i < t.size();)
    {
        if (t[i] == '[' || t[i] == '(')
        {
            const std::size_t close = t.find(t[i] == '[' ? ']' : ')', i + 1);
            if (close != std::string::npos)
            {
                u += ' ';
                i = close + 1;
                continue;
            }
        }
        u += t[i++];
    }
    // @packager tags.
    std::string v;
    for (std::size_t i = 0; i < u.size();)
    {
        if (u[i] == '@' && i + 1 < u.size() && word_char(u[i + 1]))
        {
            std::size_t j = i + 1;
            while (j < u.size() && word_char(u[j]))
                ++j;
            v += ' ';
            i = j;
        }
        else
            v += u[i++];
    }
    // Version numbers.
    std::string w;
    for (std::size_t i = 0; i < v.size();)
    {
        if (const std::size_t n = version_at(v, i); n > 0)
        {
            w += ' ';
            i += n;
        }
        else
            w += v[i++];
    }
    // Collapse whitespace, strip " -_.,".
    std::string out;
    bool space = false;
    for (const char c : w)
    {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            space = true;
        else
        {
            if (space && !out.empty())
                out += ' ';
            space = false;
            out += c;
        }
    }
    const char *const strip = " -_.,";
    const std::size_t first = out.find_first_not_of(strip);
    if (first == std::string::npos)
        return std::string();
    return out.substr(first, out.find_last_not_of(strip) - first + 1);
}

TitleExport build_title_export(const std::vector<Package> &packages, std::size_t max_bytes)
{
    std::map<std::string, std::string> seen; // sorted by titleId
    for (const Package &p : packages)
    {
        const std::string &id = p.title_id;
        if (id.size() < 4 || (id.compare(0, 4, "PPSA") != 0 && id.compare(0, 4, "CUSA") != 0))
            continue;
        if (seen.count(id) != 0)
            continue;
        std::string title = clean_export_title(p.title);
        if (!title.empty())
            seen.emplace(id, std::move(title));
    }
    TitleExport out;
    out.json = "[";
    for (const auto &entry : seen)
    {
        std::string row = out.count > 0 ? "," : "";
        row += "{\"titleId\":";
        append_json_string(row, entry.first);
        row += ",\"title\":";
        append_json_string(row, entry.second);
        row += "}";
        if (out.json.size() + row.size() + 2 > max_bytes)
        {
            out.capped = true;
            break;
        }
        out.json += row;
        ++out.count;
    }
    out.json += "]\n";
    return out;
}

std::vector<std::string> title_export_paths(const std::vector<std::string> &sibling_paths)
{
    std::vector<std::string> out;
    for (const std::string &path : sibling_paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash == std::string::npos || slash == 0)
            continue;
        const std::string target = path.substr(0, slash) + "/titles.json";
        if (std::find(out.begin(), out.end(), target) == out.end())
            out.push_back(target);
    }
    // The shared /data/wings copy first: it is the one Pegasus DL's file manager sees.
    std::stable_partition(out.begin(), out.end(),
                          [](const std::string &p) { return p.rfind("/data/wings/", 0) == 0; });
    return out;
}

int write_title_export(const std::vector<std::string> &paths, const std::string &json)
{
    int written = 0;
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, json).empty())
            ++written;
    }
    return written;
}

} // namespace orbit
