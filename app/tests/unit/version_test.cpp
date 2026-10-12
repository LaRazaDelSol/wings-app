// Wings TV app - The app version and the build stamp: param.json, release tags, assets/build-info.json.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/version.hpp"

#include <gtest/gtest.h>

#include <string>

namespace hui
{
namespace
{

TEST(Version, ReadsContentVersionFromParamJson)
{
    EXPECT_EQ(content_version("{\n  \"contentVersion\": \"00.001.001\",\n  \"x\": 1\n}"), "00.001.001");
    EXPECT_EQ(content_version("{\"contentVersion\":\"12.034.567\"}"), "12.034.567");
    EXPECT_EQ(content_version("{\"masterVersion\": \"01.00\"}"), "");           // the key is missing
    EXPECT_EQ(content_version("{\"contentVersion\": \"0.1.1\"}"), "");           // not NN.NNN.NNN
    EXPECT_EQ(content_version("{\"contentVersion\": \"00.001.0x1\"}"), "");
    EXPECT_EQ(content_version("{\"contentVersion\": \"00.001.001"), "");         // unterminated
    EXPECT_EQ(content_version(""), "");
}

TEST(Version, ContentVersionAndReleaseTagAgree)
{
    int content[3];
    int tag[3];
    ASSERT_TRUE(parse_content_version("00.001.001", content));
    ASSERT_TRUE(parse_release_tag("v0.1.1", tag));
    EXPECT_EQ(compare_versions(content, tag), 0);
    EXPECT_EQ(content[0], 0);
    EXPECT_EQ(content[1], 1);
    EXPECT_EQ(content[2], 1);
    ASSERT_TRUE(parse_content_version("01.020.300", content));
    ASSERT_TRUE(parse_release_tag("v1.20.300", tag));
    EXPECT_EQ(compare_versions(content, tag), 0);
    EXPECT_EQ(release_version_text("00.001.001"), "0.1.1");
    EXPECT_EQ(release_version_text("01.020.300"), "1.20.300");
    EXPECT_EQ(release_version_text("garbage"), "");
}

TEST(Version, ReleaseTagMustBeExactlyThreeNumbers)
{
    int v[3];
    EXPECT_TRUE(parse_release_tag("v0.1.0", v));
    EXPECT_TRUE(parse_release_tag("v10.200.999", v));
    for (const char *bad : {"", "v", "0.1.1", "v1.2", "v1.2.3.4", "v1..2", "v1.2.", "v1.2.3-rc1", "V1.2.3",
                            "v1.2.3 ", "v1.2.x", "v1000.2.3", "v-1.2.3", "v1.2.3\n"})
        EXPECT_FALSE(parse_release_tag(bad, v)) << bad;
}

TEST(Version, ComparesNumbersNotText)
{
    int a[3];
    int b[3];
    ASSERT_TRUE(parse_release_tag("v0.9.0", a));
    ASSERT_TRUE(parse_release_tag("v0.10.0", b));
    EXPECT_EQ(compare_versions(a, b), -1); // 10 is more than 9
    EXPECT_EQ(compare_versions(b, a), 1);
    ASSERT_TRUE(parse_release_tag("v1.0.0", a));
    EXPECT_EQ(compare_versions(a, b), 1);
    EXPECT_EQ(compare_versions(a, a), 0);
}

TEST(BuildInfo, ParsesTheStampTheBuildWrites)
{
    BuildInfo info = parse_build_info("{\"build\":\"20261012-203105\",\"channel\":\"test   \"}\n");
    EXPECT_TRUE(info.known);
    EXPECT_FALSE(info.release);
    EXPECT_EQ(info.build, "20261012-203105");
    info = parse_build_info("{\"build\":\"20261012-203105\",\"channel\":\"release\"}\n");
    EXPECT_TRUE(info.known);
    EXPECT_TRUE(info.release);
}

TEST(BuildInfo, RejectsAnythingElse)
{
    for (const char *bad : {"", "{}", "{\"build\":\"20261012-203105\"}", "{\"channel\":\"test   \"}",
                            "{\"build\":\"2026-10-12\",\"channel\":\"test   \"}",
                            "{\"build\":\"20261012-20310x\",\"channel\":\"test   \"}",
                            "{\"build\":\"20261012-203105\",\"channel\":\"beta   \"}",
                            "{\"build\":\"20261012-203105\",\"channel\":\"\"}",
                            "{\"build\":\"20261012-203105\",\"channel\":\"rele\"}"})
    {
        const BuildInfo info = parse_build_info(bad);
        EXPECT_FALSE(info.known) << bad;
        EXPECT_FALSE(info.release) << bad;
        EXPECT_TRUE(info.build.empty()) << bad;
    }
}

TEST(BuildInfo, LabelAndTag)
{
    const BuildInfo test_build = parse_build_info("{\"build\":\"20261012-203105\",\"channel\":\"test   \"}\n");
    const BuildInfo release_build = parse_build_info("{\"build\":\"20261012-203105\",\"channel\":\"release\"}\n");
    const BuildInfo none;
    // A release build reads as the plain version; a test build says what it is.
    EXPECT_EQ(version_label("00.001.001", release_build), "0.1.1");
    EXPECT_EQ(version_label("00.001.001", test_build), "0.1.1 \xC2\xB7 test 20261012-203105");
    EXPECT_EQ(version_label("00.001.001", none), "0.1.1");
    EXPECT_EQ(version_label("", test_build), ""); // no version, nothing to label
    // The tag is the same for the test build and the release made from it (nothing was compiled again).
    EXPECT_EQ(build_tag("00.001.001", test_build), "00.001.001+20261012-203105");
    EXPECT_EQ(build_tag("00.001.001", test_build), build_tag("00.001.001", release_build));
    EXPECT_EQ(build_tag("00.001.001", none), "00.001.001");
}

} // namespace
} // namespace hui
