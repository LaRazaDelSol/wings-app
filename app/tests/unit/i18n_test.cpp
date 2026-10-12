// Wings TV app - The interface language: lookups, plurals and the
// number and date formats the browser version gets from Intl.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/i18n.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <stdlib.h>
#include <string>

namespace orbit
{
namespace
{

class I18nTest : public ::testing::Test
{
protected:
    I18nTest()
    {
        char name[] = "/tmp/orbit-i18n-XXXXXX";
        dir_ = mkdtemp(name);
    }
    ~I18nTest() override
    {
        // Every other test expects English.
        i18n::load(dir_, "en");
        std::system(("rm -rf '" + dir_ + "'").c_str());
    }
    // Writes <dir>/<code>.json and switches to it.
    void use(const char *code, const std::string &catalogue = "{}")
    {
        std::ofstream(dir_ + "/" + code + ".json", std::ios::binary) << catalogue;
        ASSERT_TRUE(i18n::load(dir_, code));
        ASSERT_EQ(i18n::language(), code);
    }
    std::string dir_;
};

TEST_F(I18nTest, EnglishIsTheKeyAndTheFallback)
{
    ASSERT_TRUE(i18n::load(dir_, "en"));
    EXPECT_STREQ(tr("Download to PS5"), "Download to PS5");
    EXPECT_EQ(tr("Saves to {path}", {{"path", "/data"}}), "Saves to /data");
    EXPECT_EQ(tr("Saves to {path}", {}), "Saves to {path}");
    EXPECT_EQ(trn(1, "{count} game", "{count} games"), "1 game");
    EXPECT_EQ(trn(0, "{count} game", "{count} games"), "0 games");
    EXPECT_EQ(trn(1200, "{count} game", "{count} games"), "1,200 games");
}

TEST_F(I18nTest, UnknownOrMissingLanguagesStayInEnglish)
{
    EXPECT_FALSE(i18n::load(dir_, "ja"));
    EXPECT_EQ(i18n::language(), "en");
    EXPECT_FALSE(i18n::load(dir_, "de")); // no de.json written
    EXPECT_EQ(i18n::language(), "en");
    std::ofstream(dir_ + "/fr.json") << "{ not json";
    EXPECT_FALSE(i18n::load(dir_, "fr"));
    EXPECT_EQ(i18n::language(), "en");
}

TEST_F(I18nTest, TranslatesAndFillsPlaceholders)
{
    use("de", R"({
        "Saves to {path}": "Speichert in {path}",
        "{count} games": {"one": "{count} Spiel", "other": "{count} Spiele"},
        "Internal storage": "Interner Speicher",
        "USB storage {number}": "USB-Speicher {number}",
        "Adventure": "Abenteuer"
    })");
    EXPECT_EQ(tr("Saves to {path}", {{"path", "/mnt/usb0"}}), "Speichert in /mnt/usb0");
    EXPECT_STREQ(tr("Not in the catalogue"), "Not in the catalogue");
    EXPECT_EQ(trn(1, "{count} game", "{count} games"), "1 Spiel");
    EXPECT_EQ(trn(1234, "{count} game", "{count} games"), "1.234 Spiele");
    EXPECT_EQ(i18n::place("Internal storage"), "Interner Speicher");
    EXPECT_EQ(i18n::place("USB storage 2"), "USB-Speicher 2");
    EXPECT_EQ(i18n::place("USB 3"), "USB-Speicher 3");
    EXPECT_EQ(i18n::place("USB Extended Storage"), "USB Extended Storage");
    EXPECT_EQ(i18n::genre("Adventure / Action"), "Abenteuer / Action");
}

TEST_F(I18nTest, FillsEachPlaceholderOnce)
{
    ASSERT_TRUE(i18n::load(dir_, "en"));
    // A title that happens to contain a placeholder is shown as it is.
    EXPECT_EQ(trn(3, "{count} game in {title}", "{count} games in {title}",
                  {{"title", "{count} Club"}}),
              "3 games in {count} Club");
    // As in the browser's tn(), the count wins over a variable of that name.
    EXPECT_EQ(trn(2, "{count} copy", "{count} copies", {{"count", "9"}}), "2 copies");
}

TEST_F(I18nTest, NoBreakSpacesBecomeSpacesTheFontsCanDraw)
{
    use("fr", "{\"Note:\": \"Note\xC2\xA0:\"}");
    EXPECT_STREQ(tr("Note:"), "Note :");
}

TEST_F(I18nTest, RussianAndPolishPluralsHaveFewAndMany)
{
    use("ru", R"({"{count} games": {"one": "{count} игра", "few": "{count} игры",
                                     "many": "{count} игр", "other": "{count} игры"}})");
    const auto games = [](long n) { return trn(n, "{count} game", "{count} games"); };
    EXPECT_EQ(games(1), "1 игра");
    EXPECT_EQ(games(21), "21 игра");
    EXPECT_EQ(games(2), "2 игры");
    EXPECT_EQ(games(24), "24 игры");
    EXPECT_EQ(games(5), "5 игр");
    EXPECT_EQ(games(11), "11 игр");
    EXPECT_EQ(games(12), "12 игр");
    EXPECT_EQ(games(111), "111 игр");
    EXPECT_EQ(games(0), "0 игр");
    EXPECT_EQ(games(1234), "1 234 игры");

    use("pl", R"({"{count} games": {"one": "{count} gra", "few": "{count} gry",
                                     "many": "{count} gier", "other": "{count} gry"}})");
    EXPECT_EQ(games(1), "1 gra");
    EXPECT_EQ(games(21), "21 gier"); // Polish: only exactly one is singular
    EXPECT_EQ(games(22), "22 gry");
    EXPECT_EQ(games(12), "12 gier");
    EXPECT_EQ(games(0), "0 gier");
}

TEST_F(I18nTest, RomanceLanguagesUseManyForWholeMillions)
{
    use("es", R"({"{count} games": {"one": "{count} juego", "many": "{count} de juegos",
                                     "other": "{count} juegos"}})");
    const auto games = [](long n) { return trn(n, "{count} game", "{count} games"); };
    EXPECT_EQ(games(1), "1 juego");
    EXPECT_EQ(games(0), "0 juegos");
    EXPECT_EQ(games(1000000), "1.000.000 de juegos");
    EXPECT_EQ(games(1000001), "1.000.001 juegos");

    use("fr", R"({"{count} games": {"one": "{count} jeu", "many": "{count} de jeux",
                                     "other": "{count} jeux"}})");
    EXPECT_EQ(games(0), "0 jeu"); // French: 0 is singular
    EXPECT_EQ(games(2), "2 jeux");
    EXPECT_EQ(games(2000000), "2 000 000 de jeux");
}

TEST_F(I18nTest, NumbersMatchIntlNumberFormat)
{
    ASSERT_TRUE(i18n::load(dir_, "en"));
    EXPECT_EQ(i18n::count(1234), "1,234");
    EXPECT_EQ(i18n::count(-1234567), "-1,234,567");
    EXPECT_EQ(i18n::decimal(1420.0), "1,420.0");
    EXPECT_EQ(i18n::decimal(9.96), "10.0");
    EXPECT_EQ(i18n::percent(12.5), "12.5%");
    EXPECT_EQ(i18n::unit(true), "GB");

    use("de");
    EXPECT_EQ(i18n::count(1234), "1.234");
    EXPECT_EQ(i18n::decimal(1420.0), "1.420,0");
    EXPECT_EQ(i18n::percent(12.5), "12,5 %");

    // Spanish, Italian and Polish group only from five digits.
    use("es");
    EXPECT_EQ(i18n::count(1234), "1234");
    EXPECT_EQ(i18n::count(12345), "12.345");
    use("it");
    EXPECT_EQ(i18n::count(1234), "1234");
    EXPECT_EQ(i18n::decimal(1420.0), "1420,0");
    EXPECT_EQ(i18n::percent(12.5), "12,5%");
    use("pl");
    EXPECT_EQ(i18n::count(1234), "1234");
    EXPECT_EQ(i18n::count(12345), "12 345");

    use("fr");
    EXPECT_EQ(i18n::count(1234), "1 234");
    EXPECT_EQ(i18n::unit(true), "Go");
    EXPECT_EQ(i18n::unit(false), "Mo");
    use("ru");
    EXPECT_EQ(i18n::decimal(1420.0), "1 420,0");
    EXPECT_EQ(i18n::unit(true), "ГБ");
    EXPECT_EQ(i18n::percent(12.5), "12,5 %");
    use("tr");
    EXPECT_EQ(i18n::count(1234), "1.234");
    EXPECT_EQ(i18n::percent(12.5), "%12,5");
}

TEST_F(I18nTest, DatesMatchTheBrowsersMediumStyle)
{
    ASSERT_TRUE(i18n::load(dir_, "en"));
    EXPECT_EQ(i18n::date(2026, 9, 5), "Sep 5, 2026");
    EXPECT_EQ(i18n::date(2026, 13, 5), "");
    use("de");
    EXPECT_EQ(i18n::date(2026, 9, 5), "05.09.2026");
    use("es");
    EXPECT_EQ(i18n::date(2026, 9, 5), "5 sept 2026");
    use("fr");
    EXPECT_EQ(i18n::date(2026, 2, 5), "5 févr. 2026");
    use("nl");
    EXPECT_EQ(i18n::date(2026, 3, 5), "5 mrt 2026");
    use("pl");
    EXPECT_EQ(i18n::date(2026, 10, 5), "5 paź 2026");
    use("pt-BR");
    EXPECT_EQ(i18n::date(2026, 9, 5), "5 de set. de 2026");
    use("ru");
    EXPECT_EQ(i18n::date(2026, 5, 5), "5 мая 2026 г.");
    use("tr");
    EXPECT_EQ(i18n::date(2026, 2, 5), "5 Şub 2026");
}

TEST(I18n, SystemLanguagesMapToTheClosestCatalogue)
{
    EXPECT_EQ(i18n::from_system(1), "en");  // English (United States)
    EXPECT_EQ(i18n::from_system(18), "en"); // English (United Kingdom)
    EXPECT_EQ(i18n::from_system(0), "en");  // Japanese: no catalogue yet
    EXPECT_EQ(i18n::from_system(4), "de");
    EXPECT_EQ(i18n::from_system(2), "fr");
    EXPECT_EQ(i18n::from_system(22), "fr");
    EXPECT_EQ(i18n::from_system(3), "es");
    EXPECT_EQ(i18n::from_system(20), "es");
    EXPECT_EQ(i18n::from_system(7), "pt-BR");
    EXPECT_EQ(i18n::from_system(17), "pt-BR");
    EXPECT_EQ(i18n::from_system(16), "pl");
    EXPECT_EQ(i18n::from_system(19), "tr");
    EXPECT_EQ(i18n::from_system(24), "hu");
    for (const int code : {0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 17, 19, 20, 22, 24})
        EXPECT_TRUE(i18n::supported(i18n::from_system(code)));
}

} // namespace
} // namespace orbit
