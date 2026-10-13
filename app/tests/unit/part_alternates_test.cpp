// Wings TV app - "Download all parts": the same part on the release's other hosts.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_view.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orbit
{
namespace
{

struct HostSpec
{
    const char *label; // "USA 12.xx Base - 1fichier"
    const char *host;  // "1fichier.com"
    LinkRoute route;
    const char *provider;
    std::vector<int> parts; // part numbers in catalogue order; empty: one file
};

// One package, each host's links one after the other, link indexes 0..N-1.
GameView page_of(const std::vector<HostSpec> &hosts)
{
    PackageDetail d;
    d.package.id = "p1";
    d.package.title = "Game X";
    d.package.source_name = "DLPS Catalog";
    int index = 0;
    const auto add = [&](const HostSpec &h, const std::string &name)
    {
        PackageLink l;
        l.index = index;
        l.name = name;
        l.host = h.host;
        l.route = h.route;
        l.provider = h.provider;
        l.url = "https://" + std::string(h.host) + "/f/" + std::to_string(index);
        l.needs_browser = h.route == LinkRoute::capture || h.route == LinkRoute::browser_only;
        d.links.push_back(l);
        ++index;
    };
    for (const HostSpec &h : hosts)
    {
        if (h.parts.empty())
            add(h, h.label);
        for (const int p : h.parts)
            add(h, std::string(h.label) + " part " + std::to_string(p));
    }
    d.package.link_count = index;
    GameView v;
    load_game_view(&v, {d}, {}, LinkPrefs{});
    return v;
}

int mirror_of(const GameView &v, int link_index)
{
    for (std::size_t m = 0; m < v.page.mirrors.size(); ++m)
        for (const int l : v.page.mirrors[m].links)
            if (v.page.links[static_cast<std::size_t>(l)].index == link_index)
                return static_cast<int>(m);
    return -1;
}

std::string host_of(const GameView &v, int link_index)
{
    const int m = mirror_of(v, link_index);
    return m < 0 ? std::string() : v.page.mirrors[static_cast<std::size_t>(m)].host;
}

const std::vector<int> kSix = {1, 2, 3, 4, 5, 6};

// 1fichier (debrid) 0-5, Akirabox (automated) 6-11, DataNodes (automated) 12-17, Rootz (browser) 18-23.
GameView four_hosts()
{
    return page_of({{"USA 12.xx Base - 1fichier", "1fichier.com", LinkRoute::debrid, "", kSix},
                    {"USA 12.xx Base - Akirabox", "akirabox.com", LinkRoute::automated, "Akirabox", kSix},
                    {"USA 12.xx Base - DataNodes", "datanodes.to", LinkRoute::automated, "DataNodes", kSix},
                    {"USA 12.xx Base - Rootz", "rootz.so", LinkRoute::capture, "Rootz", kSix}});
}

bool no_alternates(const PartsPlan &plan)
{
    for (const std::vector<LinkRef> &a : plan.alternates)
        if (!a.empty())
            return false;
    return plan.alternates.size() == plan.refs.size() && plan.alternate_hosts.size() == plan.refs.size() &&
           plan.numbers.size() == plan.refs.size();
}

TEST(PartAlternates, AlignedHostsGiveEachPartItsCopiesBestTierFirst)
{
    const GameView v = four_hosts();
    const std::string fichier = host_of(v, 0);
    const std::string akira = host_of(v, 6);
    const std::string nodes = host_of(v, 12);
    ASSERT_EQ(mirror_of(v, 0), mirror_of(v, 5)); // one card per host
    ASSERT_NE(fichier, akira);
    ASSERT_NE(akira, nodes);
    ASSERT_LT(akira, nodes); // same tier: host A to Z

    const StartedLinks none;
    PartsPlan plan = plan_parts(v, mirror_of(v, 0), none);
    ASSERT_EQ(plan.refs.size(), 6u);
    ASSERT_EQ(plan.numbers, kSix);
    ASSERT_EQ(plan.alternates.size(), 6u);
    ASSERT_EQ(plan.alternate_hosts.size(), 6u);
    for (int i = 0; i < 6; ++i)
    {
        const std::size_t at = static_cast<std::size_t>(i);
        // Rootz opens the PS5 browser, 1fichier does not: never offered as its stand-in.
        EXPECT_EQ(plan.alternates[at], (std::vector<LinkRef>{{"p1", 6 + i}, {"p1", 12 + i}}));
        EXPECT_EQ(plan.alternate_hosts[at], (std::vector<std::string>{akira, nodes}));
    }
    EXPECT_NE(plan.message.find("If a part keeps failing, the same part is tried on " + akira + ", " + nodes + "."),
              std::string::npos)
        << plan.message;
    EXPECT_EQ(plan.title, "Download 6 parts from " + fichier + "?");

    // From Akirabox: debrid (1fichier) before the other automated host.
    plan = plan_parts(v, mirror_of(v, 6), none);
    ASSERT_EQ(plan.alternates.size(), 6u);
    EXPECT_EQ(plan.alternates[3], (std::vector<LinkRef>{{"p1", 3}, {"p1", 15}}));
    EXPECT_EQ(plan.alternate_hosts[3], (std::vector<std::string>{fichier, nodes}));

    // From Rootz (one part at a time): every other host may stand in.
    plan = plan_parts(v, mirror_of(v, 18), none);
    ASSERT_TRUE(plan.one_at_a_time);
    ASSERT_EQ(plan.refs.size(), 1u);
    ASSERT_EQ(plan.alternates.size(), 1u);
    EXPECT_EQ(plan.numbers, (std::vector<int>{1}));
    EXPECT_EQ(plan.alternates[0], (std::vector<LinkRef>{{"p1", 0}, {"p1", 6}, {"p1", 12}}));
}

TEST(PartAlternates, StartedLinksAreNeitherQueuedNorOfferedAgain)
{
    const GameView v = four_hosts();
    StartedLinks started;
    started.add(LinkRef{"p1", 0});  // 1fichier part 1: left out of the plan
    started.add(LinkRef{"p1", 8});  // Akirabox part 3: not a stand-in any more
    const PartsPlan plan = plan_parts(v, mirror_of(v, 0), started);
    ASSERT_EQ(plan.refs.size(), 5u);
    EXPECT_EQ(plan.skipped, 1);
    EXPECT_EQ(plan.numbers, (std::vector<int>{2, 3, 4, 5, 6}));
    EXPECT_EQ(plan.alternates[0], (std::vector<LinkRef>{{"p1", 7}, {"p1", 13}}));
    EXPECT_EQ(plan.alternates[1], (std::vector<LinkRef>{{"p1", 14}}));
    EXPECT_EQ(plan.alternate_hosts[1], (std::vector<std::string>{host_of(v, 14)}));
}

TEST(PartAlternates, SetsThatDoNotLineUpExactlyAreNeverUsed)
{
    // 1fichier 0-5; Akirabox 5 of 6 parts (6-10); DataNodes restarts (11-16);
    // Gofile has a gap (17-22); Mega is not supported (23-28).
    const GameView v =
        page_of({{"USA 12.xx Base - 1fichier", "1fichier.com", LinkRoute::debrid, "", kSix},
                 {"USA 12.xx Base - Akirabox", "akirabox.com", LinkRoute::automated, "Akirabox", {1, 2, 3, 4, 5}},
                 {"USA 12.xx Base - DataNodes", "datanodes.to", LinkRoute::automated, "DataNodes", {1, 2, 3, 1, 2, 3}},
                 {"USA 12.xx Base - Gofile", "gofile.io", LinkRoute::automated, "Gofile", {1, 2, 3, 5, 6, 7}},
                 {"USA 12.xx Base - Mega", "mega.nz", LinkRoute::blocked, "", kSix}});
    const StartedLinks none;
    PartsPlan plan = plan_parts(v, mirror_of(v, 0), none);
    ASSERT_EQ(plan.refs.size(), 6u);
    EXPECT_EQ(plan.numbers, kSix);
    EXPECT_TRUE(no_alternates(plan));
    EXPECT_EQ(plan.message.find("keeps failing"), std::string::npos);
    // A set whose numbering restarts gets none either, though 1fichier has six parts.
    const int nodes = mirror_of(v, 11);
    ASSERT_GE(nodes, 0);
    ASSERT_EQ(v.page.mirrors[static_cast<std::size_t>(nodes)].status, PartStatus::restarts);
    plan = plan_parts(v, nodes, none);
    EXPECT_TRUE(no_alternates(plan));
}

TEST(PartAlternates, TwoReleasesNeverMix)
{
    const GameView v = page_of({{"USA 12.xx Base - 1fichier", "1fichier.com", LinkRoute::debrid, "", kSix},
                                {"EUR 12.xx Base - Akirabox", "akirabox.com", LinkRoute::automated, "Akirabox", kSix}});
    ASSERT_NE(v.page.mirrors[static_cast<std::size_t>(mirror_of(v, 0))].release,
              v.page.mirrors[static_cast<std::size_t>(mirror_of(v, 6))].release);
    const StartedLinks none;
    EXPECT_TRUE(no_alternates(plan_parts(v, mirror_of(v, 0), none)));
    EXPECT_TRUE(no_alternates(plan_parts(v, mirror_of(v, 6), none)));
}

TEST(PartAlternates, OneFileMirrorsNeitherGetNorGiveAlternates)
{
    // 1fichier one file (0), Akirabox parts 1-6 (1-6), DataNodes parts 1-6 (7-12).
    const GameView v =
        page_of({{"USA 12.xx Base - 1fichier", "1fichier.com", LinkRoute::debrid, "", {}},
                 {"USA 12.xx Base - Akirabox", "akirabox.com", LinkRoute::automated, "Akirabox", kSix},
                 {"USA 12.xx Base - DataNodes", "datanodes.to", LinkRoute::automated, "DataNodes", kSix}});
    const StartedLinks none;
    const int single = mirror_of(v, 0);
    ASSERT_FALSE(v.page.mirrors[static_cast<std::size_t>(single)].multipart());
    PartsPlan plan = plan_parts(v, single, none);
    ASSERT_EQ(plan.refs.size(), 1u);
    EXPECT_EQ(plan.numbers, (std::vector<int>{1}));
    EXPECT_TRUE(no_alternates(plan));
    plan = plan_parts(v, mirror_of(v, 1), none);
    ASSERT_EQ(plan.alternates.size(), 6u);
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(plan.alternates[static_cast<std::size_t>(i)], (std::vector<LinkRef>{{"p1", 7 + i}}));
}

TEST(PartAlternates, PartNumberOfALinkInsideItsMirror)
{
    const GameView v = page_of({{"USA 12.xx Base - Akirabox", "akirabox.com", LinkRoute::automated, "Akirabox",
                                 {3, 1, 2}}});
    const int m = mirror_of(v, 0);
    EXPECT_EQ(mirror_part_number(v.page, m, LinkRef{"p1", 0}), 3);
    EXPECT_EQ(mirror_part_number(v.page, m, LinkRef{"p1", 1}), 1);
    EXPECT_EQ(mirror_part_number(v.page, m, LinkRef{"p1", 9}), 0);
    EXPECT_EQ(mirror_part_number(v.page, -1, LinkRef{"p1", 0}), 0);
}

} // namespace
} // namespace orbit
