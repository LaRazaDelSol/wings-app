// Wings TV app - Byte-level text scanning shared by the link parser and the title normaliser.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal to link_meta.cpp and game_group.cpp. The reference code (Python)
// uses regular expressions with Unicode \w, \s and \b; these helpers give the
// same answers on UTF-8 bytes: ASCII exactly, other code points by a small
// table of letter ranges (the catalogue's names are ASCII apart from dashes).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace orbit::text
{

constexpr std::size_t npos = std::string_view::npos;

inline bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}
inline bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}
inline char upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}
inline std::string lowered(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = lower(c);
    return out;
}

// One UTF-8 sequence at i: the code point and its length (a stray byte is itself, length 1).
inline std::uint32_t decode(std::string_view s, std::size_t i, std::size_t *len)
{
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 1;
    std::uint32_t cp = b;
    if (b >= 0xC0 && b < 0xE0)
    {
        n = 2;
        cp = b & 0x1Fu;
    }
    else if (b >= 0xE0 && b < 0xF0)
    {
        n = 3;
        cp = b & 0x0Fu;
    }
    else if (b >= 0xF0 && b < 0xF8)
    {
        n = 4;
        cp = b & 0x07u;
    }
    if (n > 1)
    {
        if (i + n > s.size())
        {
            *len = 1;
            return b;
        }
        for (std::size_t k = 1; k < n; ++k)
        {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0u) != 0x80u)
            {
                *len = 1;
                return b;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
    }
    *len = n;
    return cp;
}

// Python's str.isalnum() for the scripts that can appear in a catalogue name.
inline bool cp_is_word(std::uint32_t cp)
{
    if (cp < 0x80)
    {
        const char c = static_cast<char>(cp);
        return is_digit(c) || is_alpha(c) || c == '_';
    }
    if (cp == 0xAA || cp == 0xB5 || cp == 0xBA || cp == 0xB2 || cp == 0xB3 || cp == 0xB9 ||
        (cp >= 0xBC && cp <= 0xBE))
        return true;
    if (cp >= 0xC0 && cp <= 0x2C1)
        return cp != 0xD7 && cp != 0xF7;
    return (cp >= 0x370 && cp <= 0x3FF && cp != 0x37E && cp != 0x387) || // Greek
           (cp >= 0x400 && cp <= 0x52F && (cp < 0x482 || cp > 0x489)) ||  // Cyrillic
           (cp >= 0x5D0 && cp <= 0x5EA) || (cp >= 0x620 && cp <= 0x669) ||
           (cp >= 0xE01 && cp <= 0xE30) || (cp >= 0x1E00 && cp <= 0x1FBC) ||
           (cp >= 0x2160 && cp <= 0x2188) || (cp >= 0x2460 && cp <= 0x249B) ||
           (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xFF10 && cp <= 0xFF19) ||
           (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A) ||
           (cp >= 0xFF66 && cp <= 0xFF9F);
}

// Python's \s.
inline bool cp_is_space(std::uint32_t cp)
{
    return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// \w at byte i (false past the end).
inline bool word_at(std::string_view s, std::size_t i)
{
    if (i >= s.size())
        return false;
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80)
        return is_digit(s[i]) || is_alpha(s[i]) || s[i] == '_';
    if ((b & 0xC0u) == 0x80u)
        return false; // inside a sequence: never a match position in practice
    std::size_t n = 0;
    return cp_is_word(decode(s, i, &n));
}

// \w for the character that ends just before byte i (false at the start).
inline bool word_before(std::string_view s, std::size_t i)
{
    if (i == 0 || i > s.size())
        return false;
    std::size_t j = i - 1;
    if (static_cast<unsigned char>(s[j]) < 0x80)
        return word_at(s, j);
    while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0u) == 0x80u && i - j < 4)
        --j;
    return word_at(s, j);
}

// \b at byte i.
inline bool boundary(std::string_view s, std::size_t i)
{
    return word_before(s, i) != word_at(s, i);
}

// (?<![\w.]) at byte i.
inline bool not_after_word_or_dot(std::string_view s, std::size_t i)
{
    return !(i > 0 && s[i - 1] == '.') && !word_before(s, i);
}

// The end of the run of [\w.] characters that starts at i.
inline std::size_t word_dot_run(std::string_view s, std::size_t i)
{
    while (i < s.size())
    {
        if (s[i] == '.')
        {
            ++i;
            continue;
        }
        if (!word_at(s, i))
            break;
        std::size_t n = 1;
        if (static_cast<unsigned char>(s[i]) >= 0x80)
            decode(s, i, &n);
        i += n;
    }
    return i;
}

// The end of the run of \w characters that starts at i.
inline std::size_t word_run(std::string_view s, std::size_t i)
{
    while (i < s.size() && word_at(s, i))
    {
        std::size_t n = 1;
        if (static_cast<unsigned char>(s[i]) >= 0x80)
            decode(s, i, &n);
        i += n;
    }
    return i;
}

// Case-insensitive ASCII literal at i; `lit` is lower case.
inline bool ieq_at(std::string_view s, std::size_t i, std::string_view lit)
{
    if (i > s.size() || s.size() - i < lit.size())
        return false;
    for (std::size_t k = 0; k < lit.size(); ++k)
        if (lower(s[i + k]) != lit[k])
            return false;
    return true;
}

inline bool ieq(std::string_view a, std::string_view lit)
{
    return a.size() == lit.size() && ieq_at(a, 0, lit);
}

// The first position >= i that is not an ASCII space (the parser's \s after norm_space).
inline std::size_t skip_spaces(std::string_view s, std::size_t i)
{
    while (i < s.size() && s[i] == ' ')
        ++i;
    return i;
}

inline bool all_digits(std::string_view s)
{
    if (s.empty())
        return false;
    for (const char c : s)
        if (!is_digit(c))
            return false;
    return true;
}

// Python's re.sub(r"\s+", " ", s).strip() after the en and em dashes became '-'.
inline std::string norm_space(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool pending = false;
    for (std::size_t i = 0; i < s.size();)
    {
        std::size_t n = 1;
        const std::uint32_t cp = static_cast<unsigned char>(s[i]) < 0x80
                                     ? static_cast<unsigned char>(s[i])
                                     : decode(s, i, &n);
        if (cp_is_space(cp))
        {
            pending = !out.empty();
        }
        else
        {
            if (pending)
                out += ' ';
            pending = false;
            if (cp == 0x2013 || cp == 0x2014)
                out += '-';
            else
                out.append(s.substr(i, n));
        }
        i += n;
    }
    return out;
}

} // namespace orbit::text
