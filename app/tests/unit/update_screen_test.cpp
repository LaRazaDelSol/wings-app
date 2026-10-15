// Wings TV app - The update page's words and steps: sizes, time left, the row value, buttons, never backwards.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/update_screen.hpp"
#include "orbit/wings_install.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace orbit
{
namespace
{

using Kind = UpdateView::Kind;
using Step = UpdateView::Step;

constexpr double kMB = 1024.0 * 1024.0;

UpdateView make(Kind kind, Step step = Step::download)
{
    UpdateView v;
    v.kind = kind;
    v.step = step;
    v.current = "0.1.1";
    v.latest = "0.2.0";
    return v;
}

TEST(UpdateScreen, SizesReadAsMegabytesThenGigabytes)
{
    EXPECT_EQ(update_size_text(12.3 * kMB), "12.3 MB");
    EXPECT_EQ(update_size_text(0.0), "0.0 MB");
    EXPECT_EQ(update_size_text(-5.0), "0.0 MB");
    EXPECT_EQ(update_size_text(1.5 * 1024.0 * kMB), "1.5 GB");
}

TEST(UpdateScreen, TimeLeftRoundsUpAndHidesWhenUnknown)
{
    EXPECT_EQ(update_eta_text(9.2), "about 10 s left");
    EXPECT_EQ(update_eta_text(125.0), "about 3 min left");
    EXPECT_EQ(update_eta_text(0.0), "");
    EXPECT_EQ(update_eta_text(-1.0), "");
}

TEST(UpdateScreen, ProgressLineHasEveryKnownPart)
{
    UpdateView v = make(Kind::downloading);
    v.done_bytes = 12.3 * kMB;
    v.total_bytes = 54.7 * kMB;
    v.bytes_per_s = 4.1 * kMB;
    v.eta_s = 10.0;
    EXPECT_EQ(update_progress_text(v), "12.3 MB of 54.7 MB · 4.1 MB/s · about 10 s left");
    v.bytes_per_s = 0.0;
    v.eta_s = 0.0;
    EXPECT_EQ(update_progress_text(v), "12.3 MB of 54.7 MB");
    v.total_bytes = 0.0;
    EXPECT_EQ(update_progress_text(v), "12.3 MB downloaded");
    EXPECT_FLOAT_EQ(update_fraction(v), 0.0f);
    v.total_bytes = 10.0;
    v.done_bytes = 20.0;
    EXPECT_FLOAT_EQ(update_fraction(v), 1.0f);
}

TEST(UpdateScreen, CurrentStepFollowsTheKind)
{
    EXPECT_EQ(update_current_step(make(Kind::available)), -1);
    // Steps: 0 Check, 1 Download, 2 Install, 3 Restart Wings.
    EXPECT_EQ(update_current_step(make(Kind::downloading, Step::check)), 0); // reading the published checksum
    EXPECT_EQ(update_current_step(make(Kind::downloading)), 1);
    EXPECT_EQ(update_current_step(make(Kind::checking)), 1);         // the helper checks its download
    EXPECT_EQ(update_current_step(make(Kind::ready_to_install)), 2); // downloaded and checked
    EXPECT_EQ(update_current_step(make(Kind::installing)), 2);
    EXPECT_EQ(update_current_step(make(Kind::installed)), 3);
    EXPECT_EQ(update_current_step(make(Kind::failed, Step::check)), 0);
    EXPECT_EQ(update_current_step(make(Kind::manual, Step::install)), 2);
}

TEST(UpdateScreen, SettingsRowSaysOneOfThreeThings)
{
    EXPECT_EQ(update_row_value(nullptr), "Up to date");
    const UpdateView available = make(Kind::available);
    EXPECT_EQ(update_row_value(&available), "0.2.0 available");
    const UpdateView installed = make(Kind::installed);
    EXPECT_EQ(update_row_value(&installed), "Installed — restart to finish");
}

TEST(UpdateScreen, ButtonsPerState)
{
    const auto actions = [](const UpdateView &v)
    {
        std::vector<UpdateAction> out;
        for (const UpdateButton &b : update_buttons(v))
            out.push_back(b.action);
        return out;
    };
    using A = UpdateAction;
    EXPECT_EQ(actions(make(Kind::available)), (std::vector<A>{A::update_now, A::back}));
    UpdateView down = make(Kind::downloading);
    EXPECT_TRUE(actions(down).empty());
    down.can_cancel = true;
    EXPECT_EQ(actions(down), (std::vector<A>{A::cancel}));
    EXPECT_EQ(actions(make(Kind::ready_to_install)), (std::vector<A>{A::install, A::cancel}));
    EXPECT_TRUE(actions(make(Kind::installing)).empty());
    EXPECT_EQ(actions(make(Kind::installed)), (std::vector<A>{A::quit_now, A::back}));
    EXPECT_EQ(actions(make(Kind::manual)), (std::vector<A>{A::back}));
    UpdateView failed = make(Kind::failed);
    EXPECT_EQ(actions(failed), (std::vector<A>{A::back}));
    failed.can_retry = true;
    EXPECT_EQ(actions(failed), (std::vector<A>{A::retry, A::back}));
    EXPECT_TRUE(update_buttons(make(Kind::ready_to_install)).front().primary);
}

TEST(UpdateScreen, PageNeverShowsAStepGoingBackwards)
{
    hui::ui::Feedback feedback;
    const hui::InputFrame idle{};
    UpdatePage page;
    page.open(make(Kind::checking));
    EXPECT_EQ(page.shown_step(), 1);
    // A failure reported at an earlier step keeps the step already shown.
    page.update(make(Kind::failed, Step::download), idle, 0.016f, feedback);
    EXPECT_EQ(page.shown_step(), 1);
    page.update(make(Kind::installed), idle, 0.016f, feedback);
    EXPECT_EQ(page.shown_step(), 3);
}

TEST(UpdateScreen, FlowSnapshotMapsToTheView)
{
    WingsFlowSnapshot flow;
    UpdateView view = make(Kind::installed);
    // Nothing newer known: no view, the old one is left alone.
    EXPECT_FALSE(update_view_from_flow(flow, &view));
    EXPECT_EQ(view.kind, Kind::installed);

    flow.state = WingsFlowState::downloading;
    flow.step = WingsFlowStep::download;
    flow.step_index = 1;
    flow.version_text = "0.2.0";
    flow.running_text = "0.1.1";
    flow.text = "Downloading";
    flow.bytes = 10;
    flow.total = 40;
    flow.speed_bps = 5.0;
    flow.eta_s = 6;
    flow.buttons.cancel = true;
    ASSERT_TRUE(update_view_from_flow(flow, &view));
    EXPECT_EQ(view.kind, Kind::downloading);
    EXPECT_EQ(view.current, "0.1.1");
    EXPECT_EQ(view.latest, "0.2.0");
    EXPECT_EQ(view.done_bytes, 10.0);
    EXPECT_EQ(view.total_bytes, 40.0);
    EXPECT_EQ(view.bytes_per_s, 5.0);
    EXPECT_EQ(view.eta_s, 6.0);
    EXPECT_EQ(view.message, "Downloading");
    EXPECT_TRUE(view.can_cancel);
    EXPECT_FALSE(view.can_retry);

    // Unknown speed and time left read as 0 (the progress line leaves them out).
    flow.speed_bps = 0.0;
    flow.eta_s = -1;
    ASSERT_TRUE(update_view_from_flow(flow, &view));
    EXPECT_EQ(view.bytes_per_s, 0.0);
    EXPECT_EQ(view.eta_s, 0.0);

    // A failure at the check step: the reason, the step, Retry.
    flow = WingsFlowSnapshot{};
    flow.state = WingsFlowState::failed;
    flow.step = WingsFlowStep::check;
    flow.step_index = 0;
    flow.text = "Something went wrong";
    flow.reason = "The checksum did not match";
    flow.buttons.retry = true;
    ASSERT_TRUE(update_view_from_flow(flow, &view));
    EXPECT_EQ(view.kind, Kind::failed);
    EXPECT_EQ(view.step, Step::check);
    EXPECT_EQ(view.message, "The checksum did not match");
    EXPECT_TRUE(view.can_retry);
    EXPECT_FALSE(view.can_cancel);

    // Manual: each step is one line, its detail after the label.
    flow = WingsFlowSnapshot{};
    flow.state = WingsFlowState::manual;
    flow.step = WingsFlowStep::install;
    flow.step_index = 2;
    flow.manual_steps = {{"Copy the file", "to /data/homebrew"}, {"Restart", ""}};
    ASSERT_TRUE(update_view_from_flow(flow, &view));
    EXPECT_EQ(view.kind, Kind::manual);
    EXPECT_EQ(view.step, Step::install);
    ASSERT_EQ(view.manual_steps.size(), 2u);
    EXPECT_EQ(view.manual_steps[0], "Copy the file: to /data/homebrew");
    EXPECT_EQ(view.manual_steps[1], "Restart");

    // Every other state maps one to one.
    const std::pair<WingsFlowState, Kind> pairs[] = {
        {WingsFlowState::available, Kind::available},
        {WingsFlowState::checking, Kind::checking},
        {WingsFlowState::ready_to_install, Kind::ready_to_install},
        {WingsFlowState::installing, Kind::installing},
        {WingsFlowState::installed, Kind::installed}};
    for (const auto &[state, kind] : pairs)
    {
        flow = WingsFlowSnapshot{};
        flow.state = state;
        ASSERT_TRUE(update_view_from_flow(flow, &view));
        EXPECT_EQ(view.kind, kind);
    }
}

} // namespace
} // namespace orbit
