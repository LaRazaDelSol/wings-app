// Wings TV app - The search box: caret editing, bindings per prompt kind, the
// debounce, recent searches and what a search matches.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake_backend.hpp"
#include "orbit/catalogue_view.hpp"
#include "orbit/panels.hpp"
#include "orbit/screens.hpp"
#include "orbit/store.hpp"
#include "support.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/text_field.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>

namespace orbit
{
namespace
{

using hui::Action;
using hui::Direction;
using hui::InputFrame;

InputFrame pressed(Action action, float trigger_r = 0.0f)
{
    InputFrame input;
    input.connected = true;
    input.pressed = hui::action_bit(action);
    input.held = hui::action_bit(action);
    input.trigger_r = trigger_r;
    return input;
}

// ---- TextField caret ----

TEST(SearchTextField, EditsHappenAtTheCaretAndTheDefaultIsTheEnd)
{
    hui::ui::TextField field;
    field.set_text("abcd");
    EXPECT_EQ(field.caret(), 4u);
    field.insert('e'); // at the end, as before
    EXPECT_EQ(field.text(), "abcde");
    EXPECT_TRUE(field.move_caret(-2));
    EXPECT_EQ(field.caret_position(), 3);
    field.insert("XY");
    EXPECT_EQ(field.text(), "abcXYde");
    EXPECT_EQ(field.caret_position(), 5);
    EXPECT_TRUE(field.backspace()); // removes Y
    EXPECT_EQ(field.text(), "abcXde");
    field.move_caret(-10); // clamped
    EXPECT_EQ(field.caret(), 0u);
    EXPECT_FALSE(field.backspace()); // nothing before the caret
    EXPECT_FALSE(field.move_caret(-1));
    EXPECT_TRUE(field.move_caret(100));
    EXPECT_EQ(field.caret(), field.text().size());
    EXPECT_FALSE(field.move_caret(1));
}

TEST(SearchTextField, CaretStepsOverWholeUtf8CharactersAndSetTextResetsIt)
{
    hui::ui::TextField field;
    field.set_text("a\xC3\xA9\xE6\x97\xA5z"); // a, e-acute, a CJK character, z
    EXPECT_EQ(field.length(), 4);
    field.move_caret(-1);
    field.move_caret(-1); // before the CJK character
    EXPECT_EQ(field.caret_position(), 2);
    EXPECT_TRUE(field.backspace()); // removes the e-acute as a whole
    EXPECT_EQ(field.text(), "a\xE6\x97\xA5z");
    field.insert("\xC3\xB1");
    EXPECT_EQ(field.text(), "a\xC3\xB1\xE6\x97\xA5z");
    field.set_text("new");
    EXPECT_EQ(field.caret(), 3u);
    field.move_caret(-1);
    field.clear();
    EXPECT_EQ(field.caret(), 0u);
    EXPECT_TRUE(field.text().empty());
}

TEST(SearchTextField, TheLengthLimitStillHoldsWhenInsertingInTheMiddle)
{
    hui::ui::TextField field;
    field.style.max_length = 4;
    field.set_text("abc");
    field.move_caret(-1);
    EXPECT_TRUE(field.insert('X'));
    EXPECT_FALSE(field.insert('Y'));
    EXPECT_EQ(field.text(), "abXc");
}

// ---- keyboard bindings ----

struct KeysRig
{
    hui::ui::Keyboard keys;
    hui::ui::Feedback feedback;
    KeysRig(hui::ui::KeyboardBindings bindings, int length = 3)
    {
        keys.style.bindings = bindings;
        keys.set_bounds({0.0f, 0.0f, 1000.0f, 400.0f});
        keys.set_length(length);
    }
    hui::ui::Event send(const InputFrame &input)
    {
        return keys.handle(input, feedback);
    }
};

TEST(SearchBindings, R2IsDoneAndNoLongerSwitchesTheLayout)
{
    KeysRig rig(hui::ui::KeyboardBindings::search());
    const int layout = rig.keys.layout();
    EXPECT_EQ(rig.send(pressed(Action::jump_next, 1.0f)), hui::ui::Event::activated);
    EXPECT_EQ(rig.keys.layout(), layout);
}

TEST(SearchBindings, AHalfPulledTriggerNeverSubmits)
{
    KeysRig rig(hui::ui::KeyboardBindings::search());
    // The input layer reports the press at about half a pull ...
    EXPECT_NE(rig.send(pressed(Action::jump_next, 0.5f)), hui::ui::Event::activated);
    // ... and the submit waits for the full pull, once.
    InputFrame held;
    held.connected = true;
    held.held = hui::action_bit(Action::jump_next);
    held.trigger_r = 0.7f;
    EXPECT_NE(rig.send(held), hui::ui::Event::activated);
    held.trigger_r = 0.95f;
    EXPECT_EQ(rig.send(held), hui::ui::Event::activated);
    EXPECT_NE(rig.send(held), hui::ui::Event::activated);
    // Letting go before the full pull cancels it.
    InputFrame open;
    open.connected = true;
    EXPECT_NE(rig.send(pressed(Action::jump_next, 0.5f)), hui::ui::Event::activated);
    open.trigger_r = 0.0f;
    rig.send(open);
    held.trigger_r = 0.95f;
    EXPECT_NE(rig.send(held), hui::ui::Event::activated);
}

TEST(SearchBindings, SquareDeletesL2ShiftsR3SwitchesLayoutL3Clears)
{
    KeysRig rig(hui::ui::KeyboardBindings::search());
    rig.send(pressed(Action::west));
    EXPECT_EQ(rig.keys.erased(), 1);
    rig.send(pressed(Action::jump_prev));
    EXPECT_EQ(rig.keys.shift(), hui::ui::KeyboardShift::once);
    const int layout = rig.keys.layout();
    rig.send(pressed(Action::r3));
    EXPECT_NE(rig.keys.layout(), layout);
    rig.send(pressed(Action::l3));
    EXPECT_TRUE(rig.keys.cleared());
    rig.send(pressed(Action::north));
    EXPECT_EQ(rig.keys.typed(), " ");
}

// KeyboardBindings::standard() is still the kit's default mapping (components that opt in); the
// app's prompts moved to KeyboardBindings::prompt().
TEST(SearchBindings, StandardBindingsAreUnchanged)
{
    const auto standard = hui::ui::KeyboardBindings::standard();
    EXPECT_EQ(standard.done, Action::count);
    EXPECT_EQ(standard.layout, Action::jump_next);
    EXPECT_EQ(standard.clear, Action::count);
    KeysRig rig(standard);
    const int layout = rig.keys.layout();
    rig.send(pressed(Action::jump_next, 1.0f));
    EXPECT_NE(rig.keys.layout(), layout);
}

// ---- bindings per prompt kind (TextPrompt::open sets them on every call) ----

// Changed from "only the search prompt gets the search keys": every prompt now has the
// same editing keys (caret, R2 / Options submit, Clear, hints). Secrets differ only in
// the masked field and Triangle (show) instead of space.
TEST(SearchBindings, EveryPromptKindGetsTheSameEditingKeys)
{
    hui::ui::Feedback feedback;
    TextPrompt prompt;
    for (const bool secret : {false, true})
    {
        for (const bool search : {false, true})
        {
            TextPrompt::Options options;
            options.password = secret;
            options.search = search;
            prompt.open(feedback, "Title", "", options);
            const auto &bindings = prompt.prompt().keyboard.style.bindings;
            EXPECT_EQ(bindings.done, Action::jump_next);
            EXPECT_EQ(bindings.layout, Action::r3);
            EXPECT_EQ(bindings.clear, Action::l3);
            EXPECT_EQ(bindings.backspace, Action::west);
            EXPECT_EQ(bindings.shift, Action::jump_prev);
            EXPECT_EQ(bindings.space, secret ? Action::count : Action::north);
            EXPECT_EQ(prompt.prompt().style.reveal_key, secret ? Action::north : Action::count);
            EXPECT_TRUE(prompt.prompt().style.caret_keys);
            EXPECT_TRUE(prompt.prompt().style.submit_on_menu);
            EXPECT_TRUE(prompt.prompt().style.clear_button);
            EXPECT_FALSE(prompt.prompt().style.hints.empty());
            EXPECT_TRUE(prompt.prompt().chips().empty()); // no recents outside the search box
        }
    }
}

TEST(SearchBindings, EveryBoundButtonHasAHint)
{
    hui::ui::Feedback feedback;
    TextPrompt prompt;
    for (const bool secret : {false, true})
    {
    TextPrompt::Options search;
    search.search = !secret;
    search.password = secret;
    prompt.open(feedback, "Search", "", search);
    const auto &bound = prompt.prompt().keyboard.style.bindings;
    const auto has = [&](Action action)
    {
        for (const auto &hint : prompt.prompt().style.hints)
            for (const Action a : hint.actions)
                if (a == action)
                    return true;
        return false;
    };
    for (const Action action : {bound.backspace, bound.space, bound.shift, bound.layout, bound.done,
                                bound.clear, Action::page_prev, Action::page_next, Action::menu,
                                Action::back})
        EXPECT_TRUE(action == Action::count || has(action)) << static_cast<int>(action); // count: unbound
    if (secret) // the show toggle is bound to Triangle and has its hint
        EXPECT_TRUE(has(prompt.prompt().style.reveal_key));
    }
}

// ---- the prompt: caret keys, Options, Clear, chips ----

struct PromptRig
{
    TextPrompt prompt;
    hui::ui::Feedback feedback;
    explicit PromptRig(std::vector<std::string> chips = {}, const std::string &initial = "")
    {
        TextPrompt::Options options;
        options.search = true;
        options.allow_empty = true;
        options.chips = std::move(chips);
        prompt.open(feedback, "Search", initial, options);
    }
    TextPrompt::Result send(const InputFrame &input)
    {
        return prompt.update(input, 1.0f / 60.0f, feedback);
    }
};

TEST(SearchPrompt, L1AndR1MoveTheCaretAndTypingGoesThereWithTheDebounceSerial)
{
    PromptRig rig(/*chips*/ {}, "abcd");
    EXPECT_EQ(rig.prompt.changed_serial(), 0u);
    rig.send(pressed(Action::page_prev));
    rig.send(pressed(Action::page_prev));
    EXPECT_EQ(rig.prompt.prompt().field.caret_position(), 2);
    EXPECT_EQ(rig.prompt.changed_serial(), 0u); // moving the caret is not an edit
    rig.send(pressed(Action::page_next));
    EXPECT_EQ(rig.prompt.prompt().field.caret_position(), 3);
    // Square deletes before the caret.
    rig.send(pressed(Action::west));
    EXPECT_EQ(rig.prompt.text(), "abd");
    EXPECT_EQ(rig.prompt.changed_serial(), 1u);
    rig.prompt.type_text("X");
    EXPECT_EQ(rig.prompt.text(), "abXd");
    EXPECT_EQ(rig.prompt.changed_serial(), 2u);
}

TEST(SearchPrompt, OptionsSubmitsAndCircleCancels)
{
    {
        PromptRig rig(/*chips*/ {}, "mario");
        EXPECT_EQ(rig.send(pressed(Action::menu)), TextPrompt::Result::accepted);
        EXPECT_FALSE(rig.prompt.is_open());
    }
    {
        PromptRig rig(/*chips*/ {}, "mario");
        EXPECT_EQ(rig.send(pressed(Action::back)), TextPrompt::Result::cancelled);
    }
    {
        // An empty search is allowed here: it clears the term.
        PromptRig rig;
        EXPECT_EQ(rig.send(pressed(Action::menu)), TextPrompt::Result::accepted);
    }
}

// Changed from "Options does nothing in other prompts": Options now submits in every prompt.
TEST(SearchPrompt, OptionsSubmitsInOtherPromptsToo)
{
    hui::ui::Feedback feedback;
    TextPrompt prompt;
    TextPrompt::Options options;
    prompt.open(feedback, "Name", "x", options);
    EXPECT_EQ(prompt.update(pressed(Action::menu), 1.0f / 60.0f, feedback),
              TextPrompt::Result::accepted);
    EXPECT_FALSE(prompt.is_open());
}

TEST(SearchPrompt, L3ClearsTheTextAndR2SubmitsOnAFullPull)
{
    PromptRig rig(/*chips*/ {}, "zelda");
    rig.send(pressed(Action::l3));
    EXPECT_TRUE(rig.prompt.text().empty());
    EXPECT_EQ(rig.prompt.changed_serial(), 1u);
    rig.prompt.type_text("ab");
    EXPECT_EQ(rig.send(pressed(Action::jump_next, 0.5f)), TextPrompt::Result::none);
    EXPECT_EQ(rig.send(pressed(Action::jump_next, 1.0f)), TextPrompt::Result::accepted);
    EXPECT_EQ(rig.prompt.text(), "ab");
}

TEST(SearchPrompt, UpFromTheKeysEntersTheChipsAndConfirmSubmitsOne)
{
    PromptRig rig({"mario", "zelda"}, "");
    // The focus starts mid-board; go up to the top row and out.
    InputFrame up;
    up.connected = true;
    up.nav = Direction::up;
    for (int i = 0; i < 6 && !rig.prompt.prompt().on_chips(); ++i)
        rig.send(up);
    ASSERT_TRUE(rig.prompt.prompt().on_chips());
    InputFrame right;
    right.connected = true;
    right.nav = Direction::right;
    rig.send(right);
    EXPECT_EQ(rig.prompt.prompt().chip_focus(), 1);
    EXPECT_EQ(rig.send(pressed(Action::confirm)), TextPrompt::Result::accepted);
    EXPECT_EQ(rig.prompt.text(), "zelda");
}

TEST(SearchPrompt, TheClearButtonEmptiesTheFieldAndKeepsThePromptOpen)
{
    PromptRig rig(/*chips*/ {}, "abc");
    InputFrame down;
    down.connected = true;
    down.nav = Direction::down;
    for (int i = 0; i < 12 && !rig.prompt.prompt().on_buttons(); ++i)
        rig.send(down);
    ASSERT_TRUE(rig.prompt.prompt().on_buttons());
    InputFrame left;
    left.connected = true;
    left.nav = Direction::left;
    rig.send(left);
    rig.send(left); // Done, Cancel, Clear
    EXPECT_EQ(rig.send(pressed(Action::confirm)), TextPrompt::Result::none);
    EXPECT_TRUE(rig.prompt.text().empty());
    EXPECT_TRUE(rig.prompt.is_open());
}

// ---- recent searches ----

TEST(RecentSearches, NewestFirstNoDuplicatesCappedAtEight)
{
    CatalogueState state;
    EXPECT_FALSE(state.add_recent_search("   "));
    EXPECT_FALSE(state.add_recent_search(""));
    EXPECT_TRUE(state.add_recent_search("  mario "));
    EXPECT_TRUE(state.add_recent_search("zelda"));
    EXPECT_TRUE(state.add_recent_search("MARIO")); // a repeat in any case moves to the front
    ASSERT_EQ(state.recent_searches.size(), 2u);
    EXPECT_EQ(state.recent_searches[0], "MARIO");
    EXPECT_EQ(state.recent_searches[1], "zelda");
    EXPECT_FALSE(state.add_recent_search("MARIO")); // already first: nothing changed
    for (int i = 0; i < 12; ++i)
        state.add_recent_search("term " + std::to_string(i));
    ASSERT_EQ(state.recent_searches.size(), kMaxRecentSearches);
    EXPECT_EQ(state.recent_searches.front(), "term 11");
    EXPECT_EQ(state.recent_searches.back(), "term 4");
    EXPECT_FALSE(state.add_recent_search("a\nb"));
}

TEST(RecentSearches, PersistInOrderThroughTheSaveContainer)
{
    CatalogueState state;
    state.add_recent_search("one");
    state.add_recent_search("two");
    state.add_recent_search("three");
    const std::string path = std::string(::testing::TempDir()) + "wings_recent_test.v1";
    std::remove(path.c_str());
    ASSERT_EQ(save_catalogue_state({path}, state), 1);
    CatalogueState loaded;
    ASSERT_TRUE(load_catalogue_state({path}, &loaded));
    EXPECT_EQ(loaded.recent_searches, (std::vector<std::string>{"three", "two", "one"}));
    std::remove(path.c_str());
    // A file with too many lines is cut to the cap.
    std::string payload;
    for (int i = 0; i < 20; ++i)
        payload += "recent=t" + std::to_string(i) + "\n";
    CatalogueState many;
    ASSERT_TRUE(decode_catalogue_state(payload, &many));
    EXPECT_EQ(many.recent_searches.size(), kMaxRecentSearches);
    EXPECT_EQ(many.recent_searches.front(), "t0");
}

// ---- what a search matches ----

Package make(const char *title, const char *title_id, const char *source, const char *version = "")
{
    Package p;
    p.id = title;
    p.title = title;
    p.title_id = title_id;
    p.source_name = source;
    p.version = version;
    p.link_count = 1;
    return p;
}

TEST(SearchMatching, TitleTitleIdSourceVersionAndTags)
{
    const Package p = make("[exFAT] Marvels Wolverine Deluxe Edition", "PPSA17517", "evoX-CoreOS",
                           "v1.002.000 USA");
    EXPECT_TRUE(package_matches(p, ""));
    EXPECT_TRUE(package_matches(p, "wolverine"));
    EXPECT_TRUE(package_matches(p, "[EXFAT]"));  // a tag, any case
    EXPECT_TRUE(package_matches(p, "ppsa17517")); // title id
    EXPECT_TRUE(package_matches(p, "evox"));      // source
    EXPECT_TRUE(package_matches(p, "usa"));       // region, from the version text
    EXPECT_FALSE(package_matches(p, "zelda"));
}

TEST(SearchMatching, EveryWordMustMatchSomewhere)
{
    const Package p = make("[exFAT] Marvels Wolverine", "PPSA17517", "Alpha");
    EXPECT_TRUE(package_matches(p, "exfat wolverine"));
    EXPECT_TRUE(package_matches(p, "  wolverine   alpha "));
    EXPECT_FALSE(package_matches(p, "wolverine beta"));
}

TEST(SearchMatching, SelectPackagesUsesIt)
{
    std::vector<Package> all = {make("[exFAT] Alpha", "PPSA00001", "Src A", "USA"),
                                make("Beta", "PPSA00002", "Src B", "EUR")};
    CatalogueState state;
    InstalledIndex none;
    EXPECT_EQ(select_packages(all, {}, state, "exfat alpha", none).size(), 1u);
    EXPECT_EQ(select_packages(all, {}, state, "eur", none).size(), 1u);
    EXPECT_EQ(select_packages(all, {}, state, "src", none).size(), 2u);
}

// ---- the screen: live results, debounce, restore on cancel ----

struct SearchHarness
{
    test::TestFonts fonts;
    host::FakeBackend backend;
    test::FakeSink sink;
    Store store{backend};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    std::unique_ptr<App> app;
    hui::ui::Feedback feedback;

    explicit SearchHarness(const std::string &path = {})
    {
        context.app_version = "00.001.000";
        if (!path.empty())
            context.catalogue_paths = {path};
        app = std::make_unique<App>(context);
        context.folder_confirmed = [] { return true; };
        store.poll_once();
        store.load_catalog_once();
        frame();
        settle(3);
    }
    void frame(const InputFrame &in = {}, float dt = 1.0f / 60.0f)
    {
        InputFrame input = in;
        input.connected = true;
        feedback.clear();
        app->update(input, dt, feedback);
        Frame drawn;
        app->draw(drawn);
    }
    void settle(int n)
    {
        for (int i = 0; i < n; ++i)
            frame();
    }
    // Runs `seconds` of idle frames.
    void wait(float seconds)
    {
        for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f)
            frame();
    }
    void press(Action action, float trigger_r = 0.0f)
    {
        frame(pressed(action, trigger_r));
    }
    StoreScreen &grid()
    {
        return app->store_screen();
    }
};

TEST(SearchScreen, ResultsFollowTheTypingAfterTheDebounceAndAreRecomputedOnce)
{
    SearchHarness h;
    const int all = h.grid().shown_count();
    ASSERT_GT(all, 2);
    h.press(Action::north);
    ASSERT_TRUE(h.grid().modal());
    h.grid().type_into_prompt("zzzqqq");
    h.wait(0.15f);
    EXPECT_EQ(h.grid().shown_count(), all); // still inside the debounce
    EXPECT_TRUE(h.grid().query().empty());
    h.wait(0.2f);
    EXPECT_EQ(h.grid().shown_count(), 0);
    EXPECT_EQ(h.grid().query(), "zzzqqq");
    EXPECT_TRUE(h.grid().modal()); // the box is still open
}

TEST(SearchScreen, EachEditRestartsTheWait)
{
    SearchHarness h;
    const int all = h.grid().shown_count();
    h.press(Action::north);
    h.grid().type_into_prompt("zz");
    h.wait(0.2f);
    h.grid().type_into_prompt("zz"); // another edit just before the first would fire
    h.wait(0.2f);
    EXPECT_EQ(h.grid().shown_count(), all);
    h.wait(0.15f);
    EXPECT_EQ(h.grid().shown_count(), 0);
}

TEST(SearchScreen, CancelRestoresThePreviousTermAndList)
{
    SearchHarness h;
    const int all = h.grid().shown_count();
    h.grid().set_query("a");
    h.settle(3);
    const int before = h.grid().shown_count();
    h.press(Action::north);
    h.grid().type_into_prompt("zzzqqq");
    h.wait(0.4f);
    EXPECT_EQ(h.grid().shown_count(), 0);
    h.press(Action::back);
    h.settle(3);
    EXPECT_FALSE(h.grid().modal());
    EXPECT_EQ(h.grid().query(), "a");
    EXPECT_EQ(h.grid().shown_count(), before);
    EXPECT_LE(before, all);
    EXPECT_TRUE(h.grid().catalogue_state().recent_searches.empty());
}

TEST(SearchScreen, FullPullOfR2KeepsTheTermAndRemembersIt)
{
    SearchHarness h;
    h.press(Action::north);
    h.grid().type_into_prompt("zzzqqq");
    h.press(Action::jump_next, 0.5f); // half a pull: nothing
    EXPECT_TRUE(h.grid().modal());
    h.press(Action::jump_next, 1.0f);
    h.settle(3);
    EXPECT_FALSE(h.grid().modal());
    EXPECT_EQ(h.grid().query(), "zzzqqq");
    ASSERT_EQ(h.grid().catalogue_state().recent_searches.size(), 1u);
    EXPECT_EQ(h.grid().catalogue_state().recent_searches[0], "zzzqqq");
    // The term is kept when the box opens again.
    h.press(Action::north);
    EXPECT_EQ(h.grid().search_prompt().text(), "zzzqqq");
    EXPECT_EQ(h.grid().search_prompt().prompt().chips().size(), 1u);
}

TEST(SearchScreen, OptionsAlsoSubmits)
{
    SearchHarness h;
    h.press(Action::north);
    h.grid().type_into_prompt("qqq");
    h.press(Action::menu);
    h.settle(3);
    EXPECT_FALSE(h.grid().modal());
    EXPECT_EQ(h.grid().query(), "qqq");
}

TEST(SearchScreen, RecentSearchesSurviveARestart)
{
    const std::string path = std::string(::testing::TempDir()) + "wings_search_screen.v1";
    std::remove(path.c_str());
    {
        SearchHarness h(path);
        for (const char *term : {"one", "two", "one"})
        {
            h.press(Action::north);
            h.press(Action::l3); // clear what the last search left
            h.grid().type_into_prompt(term);
            h.press(Action::menu);
            h.settle(3);
        }
    }
    {
        SearchHarness h(path);
        EXPECT_EQ(h.grid().catalogue_state().recent_searches,
                  (std::vector<std::string>{"one", "two"}));
    }
    std::remove(path.c_str());
}

} // namespace
} // namespace orbit
