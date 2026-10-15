// Wings TV app - Bad-link flags: link ids, Best skips flagged links, expiry, cap,
// the saved file and the error-to-reason table.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_page.hpp"
#include "orbit/link_flags.hpp"
#include "orbit/ranking.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <string>

namespace orbit
{
namespace
{

constexpr std::int64_t kNow = 1791936000; // 2026-10-14 00:00 UTC

std::string tmp_dir()
{
    char path[] = "/tmp/wings-flags-XXXXXX";
    return mkdtemp(path) != nullptr ? std::string(path) : std::string("/tmp");
}

TEST(LinkFlags, UrlNormalisationKeepsTheIdStable)
{
    EXPECT_EQ(normalise_link_url("HTTPS://WWW.Example.COM:443/File/AbC/?utm_source=x&id=7&token=zz#frag"),
              "https://example.com/File/AbC?id=7");
    EXPECT_EQ(normalise_link_url("https://mega.nz/file/ABC#key"), "https://mega.nz/file/ABC");
    EXPECT_EQ(normalise_link_url("https://h.to/x?X-Amz-Signature=1&expires=2"), "https://h.to/x");
    EXPECT_EQ(normalise_link_url(""), "");
    const std::string id = link_id("https://1fichier.com/?abc123", "ppsa01234");
    EXPECT_EQ(id.size(), 16u);
    EXPECT_EQ(id, link_id("  HTTPS://1Fichier.com/?abc123&fbclid=q#x", "PPSA01234"));
    EXPECT_NE(id, link_id("https://1fichier.com/?abc124", "PPSA01234"));
    EXPECT_NE(id, link_id("https://1fichier.com/?abc123", "PPSA01235"));
    EXPECT_EQ(link_id("", "PPSA01234"), "");
    // The raw URL never shows in the id.
    EXPECT_EQ(id.find("fichier"), std::string::npos);
}

TEST(LinkFlags, AddClearLookupAndExpiry)
{
    LinkFlags flags;
    flags.add("00000000000000aa", FlagReason::removed, kNow);
    flags.add("00000000000000bb", FlagReason::none, kNow); // ignored
    EXPECT_EQ(flags.size(), 1u);
    ASSERT_NE(flags.find("00000000000000aa", kNow), nullptr);
    EXPECT_EQ(flags.find("00000000000000aa", kNow)->reason, FlagReason::removed);
    // Still there on day 59, gone on day 60: the link comes back.
    EXPECT_TRUE(flags.flagged("00000000000000aa", kNow + (kFlagDays - 1) * 86400));
    EXPECT_FALSE(flags.flagged("00000000000000aa", kNow + kFlagDays * 86400));
    EXPECT_EQ(flags.expire(kNow + kFlagDays * 86400), 1u);
    EXPECT_EQ(flags.size(), 0u);
    flags.add("00000000000000aa", FlagReason::password, kNow);
    flags.add("00000000000000aa", FlagReason::by_hand, kNow + 5); // newest reason wins
    EXPECT_EQ(flags.find("00000000000000aa", kNow)->reason, FlagReason::by_hand);
    EXPECT_TRUE(flags.clear("00000000000000aa"));
    EXPECT_FALSE(flags.clear("00000000000000aa"));
    flags.add("00000000000000aa", FlagReason::removed, kNow);
    flags.add("00000000000000bb", FlagReason::removed, kNow + 1);
    EXPECT_EQ(flags.list(kNow + 2).front().first, "00000000000000bb"); // newest first
    flags.clear_all();
    EXPECT_EQ(flags.size(), 0u);
}

TEST(LinkFlags, CapDropsTheOldest)
{
    LinkFlags flags;
    for (std::size_t i = 0; i < kMaxFlags + 3; ++i)
        flags.add(link_id("https://h.to/" + std::to_string(i), "PPSA00001"), FlagReason::host_error,
                  kNow + static_cast<std::int64_t>(i));
    EXPECT_EQ(flags.size(), kMaxFlags);
    EXPECT_FALSE(flags.flagged(link_id("https://h.to/0", "PPSA00001"), kNow + 1000));
    EXPECT_FALSE(flags.flagged(link_id("https://h.to/2", "PPSA00001"), kNow + 1000));
    EXPECT_TRUE(flags.flagged(link_id("https://h.to/3", "PPSA00001"), kNow + 1000));
}

TEST(LinkFlags, SaveAndLoadRoundTrip)
{
    const std::string path = tmp_dir() + "/link_flags.v1";
    LinkFlags flags;
    flags.add(link_id("https://a.to/1", "PPSA00001"), FlagReason::removed, kNow);
    flags.add(link_id("https://a.to/2", "PPSA00001"), FlagReason::by_hand, kNow - 10);
    EXPECT_EQ(flags.save({path}), 1);
    LinkFlags again;
    ASSERT_TRUE(again.load({tmp_dir() + "/missing", path}));
    EXPECT_EQ(again.encode(), flags.encode());
    EXPECT_EQ(again.find(link_id("https://a.to/2", "PPSA00001"), kNow)->reason, FlagReason::by_hand);
    // Damaged lines are skipped, not fatal.
    LinkFlags damaged;
    EXPECT_TRUE(damaged.decode("zz 1 5\n00000000000000aa 99 5\n00000000000000aa 2 x\n00000000000000ab 2 7\n"));
    EXPECT_EQ(damaged.size(), 1u);
    LinkFlags none;
    EXPECT_FALSE(none.load({tmp_dir() + "/missing"}));
}

TEST(LinkFlags, ReasonFromError)
{
    EXPECT_EQ(reason_from_error("", 404), FlagReason::removed);
    EXPECT_EQ(reason_from_error("", 410), FlagReason::removed);
    EXPECT_EQ(reason_from_error("", 503), FlagReason::host_error);
    EXPECT_EQ(reason_from_error("", 429), FlagReason::none);
    EXPECT_EQ(reason_from_error("File not found or deleted"), FlagReason::removed);
    EXPECT_EQ(reason_from_error("Provider resolve failed: premium only"), FlagReason::host_error);
    EXPECT_EQ(reason_from_error("Wrong password"), FlagReason::password);
    EXPECT_EQ(reason_from_error("CRC error in part 3"), FlagReason::broken_archive);
    EXPECT_EQ(reason_from_error("Unexpected end of archive"), FlagReason::broken_archive);
    EXPECT_EQ(reason_from_error("No PKG found in archive"), FlagReason::wrong_file);
    EXPECT_EQ(reason_from_error("Cancelled by user"), FlagReason::none);
    EXPECT_EQ(reason_from_error("No space left on device"), FlagReason::none);
    EXPECT_EQ(reason_from_error("Connection timed out"), FlagReason::none);
    EXPECT_EQ(reason_from_error("something odd"), FlagReason::none);
}

TEST(LinkFlags, CardLine)
{
    EXPECT_EQ(flag_date(kNow), "14 Oct");
    EXPECT_EQ(flag_date(0), "1 Jan");
    EXPECT_EQ(flag_line(LinkFlag{FlagReason::removed, kNow + 3600, ""}), "Failed for you on 14 Oct: file removed");
    EXPECT_EQ(flag_line(LinkFlag{FlagReason::by_hand, kNow, ""}), "Marked bad on 14 Oct");
    EXPECT_EQ(flag_line(LinkFlag{}), "");
}

TEST(LinkFlags, BestSkipsFlaggedWhileAnotherExists)
{
    std::vector<Candidate> c(3);
    c[0].tier = LinkTier::native_pkg;
    c[0].index = 0;
    c[0].flagged = true;
    c[1].tier = LinkTier::debrid;
    c[1].index = 1;
    c[2].tier = LinkTier::supported;
    c[2].index = 2;
    LinkPrefs prefs;
    Ranking r = rank_links(c, prefs);
    EXPECT_EQ(r.best, 1);
    EXPECT_EQ(r.order, (std::vector<int>{1, 2, 0})); // flagged last
    EXPECT_EQ(r.flagged, 1);
    // Every link flagged: Best still offers one, and says so.
    for (Candidate &x : c)
        x.flagged = true;
    r = rank_links(c, prefs);
    EXPECT_EQ(r.best, 0);
    EXPECT_NE(r.why.find("failed for you before"), std::string::npos);
}

TEST(LinkFlags, AppliedToAGamePage)
{
    PackageDetail d;
    d.package.id = "10";
    d.package.title = "Game X";
    d.package.source_name = "S";
    d.package.link_count = 2;
    PackageLink a;
    a.index = 0;
    a.name = "USA 12.xx Base - 1fichier";
    a.host = "1fichier.com";
    a.route = LinkRoute::debrid; // the better tier: Best without the flag
    a.url = "https://1fichier.com/?aaa";
    PackageLink b;
    b.index = 1;
    b.name = "USA 12.xx Base - Rootz";
    b.host = "rootz.so";
    b.provider = "Rootz";
    b.url = "https://rootz.so/d/bbb";
    d.links = {a, b};
    GamePage page = build_game_page({d});
    ASSERT_EQ(page.links.size(), 2u);
    LinkFlags flags;
    flags.add(page_link_id(page, 0, "PPSA00001"), FlagReason::removed, kNow);
    apply_link_flags(&page, flags, "PPSA00001", kNow);
    EXPECT_EQ(page.links[0].flag_reason, static_cast<std::uint8_t>(FlagReason::removed));
    EXPECT_EQ(page.links[1].flag_reason, 0);
    rank_page(&page, LinkPrefs{});
    ASSERT_GE(page.best, 0);
    EXPECT_EQ(page.mirrors[static_cast<std::size_t>(page.best)].host, "Rootz");
    EXPECT_EQ(page.mirrors[static_cast<std::size_t>(page.best)].flagged_link(page), -1);
    // Cleared: the flag is gone on the next apply.
    flags.clear(page_link_id(page, 0, "PPSA00001"));
    apply_link_flags(&page, flags, "PPSA00001", kNow);
    EXPECT_EQ(page.links[0].flag_reason, 0);
}

} // namespace
} // namespace orbit
