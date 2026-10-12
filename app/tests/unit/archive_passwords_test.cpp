// Wings TV app - Known archive passwords (attempt state machine against a scripted
// Pegasus) and the shared editing keys of every text prompt.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/archive_passwords.hpp"
#include "orbit/panels.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

using Seen = PasswordTrials::Seen;
using Kind = PasswordTrials::Step::Kind;

// A Pegasus that holds a job at waiting_password and reacts to each password.
struct ScriptedPegasus
{
    std::string secret;            // the password that works ("" never works)
    bool bounce = true;            // a wrong password: extracting for a tick, then waiting again
    bool change_detail = false;    // a wrong password changes the detail text
    bool silent = false;           // a wrong password changes nothing at all
    Seen state = Seen::waiting;
    std::string detail = "Archive password required";
    int wrong = 0;
    int bounce_ticks = 0;
    int finish_ticks = 0;
    std::vector<std::string> received;

    void send(const std::string &password)
    {
        received.push_back(password);
        if (password == secret)
        {
            state = Seen::active;
            finish_ticks = 2;
        }
        else if (silent)
        {
        }
        else if (change_detail && !bounce)
        {
            detail = "Wrong password (" + std::to_string(++wrong) + ")";
        }
        else
        {
            state = Seen::active;
            bounce_ticks = 2;
        }
    }
    void tick()
    {
        if (finish_ticks > 0 && --finish_ticks == 0)
            state = Seen::finished;
        if (bounce_ticks > 0 && --bounce_ticks == 0)
            state = Seen::waiting;
    }
};

struct Scenario
{
    PasswordTrials trials;
    ScriptedPegasus pegasus;
    std::string key = "job1";
    std::int64_t now = 0;
    bool prompted = false;
    bool done = false;
    std::string note;
    int fail_on_send = 0; // 1-based send number that errors out (0: never)
    int cancel_after_sends = 0;

    // Returns when a prompt opened, the job ended or the tick budget ran out.
    int run(int max_ticks = 2000)
    {
        for (int tick = 0; tick < max_ticks; ++tick)
        {
            now += 500;
            if (cancel_after_sends > 0 && static_cast<int>(pegasus.received.size()) >= cancel_after_sends)
                pegasus.state = Seen::ended;
            const auto step = trials.observe(key, pegasus.state, pegasus.detail, now);
            if (step.kind == Kind::send)
            {
                if (static_cast<int>(pegasus.received.size()) + 1 == fail_on_send)
                {
                    const auto failed = trials.send_failed(key, "Pegasus said no");
                    prompted = true;
                    note = failed.note;
                    return tick;
                }
                pegasus.send(step.password);
                trials.sent(key, now);
            }
            else if (step.kind == Kind::prompt)
            {
                prompted = true;
                note = step.note;
                return tick;
            }
            else if (step.kind == Kind::done)
            {
                done = true;
                return tick;
            }
            pegasus.tick();
        }
        return max_ticks;
    }
};

const std::vector<std::string> kList = {"DLPSGAME.COM",      "[DLPSGAME.COM]", "SuperPSX",
                                        "www.DLPSGAME.COM", "downloadgameps3.com", "hako"};

TEST(ArchivePasswords, TheListIsFixedAndCaseSensitive)
{
    const auto &list = known_archive_passwords();
    ASSERT_EQ(list.size(), kList.size());
    for (std::size_t i = 0; i < kList.size(); ++i)
        EXPECT_EQ(std::string(list[i]), kList[i]);
}

TEST(ArchivePasswords, TheFirstPasswordWorks)
{
    Scenario run;
    run.pegasus.secret = "DLPSGAME.COM";
    run.run();
    EXPECT_TRUE(run.done);
    EXPECT_FALSE(run.prompted);
    EXPECT_EQ(run.pegasus.received, std::vector<std::string>({"DLPSGAME.COM"}));
    EXPECT_EQ(run.trials.trying_number(run.key), 0); // dropped when the job finished
}

TEST(ArchivePasswords, TheLastPasswordWorksAfterFiveFailuresInOrder)
{
    Scenario run;
    run.pegasus.secret = "hako";
    run.run();
    EXPECT_TRUE(run.done);
    EXPECT_FALSE(run.prompted);
    EXPECT_EQ(run.pegasus.received, kList);
}

TEST(ArchivePasswords, NoneWorkOpensThePromptWithTheNote)
{
    Scenario run;
    run.pegasus.secret = "";
    run.run();
    EXPECT_TRUE(run.prompted);
    EXPECT_FALSE(run.done);
    EXPECT_EQ(run.note, "Tried 6 known passwords");
    EXPECT_EQ(run.pegasus.received, kList);
}

TEST(ArchivePasswords, ARowShowsTheNumberInFlight)
{
    PasswordTrials trials;
    EXPECT_EQ(trials.trying_number("a"), 0);
    auto step = trials.observe("a", Seen::waiting, "x", 0);
    ASSERT_EQ(step.kind, Kind::send);
    EXPECT_EQ(step.number, 1);
    EXPECT_EQ(step.total, 6);
    trials.sent("a", 0);
    EXPECT_EQ(trials.trying_number("a"), 1);
    trials.observe("a", Seen::active, "x", 100);
    step = trials.observe("a", Seen::waiting, "x", 200);
    ASSERT_EQ(step.kind, Kind::send);
    EXPECT_EQ(trials.trying_number("a"), 2);
}

TEST(ArchivePasswords, ABackendErrorMidWayStopsTryingAndOpensThePrompt)
{
    Scenario run;
    run.pegasus.secret = "";
    run.fail_on_send = 3;
    run.run();
    EXPECT_TRUE(run.prompted);
    EXPECT_EQ(run.pegasus.received.size(), 2u); // the third never left
    EXPECT_NE(run.note.find("Tried 3 known passwords"), std::string::npos);
    EXPECT_NE(run.note.find("Pegasus said no"), std::string::npos);
    // No further sends however long the job keeps waiting.
    for (int i = 0; i < 100; ++i)
        EXPECT_NE(run.trials.observe(run.key, Seen::waiting, "x", run.now + 500 * (i + 1)).kind,
                  Kind::send);
}

TEST(ArchivePasswords, ACancelledJobMidWayEndsTheTrial)
{
    Scenario run;
    run.pegasus.secret = "";
    run.cancel_after_sends = 2;
    run.run();
    EXPECT_TRUE(run.done);
    EXPECT_FALSE(run.prompted);
    EXPECT_EQ(run.pegasus.received.size(), 2u);
    EXPECT_EQ(run.trials.tried_count(run.key), 0);
    // A job that vanished from the list is reported as ended and dropped too.
    PasswordTrials trials;
    trials.observe("gone", Seen::waiting, "", 0);
    EXPECT_EQ(trials.observe("gone", Seen::ended, "", 10).kind, Kind::done);
    EXPECT_TRUE(trials.keys().empty());
}

TEST(ArchivePasswords, EachPasswordIsTriedAtMostOncePerJobAndASilentServerDoesNotLoopForever)
{
    Scenario run;
    run.pegasus.secret = "";
    run.pegasus.silent = true; // stays waiting_password, nothing changes
    const int ticks = run.run();
    EXPECT_LT(ticks, 2000);
    EXPECT_TRUE(run.prompted);
    EXPECT_EQ(run.pegasus.received, kList); // each once, in order
    std::vector<std::string> sorted = run.pegasus.received;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    // The prompt step is delivered once; later observations stay quiet.
    for (int i = 0; i < 50; ++i)
        EXPECT_EQ(run.trials.observe(run.key, Seen::waiting, run.pegasus.detail, run.now + 1000 * i).kind,
                  Kind::none);
}

TEST(ArchivePasswords, ATextChangeWhileWaitingCountsAsAWrongPasswordWithoutTheSettleWait)
{
    Scenario run;
    run.pegasus.secret = "";
    run.pegasus.bounce = false;
    run.pegasus.change_detail = true;
    const int ticks = run.run();
    EXPECT_TRUE(run.prompted);
    EXPECT_EQ(run.pegasus.received, kList);
    EXPECT_LT(ticks, 20); // no 8 s settle per password
}

TEST(ArchivePasswords, AUserPasswordIsKeptForThatJobOnly)
{
    PasswordTrials trials;
    trials.observe("a", Seen::waiting, "", 0);
    trials.observe("b", Seen::waiting, "", 0);
    trials.set_user_password("a", "mine");
    ASSERT_NE(trials.user_password("a"), nullptr);
    EXPECT_EQ(*trials.user_password("a"), "mine");
    EXPECT_EQ(trials.user_password("b"), nullptr);
    EXPECT_EQ(trials.user_password("c"), nullptr);
    // The player's try replaces the automatic ones: nothing more is sent for "a".
    EXPECT_NE(trials.observe("a", Seen::waiting, "", 100000).kind, Kind::send);
    // When the job is over the password goes with it.
    trials.observe("a", Seen::finished, "", 1);
    EXPECT_EQ(trials.user_password("a"), nullptr);
}

TEST(ArchivePasswords, AWrongUserPasswordReopensThePromptOncePerRoundTrip)
{
    PasswordTrials trials;
    trials.set_user_password("a", "nope");
    EXPECT_EQ(trials.observe("a", Seen::waiting, "", 0).kind, Kind::none);
    EXPECT_EQ(trials.observe("a", Seen::active, "", 10).kind, Kind::none);
    const auto step = trials.observe("a", Seen::waiting, "", 20);
    EXPECT_EQ(step.kind, Kind::prompt);
    EXPECT_EQ(trials.observe("a", Seen::waiting, "", 30).kind, Kind::none);
}

TEST(ArchivePasswords, JobsAreIndependent)
{
    PasswordTrials trials;
    const auto first = trials.observe("a", Seen::waiting, "", 0);
    const auto second = trials.observe("b", Seen::waiting, "", 0);
    EXPECT_EQ(first.password, "DLPSGAME.COM");
    EXPECT_EQ(second.password, "DLPSGAME.COM");
    EXPECT_EQ(trials.tried_count("a"), 1);
    EXPECT_EQ(trials.tried_count("b"), 1);
}

// ---- the shared editing keys of every prompt kind ----

hui::InputFrame pressed(hui::Action action, float trigger_r = 0.0f)
{
    hui::InputFrame input;
    input.connected = true;
    input.pressed = hui::action_bit(action);
    input.held = hui::action_bit(action);
    input.trigger_r = trigger_r;
    return input;
}

struct PromptKind
{
    const char *name;
    bool secret;
    int max_length;
    const char *initial;
};

// Archive password, API token, direct URL, folder name, rename.
const PromptKind kKinds[] = {
    {"archive password", true, 120, ""}, {"token", true, 200, ""},
    {"url", false, 500, "https://"},      {"folder name", false, 120, "New"},
    {"rename", false, 255, "Game.pkg"},
};

TEST(PromptParity, EveryKindMovesTheCaretTypesThereAndClears)
{
    for (const PromptKind &kind : kKinds)
    {
        SCOPED_TRACE(kind.name);
        hui::ui::Feedback feedback;
        TextPrompt prompt;
        TextPrompt::Options options;
        options.password = kind.secret;
        options.max_length = kind.max_length;
        prompt.open(feedback, kind.name, kind.initial, options);
        const int length = prompt.prompt().field.length();
        const auto send = [&](const hui::InputFrame &input)
        { return prompt.update(input, 1.0f / 60.0f, feedback); };
        send(pressed(hui::Action::page_prev));
        EXPECT_EQ(prompt.prompt().field.caret_position(), length > 0 ? length - 1 : 0);
        send(pressed(hui::Action::page_next));
        EXPECT_EQ(prompt.prompt().field.caret_position(), length);
        prompt.type_text("Z");
        EXPECT_EQ(prompt.text().back(), 'Z');
        send(pressed(hui::Action::l3));
        EXPECT_TRUE(prompt.text().empty());
        EXPECT_EQ(send(pressed(hui::Action::jump_next, 1.0f)), TextPrompt::Result::none); // empty
        EXPECT_TRUE(prompt.is_open());
    }
}

TEST(PromptParity, EveryKindSubmitsOnR2FullPullAndOnOptionsAndCancelsOnCircle)
{
    for (const PromptKind &kind : kKinds)
    {
        SCOPED_TRACE(kind.name);
        for (int how = 0; how < 4; ++how)
        {
            hui::ui::Feedback feedback;
            TextPrompt prompt;
            TextPrompt::Options options;
            options.password = kind.secret;
            prompt.open(feedback, kind.name, "abc", options);
            const hui::InputFrame input =
                how == 0   ? pressed(hui::Action::jump_next, 1.0f)
                : how == 1 ? pressed(hui::Action::menu)
                : how == 2 ? pressed(hui::Action::back)
                           : pressed(hui::Action::jump_next, 0.5f);
            const auto result = prompt.update(input, 1.0f / 60.0f, feedback);
            if (how <= 1)
                EXPECT_EQ(result, TextPrompt::Result::accepted);
            else if (how == 2)
                EXPECT_EQ(result, TextPrompt::Result::cancelled);
            else
                EXPECT_EQ(result, TextPrompt::Result::none); // half a pull never submits
        }
    }
}

TEST(PromptParity, SecretsStayMaskedUntilTriangleShowsThemAndHaveNoRecents)
{
    hui::ui::Feedback feedback;
    TextPrompt prompt;
    TextPrompt::Options options;
    options.password = true;
    options.note = "Tried 6 known passwords";
    prompt.open(feedback, "Archive password", "", options);
    prompt.type_text("hako");
    EXPECT_TRUE(prompt.prompt().field.style.password);
    prompt.update(pressed(hui::Action::north), 1.0f / 60.0f, feedback);
    EXPECT_FALSE(prompt.prompt().field.style.password);
    EXPECT_TRUE(prompt.prompt().revealed());
    EXPECT_EQ(prompt.text(), "hako"); // showing is not an edit
    prompt.update(pressed(hui::Action::north), 1.0f / 60.0f, feedback);
    EXPECT_TRUE(prompt.prompt().field.style.password);
    EXPECT_TRUE(prompt.prompt().chips().empty());
    // A fresh open is masked again, and a plain prompt has no reveal key.
    prompt.update(pressed(hui::Action::north), 1.0f / 60.0f, feedback);
    prompt.open(feedback, "Archive password", "", options);
    EXPECT_FALSE(prompt.prompt().revealed());
    TextPrompt::Options plain;
    prompt.open(feedback, "Folder name", "", plain);
    prompt.update(pressed(hui::Action::north), 1.0f / 60.0f, feedback);
    EXPECT_EQ(prompt.text(), " "); // Triangle is Space here
    EXPECT_FALSE(prompt.prompt().field.style.password);
}

} // namespace
} // namespace orbit
