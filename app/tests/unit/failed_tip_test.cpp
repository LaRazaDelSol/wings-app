// Wings TV app - The Downloads tip under a failed job (install_hint.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/install_hint.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

TEST(FailedDownloadTip, ShownForFailedWithManyOrUnknownConnections)
{
    const std::string tip = failed_download_tip("failed", 16);
    EXPECT_EQ(tip,
              "Tip: set Download connections to 4 or lower for the most compatible downloads "
              "(Settings > Pegasus DL).");
    EXPECT_EQ(failed_download_tip("failed", 5), tip);
    EXPECT_EQ(failed_download_tip("failed", 0), tip); // unknown
}

TEST(FailedDownloadTip, HiddenWhenAlreadyFourOrLowerAndForOtherStatuses)
{
    EXPECT_TRUE(failed_download_tip("failed", 4).empty());
    EXPECT_TRUE(failed_download_tip("failed", 1).empty());
    EXPECT_TRUE(failed_download_tip("completed", 16).empty());
    EXPECT_TRUE(failed_download_tip("cancelled", 0).empty());
    EXPECT_TRUE(failed_download_tip("downloading", 16).empty());
}

} // namespace
} // namespace orbit
