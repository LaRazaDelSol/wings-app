// Wings TV app - "Is there a newer Wings?": release JSON, tag rules, advice, saved answer, the check service
// (due time, retry window, the switch, the network gate) and the words Settings shows. No test uses the network.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/i18n.hpp"
#include "orbit/prefs.hpp"
#include "orbit/wings_update.hpp"

#include <gtest/gtest.h>

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace orbit
{
namespace
{

// The shape "releases/latest" answers with, trimmed to what we read plus a few fields we must ignore.
const char *const kLatest = R"({"url":"https://api.github.com/x","tag_name":"v0.1.2","name":"Wings 0.1.2",
 "draft":false,"prerelease":false,"published_at":"2026-10-20T10:00:00Z",
 "html_url":"https://example.invalid/not-used","assets":[{"name":"PPSA99178.ffpkg","size":54394880}],
 "body":"Notes with \"quotes\" and unicode é."})";

std::string tmp_dir()
{
    char path[] = "/tmp/wings-upd-XXXXXX";
    return mkdtemp(path) != nullptr ? std::string(path) : std::string("/tmp");
}

struct FakeFetcher final : DocFetcher
{
    int calls = 0;
    std::string last_url;
    DocResult result;
    DocResult get(const std::string &url, std::size_t) override
    {
        ++calls;
        last_url = url;
        return result;
    }
};

struct Rig
{
    explicit Rig(const std::string &running = "00.001.001")
    {
        fetcher.result.status = 200;
        fetcher.result.body = kLatest;
        dir = tmp_dir();
        config.docs = &fetcher;
        config.ready = [this](std::string *why)
        {
            if (!network)
                *why = "The network is off.";
            return network;
        };
        config.cache_paths = {dir + "/wings_update.v1"};
        config.running = running;
        config.now = [this] { return now; };
    }
    void attach(WingsUpdate &u)
    {
        u.attach(config);
    }
    FakeFetcher fetcher;
    WingsUpdateConfig config;
    std::string dir;
    std::int64_t now = 2000000000;
    bool network = true;
};

TEST(WingsUpdatePrefs, TheSwitchIsOnByDefaultAndIsKept)
{
    WingsLocalPrefs prefs;
    EXPECT_TRUE(prefs.update_check);
    prefs.update_check = false;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_FALSE(back.update_check);
    EXPECT_TRUE(back == prefs);
    prefs.update_check = true;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_TRUE(back.update_check);
    // A file written before the switch existed has no key: the default (on) applies.
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("region=USA\n", &old));
    EXPECT_TRUE(old.update_check);
}

TEST(WingsUpdateRelease, ReadsTheLatestReleaseTag)
{
    std::string tag;
    std::string error;
    ASSERT_TRUE(parse_wings_latest(kLatest, &tag, &error)) << error;
    EXPECT_EQ(tag, "v0.1.2");
}

TEST(WingsUpdateRelease, RefusesAnythingThatIsNotAPublishedVersionedRelease)
{
    std::string tag;
    std::string error;
    EXPECT_FALSE(parse_wings_latest("", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("not json", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("[]", &tag, &error));                                    // the list endpoint
    EXPECT_FALSE(parse_wings_latest("{\"message\":\"Not Found\"}", &tag, &error));           // no tag
    EXPECT_FALSE(parse_wings_latest("{\"tag_name\":\"v0.1.2\",\"draft\":true}", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("{\"tag_name\":\"v0.2.0\",\"prerelease\":true}", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("{\"tag_name\":\"latest\"}", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("{\"tag_name\":\"v0.1\"}", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("{\"tag_name\":\"v0.1.2-beta\"}", &tag, &error));
    EXPECT_FALSE(parse_wings_latest("{\"tag_name\":5}", &tag, &error));
    EXPECT_TRUE(tag.empty());
    EXPECT_FALSE(error.empty());
}

TEST(WingsUpdateAdvice, ComparesTheAppWithTheLatestTag)
{
    EXPECT_EQ(wings_advice("00.001.001", ""), WingsAdvice::not_checked);
    EXPECT_EQ(wings_advice("00.001.001", "v0.1.2"), WingsAdvice::newer);
    EXPECT_EQ(wings_advice("00.001.001", "v0.1.1"), WingsAdvice::up_to_date);
    EXPECT_EQ(wings_advice("00.001.002", "v0.1.1"), WingsAdvice::ahead);
    EXPECT_EQ(wings_advice("00.009.000", "v0.10.0"), WingsAdvice::newer); // numbers, not text
    EXPECT_EQ(wings_advice("01.000.000", "v0.99.999"), WingsAdvice::ahead);
    EXPECT_EQ(wings_advice("", "v0.1.2"), WingsAdvice::unreadable);
    EXPECT_EQ(wings_advice("garbage", "v0.1.2"), WingsAdvice::unreadable);
    EXPECT_EQ(wings_advice("00.001.001", "0.1.2"), WingsAdvice::unreadable);
}

TEST(WingsUpdateCache, DueTimeIsSixHoursAndSurvivesAClockThatWentBackwards)
{
    EXPECT_TRUE(wings_check_due(0, 1000, false));                          // never checked
    EXPECT_FALSE(wings_check_due(1000, 1000 + kWingsCheckTtlS - 1, false));
    EXPECT_TRUE(wings_check_due(1000, 1000 + kWingsCheckTtlS, false));
    EXPECT_TRUE(wings_check_due(5000, 1000, false));                       // the clock went backwards
    EXPECT_TRUE(wings_check_due(1000, 1001, true));                        // the player pressed Check
}

TEST(WingsUpdateCache, SavedAnswerRoundTripsAndRejectsDamage)
{
    std::int64_t at = 0;
    std::string tag;
    ASSERT_TRUE(decode_wings_cache(encode_wings_cache(1234567890, "v0.1.2"), &at, &tag));
    EXPECT_EQ(at, 1234567890);
    EXPECT_EQ(tag, "v0.1.2");
    EXPECT_FALSE(decode_wings_cache("", &at, &tag));
    EXPECT_FALSE(decode_wings_cache("tag=v0.1.2\n", &at, &tag));          // no time
    EXPECT_FALSE(decode_wings_cache("checked=12x\ntag=v0.1.2\n", &at, &tag));
    EXPECT_FALSE(decode_wings_cache("checked=1234567890123456\n", &at, &tag));
    // A tag that is not a version is dropped, the time is kept.
    ASSERT_TRUE(decode_wings_cache("checked=99\ntag=../../etc\n", &at, &tag));
    EXPECT_EQ(at, 99);
    EXPECT_TRUE(tag.empty());
}

TEST(WingsUpdateService, ChecksOnceThenWaitsForTheTimeToPass)
{
    Rig rig;
    WingsUpdate update;
    rig.attach(update);
    EXPECT_TRUE(update.status().attached);
    EXPECT_EQ(update.status().advice, WingsAdvice::not_checked);

    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 1);
    EXPECT_EQ(rig.fetcher.last_url, kWingsLatestReleaseUrl);
    WingsUpdateStatus s = update.status();
    EXPECT_FALSE(s.busy);
    EXPECT_TRUE(s.error.empty());
    EXPECT_TRUE(s.have_latest);
    EXPECT_EQ(s.latest_tag, "v0.1.2");
    EXPECT_EQ(s.latest_text, "0.1.2");
    EXPECT_EQ(s.running_text, "0.1.1");
    EXPECT_EQ(s.advice, WingsAdvice::newer);
    EXPECT_EQ(s.checked_at, rig.now);

    // Within six hours nothing is asked again; after them it is.
    rig.now += kWingsCheckTtlS - 1;
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 1);
    rig.now += 1;
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 2);
}

TEST(WingsUpdateService, AnAppStartUsesTheSavedAnswerWithoutAsking)
{
    Rig rig;
    {
        WingsUpdate first;
        rig.attach(first);
        first.auto_check();
        first.wait_idle();
        ASSERT_EQ(rig.fetcher.calls, 1);
    }
    // A later start, an hour on: the answer is known at once (the toast can use it) and nothing is fetched.
    rig.now += 3600;
    WingsUpdate second;
    rig.attach(second);
    const WingsUpdateStatus s = second.status();
    EXPECT_EQ(s.latest_tag, "v0.1.2");
    EXPECT_EQ(s.advice, WingsAdvice::newer);
    second.auto_check();
    second.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 1);
    // A new version of the app compares the same answer with its own number.
    rig.config.running = "00.001.002";
    WingsUpdate third;
    rig.attach(third);
    EXPECT_EQ(third.status().advice, WingsAdvice::up_to_date);
    rig.config.running = "00.002.000";
    WingsUpdate fourth;
    rig.attach(fourth);
    EXPECT_EQ(fourth.status().advice, WingsAdvice::ahead);
}

TEST(WingsUpdateService, TheSwitchStopsTheStartUpCheckButNotACheckTheLayerAskedFor)
{
    Rig rig;
    WingsUpdate update;
    rig.attach(update);
    update.set_enabled(false);
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 0);
    EXPECT_FALSE(update.status().enabled);
    EXPECT_TRUE(update.check_now());
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 1);
    update.set_enabled(true);
    EXPECT_TRUE(update.status().enabled);
}

TEST(WingsUpdateService, NothingIsAskedWhileTheNetworkIsNotReady)
{
    Rig rig;
    rig.network = false;
    WingsUpdate update;
    rig.attach(update);
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 0);
    EXPECT_TRUE(update.status().error.empty()); // the start-up check is silent about it
    EXPECT_FALSE(update.check_now());
    EXPECT_EQ(update.status().error, "The network is off."); // a check the player asked for says why
    // The network comes up: the very next look goes ahead (no retry delay was spent on the closed gate).
    rig.network = true;
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 1);
    EXPECT_TRUE(update.status().error.empty());
}

TEST(WingsUpdateService, AFailureKeepsTheOldAnswerAndIsNotRetriedEveryFrame)
{
    Rig rig;
    WingsUpdate update;
    rig.attach(update);
    update.auto_check();
    update.wait_idle();
    ASSERT_EQ(rig.fetcher.calls, 1);

    rig.now += kWingsCheckTtlS;
    rig.fetcher.result = DocResult{};
    rig.fetcher.result.status = 403;
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 2);
    WingsUpdateStatus s = update.status();
    EXPECT_NE(s.error.find("limiting"), std::string::npos);
    EXPECT_EQ(s.latest_tag, "v0.1.2"); // the earlier answer stays
    EXPECT_EQ(s.advice, WingsAdvice::newer);

    update.auto_check(); // within the retry window
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 2);
    rig.now += kWingsRetryS;
    rig.fetcher.result.status = 200;
    rig.fetcher.result.body = "{\"tag_name\":\"v0.1.3\"}";
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(rig.fetcher.calls, 3);
    EXPECT_EQ(update.status().latest_tag, "v0.1.3");
    EXPECT_TRUE(update.status().error.empty());
}

TEST(WingsUpdateService, EveryKindOfBadAnswerIsAnErrorAndNeverANewerRelease)
{
    const struct
    {
        int status;
        const char *body;
        const char *error;
    } cases[] = {
        {404, "{\"message\":\"Not Found\"}", "No release"},
        {500, "", "HTTP 500"},
        {200, "<html>not json</html>", "could not be read"},
        {200, "{\"tag_name\":\"v9.9.9\",\"draft\":true}", "could not be read"},
        {200, "{\"tag_name\":\"junk\"}", "could not be read"},
    };
    for (const auto &c : cases)
    {
        Rig rig;
        rig.fetcher.result.status = c.status;
        rig.fetcher.result.body = c.body;
        WingsUpdate update;
        rig.attach(update);
        update.auto_check();
        update.wait_idle();
        const WingsUpdateStatus s = update.status();
        EXPECT_NE(s.error.find(c.error), std::string::npos) << c.body << " -> " << s.error;
        EXPECT_FALSE(s.have_latest) << c.body;
        EXPECT_EQ(s.advice, WingsAdvice::not_checked) << c.body;
    }
    // A transport failure carries its own sentence.
    Rig rig;
    rig.fetcher.result = DocResult{};
    rig.fetcher.result.error = "connection refused";
    WingsUpdate update;
    rig.attach(update);
    update.auto_check();
    update.wait_idle();
    EXPECT_EQ(update.status().error, "connection refused");
}

TEST(WingsUpdateService, NotAttachedDoesNothing)
{
    WingsUpdate update;
    EXPECT_FALSE(update.status().attached);
    update.auto_check();
    EXPECT_FALSE(update.check_now());
    update.wait_idle();
    const WingsUpdateText words = wings_update_text(update.status());
    EXPECT_EQ(words.value, "-");
}

TEST(WingsUpdateWords, SayWhatIsKnown)
{
    WingsUpdateStatus s;
    s.attached = true;
    s.running_text = "0.1.1";
    EXPECT_EQ(wings_update_text(s).value, "Check"); // never checked
    s.busy = true;
    EXPECT_EQ(wings_update_text(s).value, "Checking...");
    s.busy = false;
    s.have_latest = true;
    s.latest_tag = "v0.1.2";
    s.latest_text = "0.1.2";
    s.advice = WingsAdvice::newer;
    EXPECT_EQ(wings_update_text(s).value, "0.1.2 available");
    EXPECT_NE(wings_update_text(s).detail.find("0.1.1"), std::string::npos);
    s.advice = WingsAdvice::up_to_date;
    EXPECT_EQ(wings_update_text(s).value, "Up to date");
    s.advice = WingsAdvice::ahead;
    EXPECT_EQ(wings_update_text(s).value, "Newer than the release");
    s.error = "connection refused";
    EXPECT_EQ(wings_update_text(s).value, "Check");
    EXPECT_NE(wings_update_text(s).detail.find("connection refused"), std::string::npos);
    // A newer release is still the news when a later look failed.
    s.advice = WingsAdvice::newer;
    EXPECT_EQ(wings_update_text(s).value, "0.1.2 available");
}

TEST(WingsUpdateWords, TheGuideHasFourStepsThatFitTheDialog)
{
    WingsUpdateStatus s;
    s.attached = true;
    s.running_text = "0.1.1";
    s.latest_text = "0.1.2";
    const WingsUpdateGuide g = wings_update_guide(s);
    EXPECT_EQ(g.title, "Update to Wings 0.1.2");
    EXPECT_NE(g.subtitle.find("0.1.1"), std::string::npos);
    ASSERT_EQ(g.steps.size(), 4u);
    for (const WingsUpdateStep &step : g.steps)
    {
        EXPECT_FALSE(step.label.empty());
        EXPECT_LE(step.label.size(), 60u); // one line in the list dialog
        EXPECT_LE(step.detail.size(), 90u);
    }
    EXPECT_NE(g.steps[0].detail.find(kWingsReleasePage), std::string::npos);
    EXPECT_NE(wings_update_toast(s).find("0.1.2"), std::string::npos);
}

} // namespace
} // namespace orbit
