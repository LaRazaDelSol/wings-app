// Wings TV app - The Library tab: installed games, APR Emu, delete.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flows follow Pegasus DL's own installed-games table (recon/pegasus-app.js:
// APR Emu update and reinstall with job polling, delete with a job). Delete is
// asked twice, the second time by typing the title id.

#include "orbit/screens.hpp"

#include "orbit/model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace orbit
{

namespace
{
constexpr float kRow = 100.0f;
constexpr float kPitch = 112.0f;
const InputFrame kIdle{};

std::string folded(const std::string &text)
{
    std::string out;
    for (const char c : text)
    {
        if (c != ' ')
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}
} // namespace

LibraryScreen::LibraryScreen(Context &context) : Screen(context)
{
}

void LibraryScreen::focus()
{
    ctx_.store.refresh_now();
}

bool LibraryScreen::modal() const
{
    return prompt_.is_open() || confirm_.is_open() || list_.is_open() ||
           ctx_.store.lane_state(Store::lane_admin).busy;
}

const InstalledGame *LibraryScreen::focused_game() const
{
    const std::vector<InstalledGame> &games = ctx_.store.state().installed;
    if (index_ < 0 || index_ >= static_cast<int>(games.size()))
        return nullptr;
    return &games[static_cast<std::size_t>(index_)];
}

const InstalledGame *LibraryScreen::game(const std::string &title_id) const
{
    for (const InstalledGame &g : ctx_.store.state().installed)
    {
        if (g.title_id == title_id)
            return &g;
    }
    return nullptr;
}

// True when Pegasus told us (in the capability probe, or when asked) that the
// list of APR Emu builds cannot be read. Updating to the latest still works.
bool LibraryScreen::versions_unavailable() const
{
    if (versions_failed_)
        return true;
    for (const auto &feature : ctx_.store.state().info.features)
    {
        if (feature.first == "APR Emu versions")
            return !feature.second;
    }
    return false;
}

void LibraryScreen::open_menu()
{
    const InstalledGame *g = focused_game();
    if (g == nullptr)
        return;
    target_ = g->title_id;
    std::vector<ListItem> items;
    menu_codes_.clear();
    const auto add = [&](const char *code, std::string label, std::string detail, bool enabled)
    {
        ListItem item;
        item.label = std::move(label);
        item.detail = std::move(detail);
        item.enabled = enabled;
        items.push_back(std::move(item));
        menu_codes_.push_back(code);
    };
    const bool busy = job_.active;
    if (g->apr.present)
    {
        std::string detail;
        if (job_.active)
            detail = tr("Another job is running; wait for it to finish");
        else if (g->apr.can_update)
            detail = g->apr.latest.empty() ? std::string(tr("Install the newest APR Emu build"))
                                           : tr("Install APR Emu {version}", {{"version", g->apr.latest}});
        else if (!g->apr.reason.empty())
            detail = g->apr.reason;
        else
            detail = tr("APR Emu is already up to date");
        add("update", tr("Update APR Emu"), detail, g->apr.can_update && !busy);
        std::string reinstall_detail;
        if (versions_unavailable())
            reinstall_detail = tr("Not available: this Pegasus DL cannot list APR Emu builds (HTTP 503)");
        else if (!g->apr.can_reinstall)
            reinstall_detail = g->apr.reason.empty() ? std::string(tr("Not available for this title"))
                                                     : g->apr.reason;
        else
            reinstall_detail = tr("Pick an exact APR Emu build to install");
        add("reinstall", tr("Reinstall a specific APR Emu build"), reinstall_detail,
            g->apr.can_reinstall && !versions_unavailable() && !busy);
    }
    else
    {
        add("noapr", tr("APR Emu"), tr("This title has no APR Emu runtime"), false);
    }
    add("details", tr("Details"), g->path, true);
    add("delete", tr("Delete this game\xE2\x80\xA6"),
        tr("Permanently removes it from this console"), !busy);
    list_for_ = ListFor::menu;
    list_.open(g->title, g->title_id, std::move(items), ListDialog::Mode::single);
}

void LibraryScreen::start_apr(const InstalledGame &g, const std::string &version_id,
                              const std::string &label)
{
    job_ = Work{};
    job_.active = true;
    job_.inflight = true;
    job_.title_id = g.title_id;
    job_.title = g.title;
    job_.status = "starting";
    job_.detail = version_id.empty() ? std::string(tr("Updating APR Emu"))
                                     : tr("Installing {build}", {{"build", label}});
    const std::string title_id = g.title_id;
    ctx_.store.call<AdminJob>(
        Store::lane_admin,
        [title_id, version_id](Backend &b) { return b.apr_update_start(title_id, version_id); },
        [this](Result<AdminJob> &&r)
        {
            job_.inflight = false;
            if (!r.ok())
            {
                job_ = Work{};
                pending_ = Pending::notice;
                confirm_.notice(tr("APR Emu could not be changed"), r.error);
                return;
            }
            job_.job_id = r.value.job_id;
            job_.status = r.value.status;
            job_.progress = r.value.progress;
            if (!r.value.detail.empty())
                job_.detail = r.value.detail;
            job_.next_poll = ctx_.clock + 0.8f;
            if (r.value.done())
                finish_job(r.value.status == "completed" ? std::string(tr("APR Emu updated"))
                                                         : r.value.error);
        });
}

void LibraryScreen::start_delete(const InstalledGame &g)
{
    job_ = Work{};
    job_.active = true;
    job_.inflight = true;
    job_.deleting = true;
    job_.title_id = g.title_id;
    job_.title = g.title;
    job_.status = "starting";
    job_.detail = tr("Starting delete");
    const std::string title_id = g.title_id;
    ctx_.store.call<AdminJob>(
        Store::lane_admin, [title_id](Backend &b) { return b.game_delete(title_id); },
        [this](Result<AdminJob> &&r)
        {
            job_.inflight = false;
            if (!r.ok())
            {
                job_ = Work{};
                pending_ = Pending::notice;
                confirm_.notice(tr("Nothing was deleted"), r.error);
                return;
            }
            job_.job_id = r.value.job_id;
            job_.status = r.value.status;
            job_.files = r.value.files_deleted;
            job_.next_poll = ctx_.clock + 0.8f;
            if (r.value.done())
                finish_job(r.value.status == "completed" ? tr("{title} deleted", {{"title", job_.title}})
                                                         : r.value.error);
        });
}

void LibraryScreen::finish_job(const std::string &toast)
{
    const bool failed = job_.status == "failed";
    job_.active = false;
    job_.inflight = false;
    if (failed)
    {
        pending_ = Pending::notice;
        confirm_.notice(job_.deleting ? tr("Delete failed") : tr("APR Emu update failed"),
                        toast.empty() ? std::string(tr("Pegasus DL reported a failure.")) : toast);
    }
    else
    {
        toast_ = toast;
    }
    ctx_.store.refresh_now();
}

void LibraryScreen::poll_job()
{
    if (!job_.active || job_.inflight || job_.job_id.empty() || ctx_.clock < job_.next_poll)
        return;
    job_.inflight = true;
    const std::string job_id = job_.job_id;
    const std::string title_id = job_.title_id;
    const bool deleting = job_.deleting;
    ctx_.store.call<AdminJob>(
        Store::lane_poll,
        [job_id, title_id, deleting](Backend &b)
        {
            return deleting ? b.game_delete_status(job_id, title_id)
                            : b.apr_update_status(job_id, title_id);
        },
        [this](Result<AdminJob> &&r)
        {
            job_.inflight = false;
            if (!job_.active)
                return;
            job_.next_poll = ctx_.clock + 0.9f;
            if (!r.ok())
            {
                if (r.status == 0)
                    return; // Pegasus is not answering for a moment: keep asking
                job_.status = "failed";
                finish_job(r.error);
                return;
            }
            job_.status = r.value.status;
            job_.progress = r.value.progress;
            job_.files = r.value.files_deleted;
            if (!r.value.detail.empty())
                job_.detail = r.value.detail;
            if (r.value.done())
            {
                if (r.value.status == "failed")
                    finish_job(r.value.error);
                else
                    finish_job(job_.deleting ? tr("{title} deleted", {{"title", job_.title}})
                                             : std::string(tr("APR Emu updated")));
            }
        });
}

void LibraryScreen::run_menu(const std::string &code, const InstalledGame &g, Feedback &feedback)
{
    target_ = g.title_id;
    if (code == "update")
    {
        pending_ = Pending::update;
        confirm_.open(tr("Update APR Emu?"),
                      tr("Pegasus DL downloads the newest APR Emu build and installs it for {title}. "
                         "The game itself is not touched.",
                         {{"title", g.title}}),
                      tr("Update"), tr("Cancel"), false);
        feedback.play(hui::audio::Cue::modal_open);
    }
    else if (code == "reinstall")
    {
        ctx_.store.call<std::vector<AprVersionItem>>(
            Store::lane_admin, [](Backend &b) { return b.apr_versions(); },
            [this, title_id = g.title_id](Result<std::vector<AprVersionItem>> &&r)
            {
                if (!r.ok())
                {
                    versions_failed_ = true;
                    versions_error_ = r.error;
                    pending_ = Pending::notice;
                    confirm_.notice(
                        tr("The list of APR Emu builds is not available"),
                        tr("Pegasus DL answered: {error}. Updating to the latest build still works; "
                           "choosing an exact build needs that list.",
                           {{"error", r.error}}));
                    return;
                }
                if (r.value.empty())
                {
                    pending_ = Pending::notice;
                    confirm_.notice(tr("No APR Emu builds"), tr("Pegasus DL listed no builds."));
                    return;
                }
                target_ = title_id;
                version_ids_.clear();
                version_labels_.clear();
                std::vector<ListItem> items;
                for (const AprVersionItem &v : r.value)
                {
                    ListItem item;
                    item.label = v.label;
                    items.push_back(std::move(item));
                    version_ids_.push_back(v.id);
                    version_labels_.push_back(v.label);
                }
                list_for_ = ListFor::versions;
                list_.open(tr("Reinstall APR Emu"), tr("Choose the build to install"),
                           std::move(items), ListDialog::Mode::single);
            });
    }
    else if (code == "details")
    {
        std::string text = g.title_id;
        if (!g.version.empty())
            text += "  \xC2\xB7  v" + g.version;
        if (!g.storage.empty())
            text += "  \xC2\xB7  " + g.storage;
        if (!g.firmware.empty())
            text += "  \xC2\xB7  " + g.firmware;
        text += "\n" + g.path;
        if (g.apr.present)
        {
            text += "\n" + tr("APR Emu: {state} {version}",
                              {{"state", g.apr.state}, {"version", g.apr.version_text}});
            if (!g.apr.reason.empty())
                text += "\n" + g.apr.reason;
        }
        pending_ = Pending::notice;
        confirm_.notice(g.title, text);
    }
    else if (code == "delete")
    {
        pending_ = Pending::delete_first;
        confirm_.open(
            tr("Delete {title}?", {{"title", g.title}}),
            tr("{title} ({id}) is permanently deleted from this console: its files, its library "
               "entry and its home-screen tile. This cannot be undone. Next you are asked to type "
               "the title id.",
               {{"title", g.title}, {"id", g.title_id}}),
            tr("Continue"), tr("Keep the game"), true);
        feedback.play(hui::audio::Cue::modal_open);
    }
}

Intent LibraryScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    ctx_.store.want_installed(true);
    if (job_.active)
        ctx_.store.set_fast(true);
    const int count = static_cast<int>(state.installed.size());
    index_ = std::clamp(index_, 0, std::max(0, count - 1));

    const bool had_prompt = prompt_.is_open();
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();
    const bool busy = ctx_.store.lane_state(Store::lane_admin).busy;

    // The typed confirmation of a delete.
    const TextPrompt::Result typed = prompt_.update(had_prompt ? input : kIdle, dt, feedback);
    if (typed == TextPrompt::Result::accepted)
    {
        const InstalledGame *g = game(target_);
        if (g != nullptr && folded(prompt_.text()) == folded(g->title_id))
        {
            start_delete(*g);
        }
        else
        {
            pending_ = Pending::notice;
            confirm_.notice(tr("Nothing was deleted"),
                            tr("What you typed is not the title id, so the game was left alone."));
        }
    }

    const ConfirmDialog::Result confirmed =
        confirm_.update(had_confirm ? input : kIdle, dt, feedback);
    if (confirmed == ConfirmDialog::Result::confirmed)
    {
        const InstalledGame *g = game(target_);
        if (g != nullptr && pending_ == Pending::update)
        {
            start_apr(*g, std::string(), std::string());
        }
        else if (g != nullptr && pending_ == Pending::reinstall)
        {
            start_apr(*g, version_id_, version_label_);
        }
        else if (g != nullptr && pending_ == Pending::delete_first)
        {
            TextPrompt::Options options;
            options.max_length = 32;
            options.placeholder = g->title_id;
            options.done_label = tr("Delete game");
            prompt_.open(feedback, tr("Type {id} to delete {title}", {{"id", g->title_id}, {"title", g->title}}),
                         "", options);
        }
    }
    if (confirmed != ConfirmDialog::Result::none)
        pending_ = Pending::none;

    const ListDialog::Result listed = list_.update(had_list ? input : kIdle, dt, feedback);
    if (listed == ListDialog::Result::chosen)
    {
        const int chosen = list_.chosen();
        if (list_for_ == ListFor::menu)
        {
            const InstalledGame *g = game(target_);
            if (g != nullptr && chosen >= 0 && chosen < static_cast<int>(menu_codes_.size()))
                run_menu(menu_codes_[static_cast<std::size_t>(chosen)], *g, feedback);
        }
        else if (list_for_ == ListFor::versions && chosen >= 0 &&
                 chosen < static_cast<int>(version_ids_.size()))
        {
            version_id_ = version_ids_[static_cast<std::size_t>(chosen)];
            version_label_ = version_labels_[static_cast<std::size_t>(chosen)];
            const InstalledGame *g = game(target_);
            if (g != nullptr)
            {
                pending_ = Pending::reinstall;
                confirm_.open(tr("Reinstall APR Emu?"),
                              tr("Pegasus DL installs {build} for {title}, replacing the APR Emu "
                                 "build it has now.",
                                 {{"build", version_label_}, {"title", g->title}}),
                              tr("Reinstall"), tr("Cancel"), false);
            }
        }
    }

    poll_job();

    const bool panel = had_prompt || had_confirm || had_list || busy;
    if (focused && !panel && state.online())
    {
        if (input.nav == Direction::up)
        {
            if (index_ > 0)
            {
                --index_;
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
            open_menu();
            feedback.play(hui::audio::Cue::modal_open);
        }
        if (input.is_pressed(Action::north))
        {
            ctx_.store.refresh_now();
            intent.toast = tr("Reading installed games");
        }
    }
    else if (focused && !state.online() && input.nav == Direction::up)
    {
        intent.kind = Intent::Kind::top_bar;
    }
    if (!toast_.empty() && intent.toast.empty())
        intent.toast = toast_;
    toast_.clear();

    row_focus_.resize(static_cast<std::size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i)
    {
        float &value = row_focus_[static_cast<std::size_t>(i)];
        const float target = focused && !panel && i == index_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    scroll_.target = std::max(0.0f, static_cast<float>(index_ + 1) * kPitch -
                                        (layout::kListBottom - layout::kListTop));
    scroll_.update(dt, 11.0f);
    return intent;
}

void LibraryScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    std::string summary;
    if (state.online() && state.have_installed)
        summary = trn(static_cast<long>(state.installed.size()), "{count} installed game",
                      "{count} installed games");
    draw_heading(ctx_, list, tr("Library"), summary);
    if (!state.online())
    {
        draw_waiting(ctx_, list, 330.0f);
    }
    else if (!state.have_installed)
    {
        ui::text(list, type.regular, tr("Reading installed games\xE2\x80\xA6"), ui::kGutter,
                 layout::kListTop + 40.0f, 26.0f, ui::color::text2);
    }
    else if (state.installed.empty())
    {
        ui::text(list, type.regular, tr("No installed games found."), ui::kGutter,
                 layout::kListTop + 40.0f, 26.0f, ui::color::text2);
    }
    else
    {
        if (versions_unavailable())
            ui::text_fit(list, type.regular,
                         tr("APR Emu: this Pegasus DL cannot list builds (HTTP 503). Updating to "
                            "the latest still works; reinstalling an exact build is off."),
                         ui::kGutter, layout::kFilterY + 30.0f, 20.0f, 1920.0f - 2.0f * ui::kGutter,
                         ui::color::tier_yellow);
        list.push_clip({0.0f, layout::kListTop - 16.0f, 1920.0f,
                        layout::kListBottom - layout::kListTop + 32.0f});
        const float width = 1920.0f - 2.0f * ui::kGutter;
        for (std::size_t i = 0; i < state.installed.size(); ++i)
        {
            const InstalledGame &g = state.installed[i];
            const float y = layout::kListTop + static_cast<float>(i) * kPitch - scroll_.value;
            if (y + kRow < layout::kListTop - 20.0f || y > layout::kListBottom + 20.0f)
                continue;
            const ui::Rect r{ui::kGutter, y, width, kRow};
            const float f = i < row_focus_.size() && focused ? row_focus_[i] : 0.0f;
            ui::focus_ring(list, r, 18.0f, f);
            ui::surface(list, r, 18.0f, ui::Color::rgb(0xffffff, 0.075f + 0.065f * f));
            draw_cover(ctx_, list, "app:" + g.title_id, g.title,
                       {r.x + 14.0f, r.y + 14.0f, 72.0f, 72.0f}, 10.0f);
            const float x = r.x + 110.0f;
            const bool job_here = job_.active && job_.title_id == g.title_id;
            const float right_room = 520.0f;
            const float title_w = std::min(type.medium.measure(g.title, 26.0f), r.w - right_room - 130.0f);
            ui::text_fit(list, type.medium, g.title, x, r.y + 42.0f, 26.0f, title_w + 1.0f,
                         ui::color::text);
            float bx = x + title_w + 16.0f;
            if (g.apr.present)
            {
                const bool update = g.apr.state == "update";
                const bool current = g.apr.state == "current";
                const ui::Color c = update ? ui::color::tier_yellow
                                           : (current ? ui::color::tier_green : ui::color::text2);
                const std::string label =
                    update ? std::string(tr("APR Emu update available"))
                           : (current ? std::string(tr("APR Emu current"))
                                      : std::string(tr("APR Emu")));
                bx += ui::chip(list, type, bx, r.y + 33.0f, label,
                               hui::gfx::mix(update || current ? c.with_alpha(0.17f)
                                                               : ui::Color::rgb(0xffffff, 0.10f),
                                             ui::Color::rgb(0x000000, 0.45f), f),
                               c, 14.0f, update || current ? c.with_alpha(0.6f) : ui::Color{0, 0, 0, 0}) +
                      8.0f;
            }
            std::string detail = g.title_id;
            if (!g.version.empty())
                detail += "  \xC2\xB7  v" + g.version;
            if (g.size_bytes > 0.0)
                detail += "  \xC2\xB7  " + format_bytes(g.size_bytes);
            if (!g.storage.empty())
                detail += "  \xC2\xB7  " + g.storage;
            if (!g.firmware.empty())
                detail += "  \xC2\xB7  " + g.firmware;
            ui::text_fit(list, type.regular, detail, x, r.y + 78.0f, 20.0f,
                         r.w - right_room - 110.0f, hui::gfx::mix(ui::color::text3, ui::color::text, f));
            if (job_here)
            {
                const std::string what =
                    job_.deleting ? tr("Deleting {count} files", {{"count", std::to_string(job_.files)}})
                                  : (job_.detail.empty() ? std::string(tr("Working")) : job_.detail);
                ui::text_fit(list, type.regular, what, r.x + r.w - 30.0f, r.y + 42.0f, 21.0f, 440.0f,
                             ui::color::text, ui::Align::right);
                ui::progress_bar(list, {r.x + r.w - 30.0f - 360.0f, r.y + 62.0f, 360.0f, 8.0f},
                                 job_.deleting ? 0.0f : static_cast<float>(job_.progress) / 100.0f,
                                 ui::color::action);
            }
            else if (!g.apr.version_text.empty())
            {
                ui::text_fit(list, type.regular, g.apr.version_text, r.x + r.w - 30.0f, r.y + 54.0f,
                             21.0f, 440.0f, hui::gfx::mix(ui::color::text2, ui::color::text, f), ui::Align::right);
            }
        }
        list.pop_clip();
    }
    if (ctx_.store.lane_state(Store::lane_admin).busy)
        draw_busy(ctx_, frame.overlay, tr("Asking Pegasus DL\xE2\x80\xA6"));
    prompt_.draw(ctx_, frame);
    confirm_.draw(ctx_, frame);
    list_.draw(ctx_, frame);
}

std::vector<hui::ui::Hint> LibraryScreen::hints() const
{
    return {{hui::ui::Button::cross, tr("Manage")},
            {hui::ui::Button::triangle, tr("Refresh")},
            {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
