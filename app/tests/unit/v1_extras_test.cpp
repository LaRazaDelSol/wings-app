// Wings TV app - Pegasus version range, failed-install wording and the title list export.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/install_hint.hpp"
#include "orbit/pegasus_version.hpp"
#include "orbit/screens.hpp"
#include "orbit/title_export.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdlib.h>

namespace orbit
{
namespace
{

TEST(PegasusVersion, TestedRangeAndNeutralNote)
{
    EXPECT_EQ(pegasus_version_fit("1.11.0"), PegasusVersionFit::tested);
    EXPECT_EQ(pegasus_version_fit("v1.11.0"), PegasusVersionFit::tested);
    EXPECT_EQ(pegasus_version_fit("1.11.0 release"), PegasusVersionFit::tested);
    EXPECT_EQ(pegasus_version_fit("1.12.0"), PegasusVersionFit::untested);
    EXPECT_EQ(pegasus_version_fit("1.9.9"), PegasusVersionFit::untested);
    EXPECT_EQ(pegasus_version_fit("2.0"), PegasusVersionFit::untested);
    EXPECT_EQ(pegasus_version_fit(""), PegasusVersionFit::unknown);
    EXPECT_EQ(pegasus_version_fit("fake"), PegasusVersionFit::unknown);
    EXPECT_EQ(pegasus_version_fit("7"), PegasusVersionFit::unknown);
    EXPECT_TRUE(pegasus_version_note(PegasusVersionFit::tested).empty());
    EXPECT_TRUE(pegasus_version_note(PegasusVersionFit::unknown).empty());
    EXPECT_EQ(pegasus_version_note(PegasusVersionFit::untested), "Untested version, things may not work.");
    EXPECT_EQ(pegasus_tested_range_text(), "1.11.0");
}

TEST(InstallHint, MatchesInstallSelectionFailuresOnly)
{
    EXPECT_TRUE(looks_like_install_failure("Multiple installable packages found"));
    EXPECT_TRUE(looks_like_install_failure("NO PKG in archive"));
    EXPECT_TRUE(looks_like_install_failure("Too many files"));
    EXPECT_TRUE(looks_like_install_failure("could not Install"));
    EXPECT_FALSE(looks_like_install_failure("Connection reset"));
    EXPECT_FALSE(looks_like_install_failure(""));
    const std::string hint = install_failure_hint("multiple installable packages");
    EXPECT_EQ(hint,
              "Pegasus could not decide what to install from this archive. Files may have been removed by "
              "Pegasus. Open Files to look, or install manually.");
    EXPECT_TRUE(install_failure_hint("disk full").empty());
}

TEST(TitleExport, CleansLikeThePackerAndDedupes)
{
    EXPECT_EQ(clean_export_title("Game\xE2\x84\xA2 Deluxe [FPKG] v1.05 @DUPLEX (USA)"), "Game Deluxe");
    EXPECT_EQ(clean_export_title("Cool Game - 01.23.45"), "Cool Game");
    EXPECT_EQ(clean_export_title("Part 2.5 Remaster"), "Part 2.5 Remaster"); // not a version token
    EXPECT_EQ(clean_export_title("[PKG]"), "");

    std::vector<Package> packages(4);
    packages[0].title_id = "PPSA00002";
    packages[0].title = "Zed \"Quest\" [PKG]";
    packages[1].title_id = "CUSA00001";
    packages[1].title = "Alpha v1.00";
    packages[2].title_id = "PPSA00002"; // duplicate id: first wins
    packages[2].title = "Other";
    packages[3].title_id = "NPXS00001"; // not a game id
    packages[3].title = "System";
    const TitleExport list = build_title_export(packages);
    EXPECT_EQ(list.count, 2);
    EXPECT_FALSE(list.capped);
    EXPECT_EQ(list.json,
              "[{\"titleId\":\"CUSA00001\",\"title\":\"Alpha\"},"
              "{\"titleId\":\"PPSA00002\",\"title\":\"Zed \\\"Quest\\\"\"}]\n");

    const TitleExport small = build_title_export(packages, 70);
    EXPECT_EQ(small.count, 1);
    EXPECT_TRUE(small.capped);
}

TEST(TitleExport, PathsSitBesideThePrefsAndWriteAtomically)
{
    const auto paths = title_export_paths({"/download0/wings/linkprefs.v1", "/data/wings/linkprefs.v1"});
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], "/data/wings/titles.json");
    EXPECT_EQ(paths[1], "/download0/wings/titles.json");

    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    store.poll_once();
    store.load_catalog_once();
    const TitleExport list = build_title_export(store.state().packages);

    char name[] = "/tmp/wings-titles-XXXXXX";
    const char *dir = mkdtemp(name);
    ASSERT_NE(dir, nullptr);
    const std::string file = std::string(dir) + "/titles.json";
    EXPECT_EQ(write_title_export({file}, list.json), 1);
    std::ifstream in(file, std::ios::binary);
    std::stringstream read;
    read << in.rdbuf();
    EXPECT_EQ(read.str(), list.json);
    EXPECT_EQ(read.str().front(), '[');
    std::system((std::string("rm -rf '") + dir + "'").c_str());
}

} // namespace
} // namespace orbit
