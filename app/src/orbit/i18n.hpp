// Wings TV app - The interface language.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// English text is the key, as in the browser version, and both read the same
// catalogues (i18n/<code>.json, packaged as assets/i18n). A missing
// translation shows the English, and Orbit's fixed service messages translate
// the same way.
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace orbit::i18n
{

// One placeholder value: tr("Saves to {path}", {{"path", where}}).
struct Var
{
    std::string_view name;
    std::string value;
};

// The language codes Orbit ships, as the catalogue files name them.
bool supported(std::string_view code);
// The closest supported language for a PS5 system language setting
// (SCE_SYSTEM_PARAM_LANG_*); English for the rest.
std::string_view from_system(int system_language);
// Loads <dir>/<code>.json; English needs no file. On failure the interface
// stays in English and false is returned.
bool load(const std::string &dir, std::string_view code);
std::string_view language();

// The translation, or the English; valid until the next load. A literal
// gives a C string, so it fits wherever the screens keep text.
const char *tr(const char *english);
std::string_view tr(std::string_view english);
// With {name} placeholders filled in.
std::string tr(std::string_view english, std::initializer_list<Var> vars);
// Plural: English singular and plural forms; {count} is filled in.
std::string trn(long count, std::string_view one, std::string_view other,
                std::initializer_list<Var> vars = {});

// 1,234 in English, 1.234 in German, 1 234 in French.
std::string count(long value);
// One decimal: 49.4 in English, 49,4 in German.
std::string decimal(double value);
// One decimal and the percent sign where the language puts it: 12.5%,
// 12,5 % in German, %12,5 in Turkish.
std::string percent(double value);
// GB and MB as the language writes them (ГБ in Russian, Go in French).
std::string_view unit(bool giga);
// Drive and location names from Orbit and ShadowMount, numbered ones too.
std::string place(std::string_view name);
// Catalogue genres combine names: "Adventure / Action".
std::string genre(std::string_view names);
// A day as the browser's medium date style writes it: "Sep 5, 2026",
// "05.09.2026", "5 sept. 2026", "5 сент. 2026 г.".
std::string date(int year, int month, int day);

} // namespace orbit::i18n

namespace orbit
{
// Every screen translates, so the names are available unqualified.
using i18n::tr;
using i18n::trn;
} // namespace orbit
