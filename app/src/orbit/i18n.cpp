// Wings TV app - The interface language.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/i18n.hpp"

#include "core/save_file.hpp"
#include "orbit/json.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace orbit::i18n
{

namespace
{

constexpr std::array<std::string_view, 11> kCodes = {"en", "de", "es", "fr", "hu", "it",
                                                   "nl", "pl", "pt-BR", "ru", "tr"};

// Plural forms in CLDR's order of categories; an empty form falls back to other.
enum Category : std::uint8_t
{
    one,
    few,
    many,
    other,
};
using Forms = std::array<std::string, 4>;

std::string current = "en";
std::map<std::string, std::string, std::less<>> strings;
std::map<std::string, Forms, std::less<>> plurals;

// The integer rules of the shipped languages, as Intl.PluralRules has them.
// French and Brazilian Portuguese treat 0 as singular; they, Spanish and
// Italian use many for whole millions ("1 000 000 de jeux"); Russian and
// Polish have few and many.
Category category(long n)
{
    const long abs = n < 0 ? -n : n;
    const long ten = abs % 10;
    const long hundred = abs % 100;
    const bool millions = abs != 0 && abs % 1000000 == 0;
    if (current == "fr" || current == "pt-BR")
        return abs <= 1 ? one : millions ? many : other;
    if (current == "es" || current == "it")
        return abs == 1 ? one : millions ? many : other;
    if (current == "ru")
    {
        if (ten == 1 && hundred != 11)
            return one;
        if (ten >= 2 && ten <= 4 && (hundred < 12 || hundred > 14))
            return few;
        return many;
    }
    if (current == "pl")
    {
        if (abs == 1)
            return one;
        if (ten >= 2 && ten <= 4 && (hundred < 12 || hundred > 14))
            return few;
        return many;
    }
    return abs == 1 ? one : other;
}

// One pass, so a filled-in value (a title, a path) is never filled again.
std::string fill(std::string_view text, std::initializer_list<Var> vars,
                 const Var *extra = nullptr)
{
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == '{')
        {
            const std::size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos)
            {
                const std::string_view name = text.substr(i + 1, close - i - 1);
                const Var *match = nullptr;
                for (const Var &var : vars)
                {
                    if (var.name == name)
                        match = &var;
                }
                if (extra != nullptr && extra->name == name)
                    match = extra;
                if (match != nullptr)
                {
                    out += match->value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

// The app's fonts have no no-break space (French puts one before ? and :),
// so it becomes an ordinary space here.
std::string drawable(std::string text)
{
    for (std::size_t at = text.find("\xC2\xA0"); at != std::string::npos;
         at = text.find("\xC2\xA0", at + 1))
        text.replace(at, 2, " ");
    return text;
}

// The thousands separator, or empty when the language does not group.
std::string_view group_separator()
{
    if (current == "en")
        return ",";
    if (current == "fr" || current == "hu" || current == "pl" || current == "ru")
        return " "; // a space: the app's fonts have no no-break space
    return ".";
}

} // namespace

bool supported(std::string_view code)
{
    for (const std::string_view known : kCodes)
    {
        if (known == code)
            return true;
    }
    return false;
}

std::string_view from_system(int system_language)
{
    switch (system_language)
    {
    case 2:  // French
    case 22: // French (Canada)
        return "fr";
    case 3:  // Spanish
    case 20: // Spanish (Latin America)
        return "es";
    case 4:
        return "de";
    case 5:
        return "it";
    case 6:
        return "nl";
    case 7:  // Portuguese (Portugal): the closest catalogue
    case 17: // Portuguese (Brazil)
        return "pt-BR";
    case 8:
        return "ru";
    case 16:
        return "pl";
    case 19:
        return "tr";
    case 24:
        return "hu";
    default:
        return "en";
    }
}

bool load(const std::string &dir, std::string_view code)
{
    strings.clear();
    plurals.clear();
    current = "en";
    if (!supported(code) || code == "en")
        return code == "en";
    std::string data;
    json::Value root;
    if (!hui::save::read_file(dir + "/" + std::string(code) + ".json", &data) ||
        !json::parse(data, &root) || !root.is_object())
        return false;
    const std::vector<std::string> &keys = root.keys();
    for (std::size_t k = 0; k < keys.size(); ++k)
    {
        const json::Value &value = root[keys[k]];
        if (value.is_string())
        {
            strings.emplace(keys[k], drawable(value.as_string()));
            continue;
        }
        Forms forms;
        forms[one] = drawable(value.text("one"));
        forms[few] = drawable(value.text("few"));
        forms[many] = drawable(value.text("many"));
        forms[other] = drawable(value.text("other"));
        plurals.emplace(keys[k], std::move(forms));
    }
    current = std::string(code);
    return true;
}

std::string_view language()
{
    return current;
}

const char *tr(const char *english)
{
    const auto found = strings.find(std::string_view(english));
    return found == strings.end() ? english : found->second.c_str();
}

std::string_view tr(std::string_view english)
{
    const auto found = strings.find(english);
    return found == strings.end() ? english : std::string_view(found->second);
}

std::string tr(std::string_view english, std::initializer_list<Var> vars)
{
    return fill(tr(english), vars);
}

std::string trn(long n, std::string_view one_form, std::string_view other_form,
                std::initializer_list<Var> vars)
{
    std::string_view text = n == 1 ? one_form : other_form;
    const auto found = plurals.find(other_form);
    if (found != plurals.end())
    {
        const Forms &forms = found->second;
        const std::string &form = forms[category(n)];
        text = form.empty() ? std::string_view(forms[other]) : std::string_view(form);
    }
    const Var number{"count", count(n)};
    return fill(text, vars, &number);
}

std::string count(long value)
{
    const bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value);
    // Spanish, Hungarian, Italian and Polish group only from five digits.
    const std::size_t from = current == "es" || current == "hu" || current == "it" || current == "pl" ? 5 : 4;
    if (digits.size() >= from)
    {
        const std::string_view separator = group_separator();
        std::string grouped;
        const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
        grouped.append(digits, 0, lead);
        for (std::size_t i = lead; i < digits.size(); i += 3)
        {
            grouped += separator;
            grouped.append(digits, i, 3);
        }
        digits = std::move(grouped);
    }
    return negative ? "-" + digits : digits;
}

std::string decimal(double value)
{
    // Round first, then group the whole part like a count: 9.96 is 10.0.
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f", std::fabs(value));
    const std::string_view rounded = text;
    const std::size_t point = rounded.find('.');
    const long whole = std::strtol(text, nullptr, 10);
    const std::string_view fraction =
        point == std::string_view::npos ? "0" : rounded.substr(point + 1);
    return (value < 0 && std::fabs(value) >= 0.05 ? "-" : "") + count(whole) +
           (current == "en" ? "." : ",") + std::string(fraction);
}

std::string percent(double value)
{
    const std::string number = decimal(value);
    if (current == "tr")
        return "%" + number;
    if (current == "de" || current == "es" || current == "fr" || current == "ru")
        return number + " %";
    return number + "%";
}

std::string place(std::string_view name)
{
    // "USB storage 2" from Orbit, "USB 2" and "Extended storage 1" from ShadowMount.
    const auto numbered = [&](std::string_view prefix, std::string *number) {
        if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix)
            return false;
        const std::string_view rest = name.substr(prefix.size());
        for (const char c : rest)
        {
            if (c < '0' || c > '9')
                return false;
        }
        *number = std::string(rest);
        return true;
    };
    std::string number;
    if (numbered("USB storage ", &number) || numbered("USB ", &number))
        return tr("USB storage {number}", {{"number", number}});
    if (numbered("Extended storage ", &number))
        return tr("Extended storage {number}", {{"number", number}});
    return std::string(tr(name));
}

std::string genre(std::string_view names)
{
    std::string out;
    for (std::size_t start = 0; start <= names.size();)
    {
        std::size_t end = names.find(" / ", start);
        if (end == std::string_view::npos)
            end = names.size();
        if (!out.empty())
            out += " / ";
        out += tr(names.substr(start, end - start));
        start = end + 3;
    }
    return out;
}

std::string_view unit(bool giga)
{
    if (current == "ru")
        return giga ? "\xD0\x93\xD0\x91" : "\xD0\x9C\xD0\x91"; // ГБ, МБ
    if (current == "fr")
        return giga ? "Go" : "Mo";
    return giga ? "GB" : "MB";
}

std::string date(int year, int month, int day)
{
    // Intl.DateTimeFormat(code, {dateStyle: "medium"}) month names.
    static const char *const kMonths[][12] = {
        {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
        {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sept", "oct", "nov", "dic"},
        {"janv.", "f\xC3\xA9vr.", "mars", "avr.", "mai", "juin", "juil.", "ao\xC3\xBBt", "sept.",
         "oct.", "nov.", "d\xC3\xA9" "c."},
        {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"},
        {"jan", "feb", "mrt", "apr", "mei", "jun", "jul", "aug", "sep", "okt", "nov", "dec"},
        {"sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "pa\xC5\xBA", "lis", "gru"},
        {"jan.", "fev.", "mar.", "abr.", "mai.", "jun.", "jul.", "ago.", "set.", "out.", "nov.",
         "dez."},
        {"\xD1\x8F\xD0\xBD\xD0\xB2.", "\xD1\x84\xD0\xB5\xD0\xB2\xD1\x80.",
         "\xD0\xBC\xD0\xB0\xD1\x80.", "\xD0\xB0\xD0\xBF\xD1\x80.", "\xD0\xBC\xD0\xB0\xD1\x8F",
         "\xD0\xB8\xD1\x8E\xD0\xBD.", "\xD0\xB8\xD1\x8E\xD0\xBB.", "\xD0\xB0\xD0\xB2\xD0\xB3.",
         "\xD1\x81\xD0\xB5\xD0\xBD\xD1\x82.", "\xD0\xBE\xD0\xBA\xD1\x82.",
         "\xD0\xBD\xD0\xBE\xD1\x8F\xD0\xB1.", "\xD0\xB4\xD0\xB5\xD0\xBA."},
        {"Oca", "\xC5\x9E" "ub", "Mar", "Nis", "May", "Haz", "Tem", "A\xC4\x9Fu", "Eyl", "Eki", "Kas",
         "Ara"},
        {"jan.", "febr.", "márc.", "ápr.", "máj.", "jún.", "júl.", "aug.", "szept.", "okt.", "nov.", "dec."},
    };
    if (month < 1 || month > 12)
        return {};
    char text[64];
    const int m = month - 1;
    if (current == "de")
    {
        std::snprintf(text, sizeof(text), "%02d.%02d.%04d", day, month, year);
        return text;
    }
    const int row = current == "es"      ? 1
                    : current == "fr"    ? 2
                    : current == "it"    ? 3
                    : current == "nl"    ? 4
                    : current == "pl"    ? 5
                    : current == "pt-BR" ? 6
                    : current == "ru"    ? 7
                    : current == "tr"    ? 8
                    : current == "hu"    ? 9
                                         : 0;
    if (row == 0)
        std::snprintf(text, sizeof(text), "%s %d, %04d", kMonths[0][m], day, year);
    else if (current == "pt-BR")
        std::snprintf(text, sizeof(text), "%d de %s de %04d", day, kMonths[row][m], year);
    else if (current == "ru")
        std::snprintf(text, sizeof(text), "%d %s %04d \xD0\xB3.", day, kMonths[row][m], year);
    else if (current == "hu")
        std::snprintf(text, sizeof(text), "%04d. %s %d.", year, kMonths[row][m], day);
    else
        std::snprintf(text, sizeof(text), "%d %s %04d", day, kMonths[row][m], year);
    return text;
}

} // namespace orbit::i18n
