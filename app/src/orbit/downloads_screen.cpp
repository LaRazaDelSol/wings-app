// Wings TV app - The Downloads tab: the whole queue and what can be done to each job.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row actions, the cancel confirmation and the "add direct link" flow follow
// Pegasus DL's own queue panel (recon/pegasus-app.js: queue actions, archive
// password dialog, manual link dialog) and Orbit Store's downloads screen
// (GPL-3.0-or-later) for focus and dialogs.

#include "orbit/screens.hpp"

#include "orbit/archive_passwords.hpp"
#include "orbit/install_hint.hpp"
#include "orbit/model.hpp"

#include <memory>
#include <algorithm>
#include <cmath>

namespace orbit
{

namespace
{
constexpr float kRow = 104.0f;
constexpr float kPitch = 116.0f;
const InputFrame kIdle{};
} // namespace

void DownloadsScreen::focus()
{
}

bool DownloadsScreen::modal() const
{
    return prompt_.is_open() || confirm_.is_open() || list_.is_open() || probing_;
}

void DownloadsScreen::rebuild()
{
    const Snapshot &state = ctx_.store.state();
    order_.clear();
    for (const Job &job : state.jobs)
    {
        if (job_active(job))
            order_.push_back(job.id);
    }
    for (const Job &job : state.jobs)
    {
        if (!job_active(job))
            order_.push_back(job.id);
    }
}

void DownloadsScreen::open_add_link()
{
    entering_url_ = true;
}

namespace
{

// The known-password trials of the jobs on this screen (one Downloads screen exists).
struct AutoPasswords
{
    PasswordTrials trials;
    std::string in_flight;      // job whose password call has no result yet
    std::uint64_t serial = 0;   // Store::action().serial when it was sent
    double clock_ms = 0.0;
    std::string pending_key;    // a prompt is due for this job (waits for a free prompt)
    std::string pending_note;
};

AutoPasswords &auto_passwords(std::shared_ptr<void> &slot)
{
    if (!slot)
        slot = std::make_shared<AutoPasswords>();
    return *static_cast<AutoPasswords *>(slot.get());
}

PasswordTrials::Seen seen_of(const Job &job)
{
    if (job.status == "waitingPassword")
        return PasswordTrials::Seen::waiting;
    if (job.status == "completed")
        return PasswordTrials::Seen::finished;
    if (job.status == "failed" || job.status == "cancelled")
        return PasswordTrials::Seen::ended;
    return PasswordTrials::Seen::active;
}

} // namespace

void DownloadsScreen::open_menu(const Job &job)
{
    std::vector<ListItem> items;
    menu_codes_.clear();
    menu_job_ = job.id;
    const auto add = [&](const char *code, const char *label, const char *detail)
    {
        ListItem item;
        item.label = label;
        item.detail = detail;
        items.push_back(std::move(item));
        menu_codes_.push_back(code);
    };
    if (job.status == "waitingPassword")
        add("password", tr("Enter the archive password"), tr("The archive is locked"));
    if (job.status == "waitingChoice")
        add("choice", tr("Choose what to keep"), tr("The archive holds more than one package"));
    if (job.status == "paused")
        add("resume", tr("Resume"), tr("Carry on from where it stopped"));
    if (job.status == "failed" || job.status == "cancelled")
        add("retry", tr("Retry"), tr("Put it back in the queue"));
    if (job.status == "downloading" || job.status == "starting" || job.status == "queued")
        add("pause", tr("Pause"), tr("Stop for now, keep what is downloaded"));
    if (job_active(job))
        add("cancel", tr("Cancel download"), tr("Stop it and take it out of the queue"));
    if (job_finished(job))
        add("clear", tr("Clear finished downloads"), tr("Remove completed, failed and cancelled entries"));
    // Only where ShadowMount's API is wired in (the console build).
    if (ctx_.after_install != nullptr && static_cast<bool>(ctx_.shadowmount.transport))
        add("scan", tr("Ask ShadowMount to scan now"),
            tr("Looks for new games. It waits while Wings is open."));
    if (items.empty())
        return;
    list_for_ = ListFor::menu;
    list_.open(job.name, status_label(job.status), std::move(items), ListDialog::Mode::single);
}

void DownloadsScreen::run_menu(const std::string &code, const Job &job, Feedback &feedback)
{
    if (code == "pause" || code == "resume" || code == "retry")
    {
        ctx_.store.job_action(job.id, code);
    }
    else if (code == "cancel")
    {
        pending_ = Pending::cancel_job;
        confirm_.open(tr("Cancel this download?"),
                      tr("{name} stops and leaves the queue. You can add it again from the Store.",
                         {{"name", job.name}}),
                      tr("Cancel download"), tr("Keep it"), true);
        feedback.play(hui::audio::Cue::modal_open);
    }
    else if (code == "clear")
    {
        pending_ = Pending::clear_finished;
        confirm_.open(tr("Clear finished downloads?"),
                      tr("Completed, failed and cancelled entries leave the list. Files already "
                         "downloaded stay where they are."),
                      tr("Clear"), tr("Cancel"), true);
        feedback.play(hui::audio::Cue::modal_open);
    }
    else if (code == "scan")
    {
        if (ctx_.after_install != nullptr)
            ctx_.after_install->scan_now(static_cast<double>(ctx_.clock));
        feedback.play(hui::audio::Cue::select);
    }
    else if (code == "password")
    {
        password_job_ = job.id;
        TextPrompt::Options options;
        options.max_length = 120;
        options.password = true;
        options.done_label = tr("Unlock");
        prompt_.open(feedback, tr("Archive password"), "", options);
    }
    else if (code == "choice")
    {
        std::vector<ListItem> items;
        for (const ArchiveChoice &choice : job.choices)
        {
            ListItem item;
            item.label = choice.name;
            items.push_back(std::move(item));
        }
        if (items.empty())
            return;
        list_for_ = ListFor::archive_choice;
        menu_job_ = job.id;
        list_.open(tr("Choose what to keep"), job.name, std::move(items), ListDialog::Mode::single);
    }
}

Intent DownloadsScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    ctx_.store.set_fast(true);
    const Snapshot &state = ctx_.store.state();
    rebuild();
    const int count = static_cast<int>(order_.size());
    index_ = std::clamp(index_, 0, std::max(0, count - 1));

    const bool had_prompt = prompt_.is_open();
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();

    // The prompt for a direct link is opened here, in the frame it was asked for.
    if (entering_url_)
    {
        entering_url_ = false;
        TextPrompt::Options options;
        options.max_length = 500;
        options.placeholder = "https://";
        options.done_label = tr("Check link");
        password_job_.clear();
        prompt_.open(feedback, tr("Direct download link"), "https://", options);
    }

    // Known passwords first: one at a time, never while a prompt is open for the player.
    {
        AutoPasswords &auto_state = auto_passwords(auto_passwords_state_);
        auto_state.clock_ms += static_cast<double>(dt) * 1000.0;
        const std::int64_t now = static_cast<std::int64_t>(auto_state.clock_ms);
        const auto apply = [&](const std::string &key, const PasswordTrials::Step &step)
        {
            if (step.kind == PasswordTrials::Step::Kind::send)
            {
                ctx_.store.archive_password(key, step.password);
                auto_state.in_flight = key;
                auto_state.serial = ctx_.store.action().serial;
                auto_state.trials.sent(key, now);
            }
            else if (step.kind == PasswordTrials::Step::Kind::prompt)
            {
                auto_state.pending_key = key;
                auto_state.pending_note = step.note;
            }
        };
        if (!auto_state.in_flight.empty() && !ctx_.store.action().busy &&
            ctx_.store.action().serial != auto_state.serial)
        {
            const std::string key = auto_state.in_flight;
            auto_state.in_flight.clear();
            if (!ctx_.store.action().error.empty())
                apply(key, auto_state.trials.send_failed(key, ctx_.store.action().error));
        }
        if (state.have_queue)
        {
            for (const std::string &key : auto_state.trials.keys())
                if (state.job(key) == nullptr)
                    auto_state.trials.observe(key, PasswordTrials::Seen::ended, {}, now);
            for (const Job &job : state.jobs)
            {
                const PasswordTrials::Seen seen = seen_of(job);
                // One call in flight at a time: nothing else is observed as waiting meanwhile.
                if (seen == PasswordTrials::Seen::waiting && !auto_state.in_flight.empty())
                    continue;
                if (auto_state.pending_key == job.id && seen != PasswordTrials::Seen::waiting)
                    auto_state.pending_key.clear();
                apply(job.id, auto_state.trials.observe(job.id, seen, job.error + "|" + job.detail, now));
            }
        }
        if (!auto_state.pending_key.empty() && !prompt_.is_open() && !had_prompt && !list_.is_open() &&
            !confirm_.is_open() && !probing_)
        {
            const Job *job = state.job(auto_state.pending_key);
            if (job != nullptr && job->status == "waitingPassword")
            {
                password_job_ = job->id;
                TextPrompt::Options options;
                options.max_length = 120;
                options.password = true;
                options.done_label = tr("Unlock");
                options.note = auto_state.pending_note;
                prompt_.open(feedback, tr("Archive password"), "", options);
            }
            auto_state.pending_key.clear();
            auto_state.pending_note.clear();
        }
    }

    const TextPrompt::Result typed = prompt_.update(had_prompt ? input : kIdle, dt, feedback);
    if (typed == TextPrompt::Result::accepted)
    {
        if (!password_job_.empty())
        {
            ctx_.store.archive_password(password_job_, prompt_.text());
            {
                const Job *asked = state.job(password_job_);
                auto_passwords(auto_passwords_state_).trials.set_user_password(
                    password_job_, prompt_.text(), asked != nullptr ? asked->error + "|" + asked->detail : "",
                    static_cast<std::int64_t>(auto_passwords(auto_passwords_state_).clock_ms));
            }
            intent.toast = tr("Password sent to Pegasus DL");
            password_job_.clear();
        }
        else
        {
            probing_ = true;
            seen_probe_action_ = ctx_.store.probe_action().serial;
            seen_probe_ = state.probe_serial;
            ctx_.store.probe_link(prompt_.text());
        }
    }
    const ConfirmDialog::Result confirmed =
        confirm_.update(had_confirm ? input : kIdle, dt, feedback);
    if (confirmed == ConfirmDialog::Result::confirmed)
    {
        if (pending_ == Pending::cancel_job)
        {
            ctx_.store.job_action(menu_job_, "cancel");
            intent.toast = tr("Cancelling the download");
        }
        else if (pending_ == Pending::clear_finished)
        {
            ctx_.store.clear_finished();
            intent.toast = tr("Cleared the finished downloads");
        }
        else if (pending_ == Pending::close_wings)
        {
            // Never automatic: only this confirmed press ends Wings.
            if (ctx_.close_app)
                ctx_.close_app();
            else
                intent.toast = tr("Close Wings from the PlayStation button, then ShadowMount can finish.");
        }
        else if (pending_ == Pending::direct_link)
        {
            ctx_.store.queue_direct(state.probe, "");
            probing_ = true; // wait for the queue reply
            seen_probe_action_ = ctx_.store.probe_action().serial;
            seen_probe_ = state.probe_serial;
        }
    }
    if (confirmed != ConfirmDialog::Result::none)
        pending_ = Pending::none;
    const ListDialog::Result listed = list_.update(had_list ? input : kIdle, dt, feedback);
    if (listed == ListDialog::Result::chosen)
    {
        const Job *job = state.job(menu_job_);
        if (list_for_ == ListFor::menu && job != nullptr && list_.chosen() >= 0 &&
            list_.chosen() < static_cast<int>(menu_codes_.size()))
            run_menu(menu_codes_[static_cast<std::size_t>(list_.chosen())], *job, feedback);
        else if (list_for_ == ListFor::archive_choice && job != nullptr && list_.chosen() >= 0 &&
                 list_.chosen() < static_cast<int>(job->choices.size()))
        {
            ctx_.store.archive_choice(job->id, job->choices[static_cast<std::size_t>(list_.chosen())].index);
            intent.toast = tr("Choice sent to Pegasus DL");
        }
    }

    // A direct link that was being checked or queued.
    const ActionState &probe = ctx_.store.probe_action();
    if (probing_ && !probe.busy && probe.serial != seen_probe_action_)
    {
        probing_ = false;
        seen_probe_action_ = probe.serial;
        if (!probe.error.empty())
        {
            pending_ = Pending::notice;
            confirm_.notice(tr("That link cannot be downloaded"), probe.error);
        }
        else if (state.probe_serial != seen_probe_)
        {
            seen_probe_ = state.probe_serial;
            const Probe &p = state.probe;
            std::string message = p.file_name.empty() ? p.url : p.file_name;
            if (p.length > 0.0)
                message += "  \xC2\xB7  " + format_bytes(p.length);
            message += std::string("  ") + tr("Pegasus DL checked the link and it is a direct download. Add it "
                                 "to the queue?");
            pending_ = Pending::direct_link;
            confirm_.open(tr("Add this download?"), message, tr("Add to queue"), tr("Cancel"), false);
        }
        else
        {
            intent.toast = probe.message.empty() ? std::string(tr("Direct link queued."))
                                                 : probe.message;
        }
    }
    const ActionState &jobs = ctx_.store.action();
    if (jobs.serial != seen_job_action_)
    {
        seen_job_action_ = jobs.serial;
        if (!jobs.error.empty() && intent.toast.empty())
        {
            pending_ = Pending::notice;
            confirm_.notice(tr("Pegasus DL could not do that"), jobs.error);
        }
    }

    const bool panel = had_prompt || had_confirm || had_list || probing_;
    if (!banner_shown())
        banner_focus_ = false;
    if (focused && !panel && state.online() && banner_focus_)
    {
        banner_input(input, feedback, intent);
    }
    else if (focused && !panel && state.online())
    {
        if (input.nav == Direction::up)
        {
            if (index_ > 0)
            {
                --index_;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (banner_shown())
            {
                banner_focus_ = true;
                banner_button_ = 0;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                intent.kind = Intent::Kind::top_bar;
            }
        }
        else if (input.nav == Direction::down)
        {
            if (index_ + 1 < count)
            {
                ++index_;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
        {
            refuse(feedback, input);
        }
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            if (const Job *job = state.job(order_[static_cast<std::size_t>(index_)]))
            {
                open_menu(*job);
                if (list_.is_open())
                    feedback.play(hui::audio::Cue::modal_open);
            }
        }
        if (input.is_pressed(Action::west))
        {
            TextPrompt::Options options;
            options.max_length = 500;
            options.placeholder = "https://";
            options.done_label = tr("Check link");
            password_job_.clear();
            prompt_.open(feedback, tr("Direct download link"), "https://", options);
        }
        if (input.is_pressed(Action::north))
        {
            bool any = false;
            for (const Job &job : state.jobs)
                any = any || job_finished(job);
            if (any)
            {
                pending_ = Pending::clear_finished;
                confirm_.open(tr("Clear finished downloads?"),
                              tr("Completed, failed and cancelled entries leave the list. Files "
                                 "already downloaded stay where they are."),
                              tr("Clear"), tr("Cancel"), true);
                feedback.play(hui::audio::Cue::modal_open);
            }
            else
            {
                refuse(feedback, input);
            }
        }
    }
    else if (focused && !state.online() && input.nav == Direction::up)
    {
        intent.kind = Intent::Kind::top_bar;
    }

    row_focus_.resize(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i)
    {
        float &value = row_focus_[static_cast<std::size_t>(i)];
        const float target = focused && i == index_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    scroll_.target = std::max(0.0f, static_cast<float>(index_ + 1) * kPitch -
                                        (layout::kListBottom - layout::kListTop));
    scroll_.update(dt, 11.0f);
    return intent;
}

void DownloadsScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    std::string summary;
    if (state.online() && state.have_queue)
    {
        int active = 0;
        double speed = 0.0;
        for (const Job &job : state.jobs)
        {
            active += job_active(job) ? 1 : 0;
            speed += job.status == "downloading" ? job.speed : 0.0;
        }
        summary = tr("{active} active, {done} finished",
                     {{"active", std::to_string(active)},
                      {"done", std::to_string(static_cast<int>(state.jobs.size()) - active)}});
        if (speed > 0.0)
            summary += "  \xC2\xB7  " + format_speed(speed);
    }
    draw_heading(ctx_, list, tr("Downloads"), summary);
    if (!state.online())
    {
        draw_waiting(ctx_, list, 330.0f);
    }
    else if (order_.empty())
    {
        ui::text(list, type.regular, tr("The queue is empty. Add a package from the Store, or press Square to add a direct link."),
                 ui::kGutter, layout::kListTop + banner_offset() + 40.0f, 26.0f, ui::color::text2);
    }
    else
    {
        const float shift = banner_offset();
        list.push_clip({0.0f, layout::kListTop + shift - 16.0f, 1920.0f,
                        layout::kListBottom - layout::kListTop - shift + 32.0f});
        const float width = 1920.0f - 2.0f * ui::kGutter;
        for (std::size_t i = 0; i < order_.size(); ++i)
        {
            const Job *job = state.job(order_[i]);
            if (job == nullptr)
                continue;
            const float y = layout::kListTop + shift + static_cast<float>(i) * kPitch - scroll_.value;
            if (y + kRow < layout::kListTop + shift - 20.0f || y > layout::kListBottom + 20.0f)
                continue;
            const ui::Rect r{ui::kGutter, y, width, kRow};
            const float f = i < row_focus_.size() && focused && !banner_focus_ ? row_focus_[i] : 0.0f;
            ui::focus_ring(list, r, 18.0f, f);
            ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.075f + 0.065f * f));

            const float right_w = 400.0f;
            const float title_w = std::min(type.medium.measure(job->name, 26.0f), 780.0f);
            ui::text_fit(list, type.medium, job->name, r.x + 30.0f, r.y + 42.0f, 26.0f, title_w + 1.0f,
                         ui::color::text);
            const bool bad = job->status == "failed" || job->status == "waitingPassword" ||
                             job->status == "waitingChoice";
            ui::chip(list, type, r.x + 30.0f + title_w + 16.0f, r.y + 33.0f, status_label(job->status),
                     bad ? ui::color::action
                         : hui::gfx::mix(ui::Color::rgb(0xffffff, 0.16f), ui::Color::rgb(0x000000, 0.42f), f),
                     bad ? ui::color::night1 : hui::gfx::mix(ui::color::text2, ui::color::text, f), 16.0f);

            // Second line: what is happening, in Pegasus' phases.
            std::string detail;
            const auto add = [&](const std::string &part)
            {
                if (part.empty())
                    return;
                if (!detail.empty())
                    detail += "  \xC2\xB7  ";
                detail += part;
            };
            if (job->status == "failed")
            {
                add(job->error.empty() ? std::string(tr("The download failed")) : job->error);
            }
            else if (job->status == "waitingPassword")
            {
                if (const int n = auto_passwords(auto_passwords_state_).trials.trying_number(job->id); n > 0)
                    add(tr("Trying known passwords ({n}/{total})",
                           {{"n", std::to_string(n)},
                            {"total", std::to_string(kKnownPasswordCount)}}));
                else
                    add(job->detail.empty() ? std::string(tr("Archive password required")) : job->detail);
            }
            else if (job->status == "waitingChoice")
            {
                add(tr("Choose which file to keep"));
            }
            else
            {
                if (job->status == "extracting" && !job->entry.empty())
                    add(job->entry);
                add(job->title_id);
                if (job->total > 0.0 && job->status != "extracting")
                    add(format_bytes(job->received) + " / " + format_bytes(job->total));
                if (job->status == "queued")
                    add(job->source_name);
                if (job->attempts > 1 && job->max_attempts > 0)
                    add(tr("Attempt {n} of {max}", {{"n", std::to_string(job->attempts)},
                                                    {"max", std::to_string(job->max_attempts)}}));
            }
            // A failed install-selection keeps Pegasus' text and gets one plain line under it.
            const std::string install_hint =
                job->status == "failed" ? install_failure_hint(job->error) : std::string();
            const std::string tip = failed_download_tip(job->status, state.settings.connections);
            const ui::Color detail_color = hui::gfx::mix(ui::color::text3, ui::color::text, f);
            if (!tip.empty())
            {
                // Pegasus' error, then the install hint when it matched, then the tip.
                const bool both = !install_hint.empty();
                const ui::Color soft = hui::gfx::mix(ui::color::text2, ui::color::text, f);
                ui::text_fit(list, type.regular, detail, r.x + 30.0f, r.y + (both ? 62.0f : 68.0f), 18.0f,
                             r.w - right_w - 80.0f, detail_color);
                if (both)
                    ui::text_fit(list, type.regular, install_hint, r.x + 30.0f, r.y + 80.0f, 18.0f,
                                 r.w - 60.0f, soft);
                ui::text_fit(list, type.regular, tip, r.x + 30.0f, r.y + (both ? 98.0f : 92.0f), 18.0f,
                             r.w - 60.0f, soft);
            }
            else if (install_hint.empty())
            {
                ui::text_fit(list, type.regular, detail, r.x + 30.0f, r.y + 76.0f, 20.0f,
                             r.w - right_w - 80.0f, detail_color);
            }
            else
            {
                ui::text_fit(list, type.regular, detail, r.x + 30.0f, r.y + 68.0f, 18.0f,
                             r.w - right_w - 80.0f, detail_color);
                ui::text_fit(list, type.regular, install_hint, r.x + 30.0f, r.y + 92.0f, 18.0f,
                             r.w - 60.0f, hui::gfx::mix(ui::color::text2, ui::color::text, f));
            }

            // Right side: speed and ETA while it moves; the fraction while it works on files.
            std::string right;
            if (job->status == "downloading")
            {
                right = format_speed(job->speed);
                const std::string eta = format_eta(job->eta_seconds);
                if (!eta.empty())
                    right += "  \xC2\xB7  " + eta;
            }
            else if (job->status == "merging" || job->status == "extracting" ||
                     job->status == "finalizing")
            {
                const double fraction = job_fraction(*job);
                if (fraction >= 0.0)
                    right = i18n::percent(fraction * 100.0);
            }
            else if (job->status == "waitingPassword" || job->status == "waitingChoice")
            {
                right = tr("Press Cross");
            }
            if (!right.empty())
                ui::text_fit(list, type.regular, right, r.x + r.w - 30.0f, r.y + 42.0f, 22.0f,
                             right_w - 30.0f, hui::gfx::mix(ui::color::text2, ui::color::text, f),
                             ui::Align::right);
            const double fraction = job_fraction(*job);
            const bool waiting = job->status == "waitingPassword" || job->status == "waitingChoice";
            if (fraction >= 0.0 && job_active(*job) && !waiting)
                ui::progress_bar(list, {r.x + r.w - 30.0f - 360.0f, r.y + 62.0f, 360.0f, 8.0f},
                                 static_cast<float>(fraction), ui::color::action);
        }
        list.pop_clip();
    }
    if (probing_)
        draw_busy(ctx_, frame.overlay, tr("Checking the link\xE2\x80\xA6"));
    banner_draw(frame, focused);
    prompt_.draw(ctx_, frame);
    confirm_.draw(ctx_, frame);
    list_.draw(ctx_, frame);
}

std::vector<hui::ui::Hint> DownloadsScreen::hints() const
{
    if (banner_focus_)
        return {{hui::ui::Button::cross, tr("Choose")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    return {{hui::ui::Button::cross, tr("Actions")},
            {hui::ui::Button::square, tr("Add link")},
            {hui::ui::Button::triangle, tr("Clear finished")},
            {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
