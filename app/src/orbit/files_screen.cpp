// Wings TV app - The Files tab: Pegasus DL's file manager, one list at a time.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Behaviour follows Pegasus DL's own file manager (recon/pegasus-file-manager.js)
// and docs/research/files-ux.md: the filesystem from "/" with drive chips
// (Pegasus's shortcut filter), mkdir/rename/copy/move/delete/extract with the
// collision choice, .pkg install, and background operations that are polled
// from the app (whichever tab shows) while one is active. Raw upload and
// download of files is left out on purpose: nothing on the console side can
// send or receive them.
//
// Requests carry their own ids: folder listings, the locations and the
// operation polls never drop each other's answers. A folder answer is dropped
// only when a newer folder request exists; a failed or late listing shows an
// error for the folder asked for (with Retry), never the previous folder's
// rows. Every listing and poll leaves a line in the title log.

#include "orbit/screens.hpp"

#include "orbit/archive_passwords.hpp"
#include "orbit/model.hpp"
#include "wings/logic.hpp"

#include <memory>
#include <algorithm>
#include <cmath>
#include <ctime>

namespace orbit
{

namespace
{
constexpr float kChipY = 212.0f;
constexpr float kChipHeight = 50.0f;
constexpr float kCrumbY = 280.0f;
constexpr float kCrumbHeight = 46.0f;
constexpr float kListTop = 346.0f;
constexpr float kRow = 72.0f;
constexpr float kPitch = 80.0f;
constexpr float kListEnd = 872.0f;
constexpr float kStripY = 886.0f;
constexpr float kStripHeight = 62.0f;
constexpr float kActivePoll = 1.5f; // seconds between operation polls while one runs
constexpr float kIdlePoll = 5.0f;   // ... while the tab shows and none runs
constexpr float kSlowNote = 4.0f;   // a listing this old says Pegasus is slow
constexpr float kDoneShown = 8.0f;  // "Done: ..." stays this long on the strip
constexpr int kPageRows = 6;
constexpr float kPanelPitch = 96.0f;
constexpr int kPanelRows = 6;
const InputFrame kIdle{};

wings::FileEntry as_entry(const FileItem &item)
{
    wings::FileEntry e;
    e.name = item.name;
    e.path = item.path;
    e.directory = item.directory;
    e.file = !item.directory;
    e.type = item.directory ? "dir" : "file";
    return e;
}

std::string op_state(const FileOp &op)
{
    if (op.status == "completed")
        return tr("Done");
    if (op.status == "accepted")
        return tr("Handed to the installer");
    if (op.status == "failed")
        return op.error.empty() ? std::string(tr("Failed")) : tr("Failed: {error}", {{"error", op.error}});
    if (op.status == "cancelled")
        return tr("Cancelled");
    if (op.status == "gone")
        return tr("No longer listed");
    if (files::op_waiting_password(op))
        return op.error.empty() ? std::string(tr("Needs a password")) : op.error;
    if (op.cancel_requested)
        return tr("Cancelling\xE2\x80\xA6");
    if (op.status == "queued")
        return tr("Queued");
    if (op.status == "installing")
        return tr("Installing");
    return tr("Running");
}

std::string when(std::int64_t modified)
{
    if (modified <= 0)
        return std::string();
    const double now = unix_seconds();
    if (now > 0.0 && now - static_cast<double>(modified) < 7.0 * 86400.0 &&
        now >= static_cast<double>(modified))
        return format_age(static_cast<double>(modified), now);
    const std::time_t t = static_cast<std::time_t>(modified);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr)
        return std::string();
    return i18n::date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

std::string ms_since(double started)
{
    return std::to_string(static_cast<long>((now_seconds() - started) * 1000.0)) + " ms";
}

std::string progress_text(const files::OpsTracker::Tracked &t)
{
    const FileOp &op = t.op;
    std::string out;
    const float f = files::op_fraction(op);
    if (f >= 0.0f)
        out = i18n::percent(static_cast<double>(f) * 100.0);
    else
        out = tr("Preparing");
    if (op.total_bytes > 0.0)
        out += "  \xC2\xB7  " + files::format_size(op.done_bytes) + " / " + files::format_size(op.total_bytes);
    else if (op.total_items > 1)
        out += "  \xC2\xB7  " + tr("{done} of {total} items", {{"done", std::to_string(op.done_items)},
                                                               {"total", std::to_string(op.total_items)}});
    const double eta = files::OpsTracker::eta_seconds(t);
    if (eta >= 0.0)
        out += "  \xC2\xB7  ~" + format_eta(eta);
    return out;
}

// Known-password trials of the extractions on this tab (one Files screen exists).
struct AutoExtract
{
    PasswordTrials trials;
    std::string in_flight;     // operation whose password call has no answer yet
    std::string pending_key;   // a prompt is due for this operation
    std::string pending_note;  // its note ("Tried 6 known passwords")
    std::string note;          // the note ask_password() shows once
};

AutoExtract &auto_extract(std::shared_ptr<void> &slot)
{
    if (!slot)
        slot = std::make_shared<AutoExtract>();
    return *static_cast<AutoExtract *>(slot.get());
}

} // namespace

FilesScreen::FilesScreen(Context &context) : Screen(context)
{
    trace_.sink = [this](const std::string &line)
    {
        if (ctx_.log_line)
            ctx_.log_line(line);
    };
}

bool FilesScreen::modal() const
{
    return prompt_.is_open() || confirm_.is_open() || list_.is_open() || dest_.is_open() || ops_open_;
}

void FilesScreen::trace(const std::string &line)
{
    trace_.add(line);
}

void FilesScreen::focus()
{
    // The drive list is re-read now and then (a USB drive may have come or gone).
    if (have_locations_ && ctx_.clock - locations_at_ > 60.0f && !locations_loading_)
        have_locations_ = false;
}

void FilesScreen::report(const std::string &error)
{
    pending_ = Pending::notice;
    confirm_.notice(tr("Pegasus DL could not do that"), error);
}

// ---- reading ----

void FilesScreen::load_locations()
{
    locations_loading_ = true;
    const std::uint64_t id = ++locations_req_;
    const double asked = now_seconds();
    ctx_.store.call<std::vector<StorageMount>>(
        Store::lane_poll, [](Backend &b) { return b.file_locations(); },
        [this, id, asked](Result<std::vector<StorageMount>> &&r)
        {
            if (id != locations_req_)
                return;
            locations_loading_ = false;
            locations_at_ = ctx_.clock;
            if (!r.ok())
            {
                // The chips still show Main drive and Root; try again on the next focus.
                trace("locations #" + std::to_string(id) + " error after " + ms_since(asked) + ": " + r.error);
                have_locations_ = true;
                locations_at_ = ctx_.clock - 50.0f;
                return;
            }
            trace("locations #" + std::to_string(id) + " ok " + std::to_string(r.value.size()) +
                  " mounts, " + ms_since(asked));
            have_locations_ = true;
            locations_ = std::move(r.value);
            drives_ = files::drives(locations_);
        });
}

void FilesScreen::open_path(const std::string &path)
{
    started_ = true;
    area_ = Area::list;
    navigate(path, Nav::open, std::string());
}

void FilesScreen::navigate(const std::string &path, Nav how, const std::string &focus_name)
{
    const std::string target = files::normalize(path);
    const bool same = target == path_ && listed_ok_;
    if (how != Nav::refresh || !same)
    {
        if (target != path_)
        {
            marks_.clear();
            filter_.clear();
        }
        // Never the previous folder's rows under the new path.
        path_ = target;
        listed_ok_ = false;
        listed_.clear();
        entries_.clear();
        index_ = 0;
        scroll_.snap(0.0f);
    }
    loading_ = true;
    error_.clear();
    load_started_ = ctx_.clock;
    const std::uint64_t id = ++folder_req_;
    const double asked = now_seconds();
    folder_asked_at_ = asked;
    const char *kind = how == Nav::walk_up ? "up" : (how == Nav::refresh ? "refresh" : "open");
    trace("list #" + std::to_string(id) + " " + target + " asked (" + kind + ")");
    ctx_.store.call<FolderListing>(
        Store::lane_poll, [target](Backend &b) { return b.file_list(target); },
        [this, id, target, how, focus_name, asked](Result<FolderListing> &&r)
        {
            if (id != folder_req_)
            {
                // Only a newer folder request makes an answer stale.
                trace("list #" + std::to_string(id) + " " + target + " dropped-stale after " +
                      ms_since(asked) + " (newer #" + std::to_string(folder_req_) + ")");
                return;
            }
            loading_ = false;
            if (!r.ok())
            {
                trace("list #" + std::to_string(id) + " " + target + " error after " + ms_since(asked) +
                      ": " + r.error);
                const std::string up = files::parent(target);
                if (how == Nav::walk_up && !up.empty())
                {
                    // Going up into a folder the console will not list: keep going up.
                    trace("list " + target + " cannot be listed; trying " + up);
                    navigate(up, Nav::walk_up, files::base_name(target));
                    return;
                }
                error_ = r.error;
                listed_ok_ = false;
                listed_.clear();
                entries_.clear();
                error_button_ = 0;
                return;
            }
            trace("list #" + std::to_string(id) + " " + target + " ok " +
                  std::to_string(r.value.entries.size()) + " entries, " + ms_since(asked));
            std::string keep = focus_name;
            if (keep.empty() && how == Nav::refresh)
            {
                const FileItem *item = focused_item();
                if (item != nullptr)
                    keep = item->name;
            }
            path_ = files::normalize(r.value.path.empty() ? target : r.value.path);
            writable_ = r.value.writable;
            listed_ = std::move(r.value.entries);
            listed_ok_ = true;
            error_.clear();
            rebuild_shown(keep);
        });
}

void FilesScreen::rebuild_shown(const std::string &focus_name)
{
    entries_.clear();
    for (const FileItem &item : listed_)
    {
        if (files::matches(item, filter_))
            entries_.push_back(item);
    }
    files::sort_entries(entries_, sort_, sort_descending_);
    marks_.erase(std::remove_if(marks_.begin(), marks_.end(),
                                [&](const std::string &p)
                                {
                                    return std::none_of(listed_.begin(), listed_.end(),
                                                        [&](const FileItem &e) { return e.path == p; });
                                }),
                 marks_.end());
    if (!focus_name.empty())
    {
        for (std::size_t i = 0; i < entries_.size(); ++i)
        {
            if (entries_[i].name == focus_name)
                index_ = static_cast<int>(i);
        }
    }
    index_ = std::clamp(index_, 0, std::max(0, static_cast<int>(entries_.size()) - 1));
    row_focus_.resize(entries_.size(), 0.0f);
}

void FilesScreen::refresh()
{
    // A navigation in flight wins: its answer is what the player waits for.
    if (loading_)
        return;
    navigate(path_, Nav::refresh, std::string());
}

void FilesScreen::set_filter(const std::string &text)
{
    filter_ = text;
    index_ = 0;
    scroll_.snap(0.0f);
    rebuild_shown(std::string());
}

void FilesScreen::go_up(Feedback &feedback, Intent &intent)
{
    if (path_ == "/")
    {
        intent.kind = Intent::Kind::top_bar;
        return;
    }
    navigate(files::parent(path_), Nav::walk_up, files::base_name(path_));
    feedback.play(hui::audio::Cue::focus);
}

// ---- operations ----

void FilesScreen::background(bool visible)
{
    visible_ = visible;
    if (!ctx_.store.state().online())
        return;
    if (visible)
    {
        if (!have_locations_ && !locations_loading_)
            load_locations();
        if (!started_)
        {
            started_ = true;
            navigate(path_, Nav::walk_up, std::string());
        }
    }
    const bool active = ops_.any_active() || posts_ > 0;
    if (!active && !visible)
        return; // nothing running and nobody looking: no polling
    if (!ops_inflight_ && ctx_.clock >= next_ops_poll_)
        poll_ops();
}

void FilesScreen::poll_ops()
{
    ops_inflight_ = true;
    const std::uint64_t id = ++ops_req_;
    const double asked = now_seconds();
    ctx_.store.call<std::vector<FileOp>>(
        Store::lane_poll, [](Backend &b) { return b.file_operations(); },
        [this, id, asked](Result<std::vector<FileOp>> &&r)
        {
            ops_inflight_ = false;
            if (!r.ok())
            {
                ops_.poll_failed(r.error);
                trace("ops poll #" + std::to_string(id) + " error after " + ms_since(asked) + ": " + r.error +
                      " (" + std::to_string(ops_.failures()) + " in a row)");
                next_ops_poll_ = ctx_.clock + (ops_.any_active() ? kActivePoll : kIdlePoll);
                return;
            }
            const std::size_t before = ops_.active().size();
            take_ops(r.value, "poll #" + std::to_string(id));
            if (ops_.active().size() != before)
                trace("ops poll #" + std::to_string(id) + " ok " + std::to_string(r.value.size()) +
                      " listed, " + std::to_string(ops_.active().size()) + " active, " + ms_since(asked));
        });
}

void FilesScreen::take_ops(const std::vector<FileOp> &ops, const std::string &why)
{
    const std::vector<FileOp> finished = ops_.take(ops, ctx_.clock);
    bool touched = false;
    for (const FileOp &op : finished)
    {
        trace("op " + op.id + " " + op.type + " " + op.status + (op.error.empty() ? "" : ": " + op.error) +
              " (" + why + ")");
        if (op.status == "failed")
            failed_unseen_ = true;
        touched = touched || files::op_touches(op, path_);
    }
    // Reload the visible folder once, only if an operation changed it and no
    // navigation is in flight (Pegasus operationTouchesPath).
    if (touched && listed_ok_ && !loading_)
    {
        trace("refresh " + path_ + " after an operation finished");
        refresh();
    }
    next_ops_poll_ = ctx_.clock + (ops_.any_active() ? kActivePoll : kIdlePoll);
    // An extraction that needs a password: the known passwords go first, one at
    // a time; the prompt opens (once) only when none of them worked.
    AutoExtract &auto_state = auto_extract(auto_extract_state_);
    const std::int64_t now = static_cast<std::int64_t>(ctx_.clock * 1000.0);
    const auto apply = [&](const std::string &key, const PasswordTrials::Step &step)
    {
        if (step.kind == PasswordTrials::Step::Kind::send)
        {
            auto_state.in_flight = key;
            auto_state.trials.sent(key, now);
            const std::string password = step.password;
            ctx_.store.call<std::vector<FileOp>>(
                Store::lane_files, [key, password](Backend &b) { return b.file_extract_password(key, password); },
                [this, key](Result<std::vector<FileOp>> &&r)
                {
                    AutoExtract &state = auto_extract(auto_extract_state_);
                    if (state.in_flight == key)
                        state.in_flight.clear();
                    if (!r.ok())
                    {
                        const PasswordTrials::Step failed = state.trials.send_failed(key, r.error);
                        state.pending_key = key;
                        state.pending_note = failed.note;
                        return;
                    }
                    take_ops(r.value, "known password");
                });
        }
        else if (step.kind == PasswordTrials::Step::Kind::prompt)
        {
            auto_state.pending_key = key;
            auto_state.pending_note = step.note;
        }
    };
    for (const std::string &key : auto_state.trials.keys())
    {
        // Replies that list only one operation must not end the others: ask the tracker.
        const auto tracked = ops_.active();
        const bool live = std::any_of(tracked.begin(), tracked.end(),
                                      [&](const files::OpsTracker::Tracked *t) { return t->op.id == key; });
        if (!live)
            auto_state.trials.observe(key, PasswordTrials::Seen::ended, {}, now);
    }
    for (const FileOp &op : ops)
    {
        PasswordTrials::Seen seen = PasswordTrials::Seen::active;
        if (files::op_waiting_password(op))
            seen = PasswordTrials::Seen::waiting;
        else if (op.status == "failed" || op.status == "cancelled" || op.status == "canceled")
            seen = PasswordTrials::Seen::ended;
        else if (files::op_terminal(op))
            seen = PasswordTrials::Seen::finished;
        // One call in flight at a time.
        if (seen == PasswordTrials::Seen::waiting && !auto_state.in_flight.empty())
            continue;
        apply(op.id, auto_state.trials.observe(op.id, seen, op.error, now));
    }
    if (!visible_ || modal() || auto_state.pending_key.empty())
        return;
    for (const files::OpsTracker::Tracked *t : ops_.active())
    {
        if (t->op.id == auto_state.pending_key && files::op_waiting_password(t->op))
        {
            auto_state.note = auto_state.pending_note;
            auto_state.pending_key.clear();
            Feedback quiet;
            ask_password(t->op, quiet);
            return;
        }
    }
}

void FilesScreen::ask_password(const FileOp &op, Feedback &feedback)
{
    if (std::find(password_asked_.begin(), password_asked_.end(), op.id) == password_asked_.end())
        password_asked_.push_back(op.id);
    password_op_ = op.id;
    typing_ = Typing::password;
    TextPrompt::Options options;
    options.password = true;
    options.max_length = 200;
    options.placeholder = tr("Password");
    options.done_label = tr("Extract");
    options.note = auto_extract(auto_extract_state_).note;
    auto_extract(auto_extract_state_).note.clear();
    const std::string name = files::base_name(op.source);
    prompt_.open(feedback,
                 op.error.empty() ? tr("Password for {name}", {{"name", name}})
                                  : tr("{name}: {error}. Try again.", {{"name", name}, {"error", op.error}}),
                 "", options);
}

void FilesScreen::post_ops(const char *starting,
                           std::function<Result<std::vector<FileOp>>(Backend &)> work,
                           const std::string &done_toast)
{
    ++posts_;
    starting_ = starting;
    trace(std::string("post: ") + starting);
    ctx_.store.call<std::vector<FileOp>>(
        Store::lane_files, std::move(work),
        [this, done_toast](Result<std::vector<FileOp>> &&r)
        {
            posts_ = std::max(0, posts_ - 1);
            if (!r.ok())
            {
                trace("post failed: " + r.error);
                report(r.error);
                return;
            }
            take_ops(r.value, "post");
            toast_ = done_toast;
        });
}

std::string FilesScreen::strip_text() const
{
    const std::vector<const files::OpsTracker::Tracked *> active = ops_.active();
    if (posts_ > 0)
        return starting_;
    if (ops_.failures() >= 3)
        return tr("Can't read operation status from Pegasus DL ({error})", {{"error", ops_.poll_error()}}) +
               "  \xC2\xB7  " + tr("Options: Operations");
    if (!active.empty())
    {
        const FileOp &op = active.front()->op;
        std::string text;
        std::string hint;
        if (op.cancel_requested)
        {
            text = tr("Cancelling {what}\xE2\x80\xA6", {{"what", files::op_title(op)}});
            if (!op.destination.empty())
                text += " " + tr("(may leave a partial file at {path})", {{"path", op.destination}});
            hint = tr("Options: Details");
        }
        else if (files::op_waiting_password(op))
        {
            text = tr("{what} needs a password", {{"what", files::op_title(op)}});
            hint = tr("Options: Enter it");
        }
        else
        {
            text = files::op_title(op);
            if (!op.destination.empty())
                text += " \xE2\x86\x92 " + op.destination;
            text += "  " + progress_text(*active.front());
            hint = files::op_can_cancel(op) ? tr("Options: Manage / Cancel")
                                            : tr("Options: Details (Pegasus DL cannot stop this one)");
        }
        if (active.size() > 1)
            text += "  " + tr("+{count} more", {{"count", std::to_string(active.size() - 1)}});
        return text + "  \xC2\xB7  " + hint;
    }
    const std::vector<const files::OpsTracker::Tracked *> finished = ops_.finished();
    if (failed_unseen_)
    {
        for (const files::OpsTracker::Tracked *t : finished)
        {
            if (t->op.status == "failed")
                return tr("Failed: {what} ({error})",
                          {{"what", files::op_title(t->op)},
                           {"error", t->op.error.empty() ? std::string(tr("no reason given")) : t->op.error}}) +
                       "  \xC2\xB7  " + tr("Options: Details");
        }
    }
    if (!finished.empty() && ctx_.clock - static_cast<float>(finished.front()->finished_at) < kDoneShown)
    {
        const FileOp &op = finished.front()->op;
        if (op.status == "completed" || op.status == "accepted")
            return tr("Done: {what}", {{"what", files::op_done_title(op)}}) + "  \xC2\xB7  " +
                   tr("Options: History");
        if (op.status == "cancelled")
            return tr("Cancelled: {what}", {{"what", files::op_title(op)}}) + "  \xC2\xB7  " +
                   tr("Options: History");
    }
    if (ops_.failures() > 0)
        return std::string(tr("No file operations running (last check failed, trying again)")) + "  \xC2\xB7  " +
               tr("Options: History");
    return std::string(tr("No file operations running")) + "  \xC2\xB7  " + tr("Options: History");
}

std::vector<const files::OpsTracker::Tracked *> FilesScreen::panel_ops() const
{
    std::vector<const files::OpsTracker::Tracked *> out = ops_.active();
    const std::vector<const files::OpsTracker::Tracked *> done = ops_.finished();
    out.insert(out.end(), done.begin(), done.end());
    return out;
}

void FilesScreen::open_operations_panel()
{
    ops_open_ = true;
    ops_confirm_ = false;
    ops_note_.clear();
    failed_unseen_ = false;
    ops_focus_ = 0; // the newest active one comes first
    if (!ops_inflight_)
        poll_ops();
}

void FilesScreen::update_ops_panel(const InputFrame &input, Feedback &feedback)
{
    const std::vector<const files::OpsTracker::Tracked *> rows = panel_ops();
    const int count = static_cast<int>(rows.size());
    ops_focus_ = std::clamp(ops_focus_, 0, std::max(0, count - 1));
    const auto cancel = [&](const std::string &id)
    {
        ops_confirm_ = false;
        trace("cancel op " + id + " asked");
        ctx_.store.call<bool>(
            Store::lane_files, [id](Backend &b) { return b.file_cancel(id); },
            [this, id](Result<bool> &&r)
            {
                if (!r.ok())
                {
                    trace("cancel op " + id + " error: " + r.error);
                    report(r.error);
                    return;
                }
                trace("cancel op " + id + " accepted by Pegasus DL");
                toast_ = tr("Cancel requested");
                if (!ops_inflight_)
                    poll_ops();
            });
        feedback.play(hui::audio::Cue::select);
    };
    if (ops_confirm_)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = input.nav == Direction::right ? 1 : 0;
            if (next != ops_confirm_button_)
            {
                ops_confirm_button_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        if (input.is_pressed(Action::west) ||
            (input.is_pressed(Action::confirm) && ops_confirm_button_ == 1))
        {
            cancel(ops_confirm_id_);
        }
        else if (input.is_pressed(Action::confirm) || input.is_pressed(Action::back))
        {
            ops_confirm_ = false;
            feedback.play(hui::audio::Cue::modal_close);
        }
        return;
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = ops_focus_ + (input.nav == Direction::down ? 1 : -1);
        if (next >= 0 && next < count)
        {
            ops_focus_ = next;
            ops_note_.clear();
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    const files::OpsTracker::Tracked *t =
        ops_focus_ >= 0 && ops_focus_ < count ? rows[static_cast<std::size_t>(ops_focus_)] : nullptr;
    if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
    {
        ops_open_ = false;
        feedback.play(hui::audio::Cue::modal_close);
    }
    else if (input.is_pressed(Action::west))
    {
        if (t == nullptr)
        {
            refuse(feedback, input);
        }
        else if (t->active() && files::op_can_cancel(t->op) && !t->op.cancel_requested)
        {
            ops_confirm_ = true;
            ops_confirm_button_ = 0; // Keep going: the safe default
            ops_confirm_id_ = t->op.id;
            feedback.play(hui::audio::Cue::modal_open);
        }
        else
        {
            ops_note_ = !t->active()             ? tr("This operation has already finished.")
                        : t->op.cancel_requested ? tr("Already cancelling; Pegasus DL stops it when it can.")
                        : !t->op.cancelable      ? tr("Pegasus DL cannot stop this operation.")
                                                 : tr("Pegasus DL can only stop a running operation.");
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    else if (input.is_pressed(Action::confirm) && t != nullptr && t->active() &&
             files::op_waiting_password(t->op))
    {
        ops_open_ = false;
        ask_password(t->op, feedback);
    }
    else if (input.is_pressed(Action::north) && t != nullptr)
    {
        const std::string where =
            !t->op.destination.empty() ? t->op.destination : files::parent(t->op.source);
        if (where.empty())
        {
            refuse(feedback, input);
        }
        else
        {
            ops_open_ = false;
            area_ = Area::list;
            navigate(where, Nav::walk_up, std::string());
            feedback.play(hui::audio::Cue::select);
        }
    }
}

// ---- actions ----

std::vector<std::string> FilesScreen::targets() const
{
    if (!marks_.empty())
        return marks_;
    const FileItem *item = focused_item();
    if (item != nullptr)
        return {item->path};
    return {};
}

const FileItem *FilesScreen::find_entry(const std::string &path) const
{
    for (const FileItem &e : listed_)
    {
        if (e.path == path)
            return &e;
    }
    return nullptr;
}

std::vector<FileItem> FilesScreen::target_items() const
{
    std::vector<FileItem> out;
    for (const std::string &p : targets())
    {
        const FileItem *e = find_entry(p);
        if (e != nullptr)
            out.push_back(*e);
    }
    return out;
}

const FileItem *FilesScreen::focused_item() const
{
    if (!listed_ok_ || index_ < 0 || index_ >= static_cast<int>(entries_.size()))
        return nullptr;
    return &entries_[static_cast<std::size_t>(index_)];
}

void FilesScreen::mark(const std::string &name)
{
    for (const FileItem &e : entries_)
    {
        if (e.name == name && std::find(marks_.begin(), marks_.end(), e.path) == marks_.end())
            marks_.push_back(e.path);
    }
}

void FilesScreen::open_actions()
{
    subject_ = targets();
    subject_items_ = target_items();
    subject_from_ = path_;
    std::vector<ListItem> items;
    menu_codes_.clear();
    const auto add = [&](const char *code, std::string label, std::string detail, bool enabled = true)
    {
        ListItem item;
        item.label = std::move(label);
        item.detail = std::move(detail);
        item.enabled = enabled;
        items.push_back(std::move(item));
        menu_codes_.push_back(code);
    };
    const std::string read_only = tr("Read-only here: you can copy from this folder, not change it");
    std::string header = tr("This folder");
    double bytes = 0.0;
    bool any_folder = false;
    for (const FileItem &item : subject_items_)
    {
        any_folder = any_folder || item.directory;
        if (!item.directory)
            bytes += item.size_bytes;
    }
    if (!subject_items_.empty())
    {
        bool all_rar = true;
        for (const FileItem &item : subject_items_)
            all_rar = all_rar && wings::logic::isExtractableRarEntry(as_entry(item));
        const bool one = subject_items_.size() == 1;
        const bool one_pkg = one && wings::logic::isPkgEntry(as_entry(subject_items_[0]));
        header = one ? subject_items_[0].name : tr("{count} items", {{"count", std::to_string(subject_items_.size())}});
        if (bytes > 0.0)
            header += "  \xC2\xB7  " + (any_folder && !one ? tr("at least {size}", {{"size", files::format_size(bytes)}})
                                                           : files::format_size(bytes));
        add("move", tr("Move to\xE2\x80\xA6"), writable_ ? std::string(tr("Choose where it goes")) : read_only,
            writable_);
        add("copy", tr("Copy to\xE2\x80\xA6"), tr("Choose where the copy goes"));
        if (one_pkg)
            add("install", tr("Install package"), tr("Hand this .pkg to the PS5 installer"));
        if (all_rar)
            add("extract", tr("Extract to\xE2\x80\xA6"), tr("Choose where the files go"));
        if (one)
            add("rename", tr("Rename\xE2\x80\xA6"), writable_ ? std::string(tr("Give it another name")) : read_only,
                writable_);
        add("delete", tr("Delete\xE2\x80\xA6"),
            writable_ ? std::string(tr("Permanently removes it from this console")) : read_only, writable_);
    }
    add("selectall", tr("Select everything here"), tr("Then choose Copy, Move or Delete"), !entries_.empty());
    add("clearsel", tr("Clear the selection"),
        tr("{count} selected", {{"count", std::to_string(marks_.size())}}), !marks_.empty());
    add("mkdir", tr("New folder\xE2\x80\xA6"),
        writable_ ? path_ : tr("Read-only: no folder can be made here"), writable_ && listed_ok_);
    add("sort", tr("Sort by\xE2\x80\xA6"),
        tr("Now: {key}", {{"key", files::sort_label(sort_)}}) + (sort_descending_ ? " \xE2\x86\x93" : " \xE2\x86\x91"));
    add("filter", tr("Filter\xE2\x80\xA6"),
        filter_.empty() ? std::string(tr("Show only names that contain some text"))
                        : tr("Now: {text}", {{"text", filter_}}));
    add("refresh", tr("Refresh"), tr("Read this folder again"));
    if (subject_items_.size() == 1)
        add("info", tr("Details"), tr("Full path, exact size, date and permissions"));
    list_for_ = ListFor::actions;
    list_.open(header, path_, std::move(items), ListDialog::Mode::single);
}

std::string FilesScreen::default_destination(Transfer kind) const
{
    const std::string &last = last_destination_[static_cast<int>(kind)];
    if (!last.empty())
        return last;
    if (kind == Transfer::extract)
        return path_;
    // Pegasus defaultTargetPath: from an extended drive to the main one, and back.
    if (files::inside(path_, "/data"))
    {
        for (const files::Drive &d : drives_)
        {
            if (d.path != "/data" && d.path != "/" && d.writable)
                return d.path;
        }
        return "/data";
    }
    if (files::inside(path_, "/mnt"))
        return "/data/homebrew"; // falls back to /data when it is missing
    return "/data";
}

void FilesScreen::open_destination(bool move)
{
    if (subject_items_.empty())
    {
        subject_ = targets();
        subject_items_ = target_items();
        subject_from_ = path_;
    }
    transfer_ = move ? Transfer::move : Transfer::copy;
    DestinationBrowser::Request request;
    request.verb = move ? DestinationBrowser::Verb::move : DestinationBrowser::Verb::copy;
    request.items = subject_items_;
    request.from = subject_from_;
    request.start = default_destination(transfer_);
    dest_.open(ctx_.store, std::move(request), locations_, &trace_);
}

void FilesScreen::run_action(const std::string &code, Feedback &feedback)
{
    if (code == "move" || code == "copy" || code == "extract")
    {
        transfer_ = code == "copy" ? Transfer::copy : (code == "move" ? Transfer::move : Transfer::extract);
        DestinationBrowser::Request request;
        request.verb = transfer_ == Transfer::move      ? DestinationBrowser::Verb::move
                       : transfer_ == Transfer::extract ? DestinationBrowser::Verb::extract
                                                        : DestinationBrowser::Verb::copy;
        request.items = subject_items_;
        request.from = subject_from_;
        request.start = default_destination(transfer_);
        dest_.open(ctx_.store, std::move(request), locations_, &trace_);
    }
    else if (code == "install")
    {
        pending_ = Pending::install_pkg;
        confirm_.open(tr("Install this package?"),
                      tr("{path} is handed to the PS5 installer. You finish the install from the "
                         "home screen.",
                         {{"path", subject_.empty() ? std::string() : subject_[0]}}),
                      tr("Install"), tr("Cancel"), false);
    }
    else if (code == "delete")
    {
        pending_ = Pending::delete_items;
        const std::string what =
            subject_.size() == 1 ? subject_[0] : tr("{count} items", {{"count", std::to_string(subject_.size())}});
        confirm_.open(tr("Delete {what}?", {{"what", what}}),
                      tr("They are removed from this console for good, folders with everything "
                         "inside. This cannot be undone."),
                      tr("Delete"), tr("Keep"), true);
    }
    else if (code == "rename")
    {
        typing_ = Typing::rename;
        TextPrompt::Options options;
        options.max_length = 200;
        options.done_label = tr("Rename");
        const std::string current = subject_.empty() ? std::string() : files::base_name(subject_[0]);
        prompt_.open(feedback, tr("Rename"), current, options);
    }
    else if (code == "selectall")
    {
        marks_.clear();
        for (const FileItem &e : entries_)
            marks_.push_back(e.path);
    }
    else if (code == "clearsel")
    {
        marks_.clear();
    }
    else if (code == "mkdir")
    {
        typing_ = Typing::new_folder;
        TextPrompt::Options options;
        options.max_length = 120;
        options.placeholder = tr("Folder name");
        options.done_label = tr("Create");
        prompt_.open(feedback, tr("New folder in {path}", {{"path", path_}}), "", options);
    }
    else if (code == "sort")
    {
        std::vector<ListItem> items;
        for (files::SortKey key : {files::SortKey::name, files::SortKey::date, files::SortKey::size,
                                   files::SortKey::type})
        {
            ListItem item;
            item.label = files::sort_label(key);
            item.right = key == sort_ ? (sort_descending_ ? "\xE2\x86\x93" : "\xE2\x86\x91") : "";
            item.detail = key == sort_ ? std::string(tr("Choose again to reverse")) : std::string();
            items.push_back(std::move(item));
        }
        list_for_ = ListFor::sort;
        list_.open(tr("Sort by"), tr("Folders stay first"), std::move(items), ListDialog::Mode::single);
    }
    else if (code == "filter")
    {
        typing_ = Typing::filter;
        TextPrompt::Options options;
        options.max_length = 80;
        options.allow_empty = true;
        options.placeholder = tr("Part of a name");
        options.done_label = tr("Filter");
        prompt_.open(feedback, tr("Show names that contain"), filter_, options);
    }
    else if (code == "refresh")
    {
        navigate(path_, Nav::refresh, std::string());
    }
    else if (code == "info" && subject_items_.size() == 1)
    {
        const FileItem &e = subject_items_[0];
        std::string text = e.path;
        if (!e.directory)
            text += "\n" + tr("{size} ({bytes} bytes)", {{"size", files::format_size(e.size_bytes)},
                                                        {"bytes", i18n::count(static_cast<long>(e.size_bytes))}});
        if (e.modified > 0)
            text += "\n" + tr("Modified {when}", {{"when", when(e.modified)}});
        text += "\n" + std::string(files::kind_label(files::kind_of(e))) + "  \xC2\xB7  " +
                (e.writable ? std::string(tr("Writable")) : std::string(tr("Read-only")));
        pending_ = Pending::notice;
        confirm_.notice(e.name, text);
    }
}

void FilesScreen::start_transfer(const std::string &collision)
{
    const std::vector<std::string> paths = subject_;
    const std::string dest = destination_;
    const Transfer kind = transfer_;
    trace("transfer " + std::to_string(paths.size()) + " item(s) to " + dest + " collision=" + collision);
    marks_.clear();
    post_ops(kind == Transfer::move      ? tr("Starting the move\xE2\x80\xA6")
             : kind == Transfer::extract ? tr("Starting the extraction\xE2\x80\xA6")
                                         : tr("Starting the copy\xE2\x80\xA6"),
             [paths, dest, collision, kind](Backend &b)
             {
                 if (kind == Transfer::move)
                     return b.file_move(paths, dest, collision);
                 if (kind == Transfer::extract)
                     return b.file_extract(paths, dest, collision);
                 return b.file_copy(paths, dest, collision);
             },
             kind == Transfer::move      ? std::string(tr("Move started"))
             : kind == Transfer::extract ? std::string(tr("Extraction started"))
                                         : std::string(tr("Copy started")));
    subject_items_.clear();
}

// ---- input ----

int FilesScreen::error_buttons() const
{
    int n = 1; // Retry
    if (!files::parent(path_).empty())
        ++n; // Up
    if (path_ != "/data")
        ++n; // Main drive
    return n;
}

void FilesScreen::press_error_button(int button, Feedback &feedback)
{
    feedback.play(hui::audio::Cue::select);
    if (button == 0)
    {
        navigate(path_, Nav::open, std::string());
        return;
    }
    const bool has_up = !files::parent(path_).empty();
    if (button == 1 && has_up)
    {
        navigate(files::parent(path_), Nav::walk_up, files::base_name(path_));
        return;
    }
    navigate("/data", Nav::walk_up, std::string());
}

void FilesScreen::update_list(const InputFrame &input, Feedback &feedback, Intent &intent)
{
    const int count = static_cast<int>(entries_.size());
    const bool failed = !error_.empty();
    const bool waiting = loading_ && count == 0;
    const bool empty = listed_ok_ && count == 0;
    const auto to_crumbs = [&]
    {
        area_ = Area::crumbs;
        crumb_ = static_cast<int>(files::crumbs(path_).size()) - 1;
        feedback.play(hui::audio::Cue::focus);
    };
    if (input.nav == Direction::up)
    {
        if (failed || waiting || empty || index_ == 0)
            to_crumbs();
        else
        {
            --index_;
            feedback.play(hui::audio::Cue::focus);
        }
    }
    else if (input.nav == Direction::down)
    {
        if (!failed && index_ + 1 < count)
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
        // Left and Right only move between the buttons of an error card.
        const int next = error_button_ + (input.nav == Direction::right ? 1 : -1);
        if (failed && next >= 0 && next < error_buttons())
        {
            error_button_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }

    if (input.is_pressed(Action::confirm))
    {
        const FileItem *item = focused_item();
        if (failed)
            press_error_button(error_button_, feedback);
        else if (empty && !filter_.empty())
        {
            set_filter(std::string());
            feedback.play(hui::audio::Cue::select);
        }
        else if (empty && writable_)
            run_action("mkdir", feedback);
        else if (item != nullptr && item->directory)
        {
            navigate(item->path, Nav::open, std::string());
            feedback.play(hui::audio::Cue::select);
        }
        else if (item != nullptr)
        {
            // A file: its own actions, whatever is marked.
            const std::vector<std::string> keep = marks_;
            marks_.clear();
            open_actions();
            marks_ = keep;
            feedback.play(hui::audio::Cue::modal_open);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    else if (input.is_pressed(Action::back))
    {
        // Never a trap: the filter, then the marks, then up; at "/" the tab bar.
        if (!filter_.empty())
        {
            set_filter(std::string());
            feedback.play(hui::audio::Cue::focus);
        }
        else if (!marks_.empty())
        {
            marks_.clear();
            feedback.play(hui::audio::Cue::toggle);
        }
        else
        {
            go_up(feedback, intent);
        }
    }
    else if (input.is_pressed(Action::west))
    {
        // Square: mark or unmark, then on to the next row.
        const FileItem *item = focused_item();
        if (item == nullptr)
        {
            refuse(feedback, input);
        }
        else
        {
            const auto it = std::find(marks_.begin(), marks_.end(), item->path);
            if (it == marks_.end())
                marks_.push_back(item->path);
            else
                marks_.erase(it);
            if (index_ + 1 < count)
                ++index_;
            feedback.play(hui::audio::Cue::toggle);
        }
    }
    else if (input.is_pressed(Action::north))
    {
        open_actions();
        feedback.play(hui::audio::Cue::modal_open);
    }
    else if (input.is_pressed(Action::menu))
    {
        open_operations_panel();
        feedback.play(hui::audio::Cue::modal_open);
    }
    else if (input.is_pressed(Action::touch))
    {
        run_action("filter", feedback);
    }
    else if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next))
    {
        const int step = input.is_pressed(Action::jump_next) ? kPageRows : -kPageRows;
        index_ = std::clamp(index_ + step, 0, std::max(0, count - 1));
        feedback.play(hui::audio::Cue::focus);
    }
    else if (input.is_pressed(Action::r3))
    {
        // Name, Date, Size, Type; the same key again reverses.
        sort_ = static_cast<files::SortKey>((static_cast<int>(sort_) + 1) % 4);
        sort_descending_ = sort_ == files::SortKey::date || sort_ == files::SortKey::size;
        const FileItem *item = focused_item();
        rebuild_shown(item != nullptr ? item->name : std::string());
        feedback.play(hui::audio::Cue::toggle);
    }
}

void FilesScreen::update_header(const InputFrame &input, Feedback &feedback, Intent &intent)
{
    if (area_ == Area::crumbs)
    {
        const std::vector<files::Crumb> parts = files::crumbs(path_);
        const int count = static_cast<int>(parts.size());
        crumb_ = std::clamp(crumb_, 0, count - 1);
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = crumb_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                crumb_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::up)
        {
            area_ = Area::chips;
            chip_ = 0;
            for (std::size_t i = 0; i < drives_.size(); ++i)
            {
                if (drives_[i].path != "/" && files::inside(path_, drives_[i].path))
                    chip_ = static_cast<int>(i);
            }
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.nav == Direction::down)
        {
            area_ = Area::list;
            feedback.play(hui::audio::Cue::focus);
        }
        if (input.is_pressed(Action::confirm))
        {
            area_ = Area::list;
            navigate(parts[static_cast<std::size_t>(crumb_)].path, Nav::open, std::string());
            feedback.play(hui::audio::Cue::select);
        }
        else if (input.is_pressed(Action::back))
        {
            area_ = Area::list;
            feedback.play(hui::audio::Cue::focus);
        }
    }
    else
    {
        if (drives_.empty())
            drives_ = files::drives(locations_);
        const int count = static_cast<int>(drives_.size());
        chip_ = std::clamp(chip_, 0, count - 1);
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = chip_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                chip_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::up)
        {
            intent.kind = Intent::Kind::top_bar;
        }
        else if (input.nav == Direction::down)
        {
            area_ = Area::crumbs;
            crumb_ = static_cast<int>(files::crumbs(path_).size()) - 1;
            feedback.play(hui::audio::Cue::focus);
        }
        if (input.is_pressed(Action::confirm))
        {
            area_ = Area::list;
            navigate(drives_[static_cast<std::size_t>(chip_)].path, Nav::open, std::string());
            feedback.play(hui::audio::Cue::select);
        }
        else if (input.is_pressed(Action::back))
        {
            area_ = Area::list;
            feedback.play(hui::audio::Cue::focus);
        }
    }
    if (input.is_pressed(Action::menu))
    {
        open_operations_panel();
        feedback.play(hui::audio::Cue::modal_open);
    }
}

Intent FilesScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    const bool had_prompt = prompt_.is_open();
    const bool had_confirm = confirm_.is_open();
    const bool had_list = list_.is_open();
    const bool had_dest = dest_.is_open();
    const bool had_ops = ops_open_;
    if (drives_.empty())
        drives_ = files::drives(locations_);

    // Typed values.
    const TextPrompt::Result typed = prompt_.update(had_prompt ? input : kIdle, dt, feedback);
    if (typed == TextPrompt::Result::accepted)
    {
        const std::string text = prompt_.text();
        if (typing_ == Typing::new_folder)
        {
            const std::string parent = path_;
            ctx_.store.call<FileItem>(
                Store::lane_files, [parent, text](Backend &b) { return b.file_mkdir(parent, text); },
                [this, parent, text](Result<FileItem> &&r)
                {
                    if (!r.ok())
                    {
                        trace("mkdir in " + parent + " error: " + r.error);
                        report(r.error);
                        return;
                    }
                    toast_ = tr("Folder created");
                    if (path_ == parent && !loading_)
                        navigate(parent, Nav::refresh, r.value.name.empty() ? text : r.value.name);
                });
        }
        else if (typing_ == Typing::rename && !subject_.empty())
        {
            const std::string path = subject_[0];
            const std::string folder = path_;
            ctx_.store.call<FileItem>(
                Store::lane_files, [path, text](Backend &b) { return b.file_rename(path, text); },
                [this, folder](Result<FileItem> &&r)
                {
                    if (!r.ok())
                    {
                        trace("rename error: " + r.error);
                        report(r.error);
                        return;
                    }
                    marks_.clear();
                    toast_ = tr("Renamed");
                    if (path_ == folder && !loading_)
                        navigate(folder, Nav::refresh, r.value.name);
                });
        }
        else if (typing_ == Typing::password)
        {
            const std::string op = password_op_;
            {
                std::string seen_error;
                for (const files::OpsTracker::Tracked *t : ops_.active())
                    if (t->op.id == op)
                        seen_error = t->op.error;
                auto_extract(auto_extract_state_).trials.set_user_password(op, text, seen_error,
                                                        static_cast<std::int64_t>(ctx_.clock * 1000.0));
            }
            ctx_.store.call<std::vector<FileOp>>(
                Store::lane_files, [op, text](Backend &b) { return b.file_extract_password(op, text); },
                [this, op](Result<std::vector<FileOp>> &&r)
                {
                    // Asked again if Pegasus still waits (a wrong password says so on the op).
                    password_asked_.erase(std::remove(password_asked_.begin(), password_asked_.end(), op),
                                          password_asked_.end());
                    if (!r.ok())
                    {
                        report(r.error);
                        return;
                    }
                    take_ops(r.value, "password");
                });
        }
        else if (typing_ == Typing::filter)
        {
            set_filter(text);
        }
    }
    if (typed != TextPrompt::Result::none)
        typing_ = Typing::none;

    const ConfirmDialog::Result confirmed = confirm_.update(had_confirm ? input : kIdle, dt, feedback);
    if (confirmed == ConfirmDialog::Result::confirmed)
    {
        if (pending_ == Pending::delete_items)
        {
            const std::vector<std::string> paths = subject_;
            marks_.clear();
            post_ops(tr("Starting the delete\xE2\x80\xA6"), [paths](Backend &b) { return b.file_delete(paths); },
                     tr("Delete started"));
        }
        else if (pending_ == Pending::install_pkg && !subject_.empty())
        {
            const std::string path = subject_[0];
            post_ops(tr("Handing the package to the installer\xE2\x80\xA6"),
                     [path](Backend &b) { return b.file_pkg_install(path); }, tr("Install started"));
        }
        else if (pending_ == Pending::replace_ok)
        {
            start_transfer("replace");
        }
    }
    if (confirmed != ConfirmDialog::Result::none)
        pending_ = Pending::none;

    // "Where to?", then (for Replace) one more question.
    const DestinationBrowser::Result picked = dest_.update(ctx_.store, had_dest ? input : kIdle, dt, feedback);
    if (picked == DestinationBrowser::Result::chosen)
    {
        destination_ = dest_.destination();
        last_destination_[static_cast<int>(transfer_)] = destination_;
        if (dest_.collision() == "replace")
        {
            pending_ = Pending::replace_ok;
            confirm_.open(tr("Replace existing items?"),
                          tr("Items in {path} with the same name are overwritten and cannot "
                             "be brought back.",
                             {{"path", destination_}}),
                          tr("Replace"), tr("Cancel"), true);
        }
        else
        {
            start_transfer(dest_.collision());
        }
    }

    const ListDialog::Result listed = list_.update(had_list ? input : kIdle, dt, feedback);
    if (listed == ListDialog::Result::chosen)
    {
        const int chosen = list_.chosen();
        if (list_for_ == ListFor::actions && chosen >= 0 && chosen < static_cast<int>(menu_codes_.size()))
        {
            run_action(menu_codes_[static_cast<std::size_t>(chosen)], feedback);
        }
        else if (list_for_ == ListFor::sort && chosen >= 0 && chosen < 4)
        {
            const files::SortKey key = static_cast<files::SortKey>(chosen);
            sort_descending_ = key == sort_ ? !sort_descending_ : false;
            sort_ = key;
            const FileItem *item = focused_item();
            rebuild_shown(item != nullptr ? item->name : std::string());
        }
    }

    if (had_ops && ops_open_)
        update_ops_panel(focused ? input : kIdle, feedback);

    const bool panel = had_prompt || had_confirm || had_list || had_dest || had_ops;
    const int count = static_cast<int>(entries_.size());
    index_ = std::clamp(index_, 0, std::max(0, count - 1));
    if (focused && !panel && state.online())
    {
        if (area_ == Area::list)
            update_list(input, feedback, intent);
        else
            update_header(input, feedback, intent);
    }
    else if (focused && !state.online() && input.nav == Direction::up)
    {
        intent.kind = Intent::Kind::top_bar;
    }
    if (!toast_.empty() && intent.toast.empty())
        intent.toast = toast_;
    toast_.clear();

    ops_in_.target = ops_open_ ? 1.0f : 0.0f;
    ops_in_.update(dt, 18.0f);
    row_focus_.resize(entries_.size(), 0.0f);
    for (std::size_t i = 0; i < row_focus_.size(); ++i)
    {
        float &value = row_focus_[i];
        const float target =
            focused && !modal() && area_ == Area::list && static_cast<int>(i) == index_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-16.0f * dt));
    }
    scroll_.target = std::max(0.0f, static_cast<float>(index_ + 1) * kPitch - (kListEnd - kListTop));
    scroll_.update(dt, 11.0f);
    return intent;
}

// ---- drawing ----

void FilesScreen::draw_header(hui::gfx::DrawList &list, bool focused) const
{
    const ui::Type &type = ctx_.type;
    // Drive chips.
    const std::vector<files::Drive> &chips = drives_;
    float x = ui::kGutter;
    for (std::size_t i = 0; i < chips.size(); ++i)
    {
        const files::Drive &d = chips[i];
        std::string label = d.label;
        if (d.path != "/" && d.path != "/data")
            label = d.label;
        std::string free;
        if (d.free_bytes >= 0.0)
            free = tr("{free} free", {{"free", files::format_size(d.free_bytes)}});
        const float w = type.medium.measure(label, 21.0f) +
                        (free.empty() ? 0.0f : type.regular.measure(free, 18.0f) + 14.0f) + 44.0f;
        if (x + w > 1920.0f - ui::kGutter)
            break;
        const ui::Rect chip{x, kChipY, w, kChipHeight};
        const bool here = d.path == "/" ? path_ == "/" : files::inside(path_, d.path);
        const bool lit = focused && !modal() && area_ == Area::chips && static_cast<int>(i) == chip_;
        ui::focus_ring(list, chip, kChipHeight * 0.5f, lit ? 1.0f : 0.0f);
        list.bordered_rect(chip, kChipHeight * 0.5f, ui::Color::rgb(0xffffff, here ? 0.2f : 0.07f), 1.0f,
                           ui::Color::rgb(0xffffff, here ? 0.35f : 0.12f));
        const float tx = chip.x + 22.0f;
        const float lw = ui::text(list, type.medium, label, tx, chip.cy() + 7.5f, 21.0f, ui::color::text);
        if (!free.empty())
            ui::text(list, type.regular, free, tx + lw + 14.0f, chip.cy() + 6.5f, 18.0f, ui::color::text3);
        x += w + 14.0f;
    }

    // Breadcrumb, with what the folder holds on the right.
    const std::vector<files::Crumb> parts = files::crumbs(path_);
    x = ui::kGutter;
    const float cy = kCrumbY + kCrumbHeight * 0.5f;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const bool last = i + 1 == parts.size();
        const float w = type.medium.measure(parts[i].label, 22.0f) + 28.0f;
        const ui::Rect r{x, kCrumbY, w, kCrumbHeight};
        const bool lit = focused && !modal() && area_ == Area::crumbs && static_cast<int>(i) == crumb_;
        if (lit)
        {
            ui::focus_ring(list, r, kCrumbHeight * 0.5f, 1.0f);
            list.rounded_rect(r, kCrumbHeight * 0.5f, ui::Color::rgb(0xffffff, 0.14f));
        }
        ui::text(list, type.medium, parts[i].label, x + 14.0f, cy + 8.0f, 22.0f,
                 last ? ui::color::text : ui::color::text3);
        x += w;
        if (!last)
        {
            ui::text(list, type.regular, ">", x + 2.0f, cy + 8.0f, 22.0f, ui::color::text3); // no › in the font
            x += 22.0f;
        }
        if (x > 1100.0f)
            break;
    }
    std::string right;
    if (loading_ && !listed_ok_)
        right = tr("Loading\xE2\x80\xA6");
    else if (loading_)
        right = tr("Refreshing\xE2\x80\xA6");
    if (!marks_.empty())
    {
        double bytes = 0.0;
        for (const std::string &p : marks_)
        {
            const FileItem *e = find_entry(p);
            if (e != nullptr && !e->directory)
                bytes += e->size_bytes;
        }
        std::string sel = tr("{count} selected", {{"count", std::to_string(marks_.size())}});
        if (bytes > 0.0)
            sel += "  \xC2\xB7  " + files::format_size(bytes);
        sel += "  \xC2\xB7  " + std::string(tr("Triangle: Actions  \xC2\xB7  Circle: Clear"));
        right = right.empty() ? sel : right + "  \xC2\xB7  " + sel;
    }
    else if (listed_ok_)
    {
        std::string what;
        if (!filter_.empty())
            what = tr("Filter: {text}", {{"text", filter_}}) + "  \xC2\xB7  " +
                   tr("{shown} of {total}",
                      {{"shown", std::to_string(entries_.size())}, {"total", std::to_string(listed_.size())}});
        else
            what = trn(static_cast<long>(listed_.size()), "{count} item", "{count} items");
        if (!writable_)
            what = std::string(tr("Read-only")) + "  \xC2\xB7  " + what;
        right = right.empty() ? what : right + "  \xC2\xB7  " + what;
    }
    if (!right.empty())
        ui::text_fit(list, type.regular, right, 1920.0f - ui::kGutter, cy + 7.0f, 20.0f,
                     std::max(300.0f, 1920.0f - ui::kGutter - x - 30.0f), ui::color::text2, ui::Align::right);
}

void FilesScreen::draw_list(hui::gfx::DrawList &list, bool focused) const
{
    const ui::Type &type = ctx_.type;
    const float width = 1920.0f - 2.0f * ui::kGutter;
    const bool lit = focused && !modal() && area_ == Area::list;
    if (!error_.empty())
    {
        // The folder asked for could not be read: say so, never show another folder's rows.
        const ui::Rect card{ui::kGutter, kListTop + 10.0f, width, 250.0f};
        ui::surface(list, card, 18.0f, ui::Color::rgb(0xffffff, 0.06f));
        ui::text_fit(list, type.medium, tr("Could not open {path}", {{"path", path_}}), card.x + 40.0f,
                     card.y + 62.0f, 30.0f, card.w - 80.0f, ui::color::text);
        ui::text_fit(list, type.regular, error_, card.x + 40.0f, card.y + 104.0f, 22.0f, card.w - 80.0f,
                     ui::color::tier_red);
        std::vector<std::string> labels = {tr("Retry")};
        if (!files::parent(path_).empty())
            labels.push_back(tr("Up to {path}", {{"path", files::parent(path_)}}));
        if (path_ != "/data")
            labels.push_back(tr("Main drive"));
        float bx = card.x + 40.0f;
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            const float bw = std::max(200.0f, ui::button_width(type, labels[i]) + 20.0f);
            ui::button(list, type, {bx, card.y + 150.0f, bw, ui::kButtonHeight}, labels[i],
                       i == 0 ? ui::ButtonKind::primary : ui::ButtonKind::secondary,
                       lit && static_cast<int>(i) == error_button_ ? 1.0f : 0.0f);
            bx += bw + 20.0f;
        }
        return;
    }
    if (entries_.empty())
    {
        if (loading_)
        {
            // Skeleton rows under the folder being opened.
            for (int i = 0; i < 3; ++i)
            {
                const ui::Rect r{ui::kGutter, kListTop + static_cast<float>(i) * kPitch, width, kRow};
                ui::surface(list, r, 16.0f, ui::Color::rgb(0xffffff, 0.04f));
                list.rounded_rect({r.x + 28.0f, r.cy() - 9.0f, 500.0f - 120.0f * static_cast<float>(i), 18.0f},
                                  9.0f, ui::Color::rgb(0xffffff, 0.08f));
            }
            if (ctx_.clock - load_started_ > kSlowNote)
                ui::text(list, type.regular,
                         ops_.any_active()
                             ? tr("Pegasus DL is slow to answer (a file operation is running). Still waiting\xE2\x80\xA6")
                             : tr("Pegasus DL is slow to answer. Still waiting\xE2\x80\xA6"),
                         ui::kGutter, kListTop + 3.0f * kPitch + 40.0f, 24.0f, ui::color::text2);
            return;
        }
        if (!listed_ok_)
            return;
        const std::string text = !filter_.empty()
                                     ? tr("No names contain \xE2\x80\x9C{text}\xE2\x80\x9D in this folder.",
                                          {{"text", filter_}})
                                     : std::string(tr("This folder is empty."));
        ui::text(list, type.regular, text, ui::kGutter, kListTop + 44.0f, 28.0f, ui::color::text2);
        const char *button = !filter_.empty() ? tr("Clear filter") : (writable_ ? tr("New folder") : nullptr);
        if (button != nullptr)
            ui::button(list, type, {ui::kGutter, kListTop + 80.0f, std::max(220.0f, ui::button_width(type, button) + 20.0f),
                                    ui::kButtonHeight},
                       button, ui::ButtonKind::primary, lit ? 1.0f : 0.0f);
        return;
    }
    list.push_clip({0.0f, kListTop - 16.0f, 1920.0f, kListEnd - kListTop + 24.0f});
    const bool marking = !marks_.empty();
    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        const FileItem &e = entries_[i];
        const float y = kListTop + static_cast<float>(i) * kPitch - scroll_.value;
        if (y + kRow < kListTop - 20.0f || y > kListEnd + 20.0f)
            continue;
        const ui::Rect r{ui::kGutter, y, width, kRow};
        const float f = i < row_focus_.size() && lit ? row_focus_[i] : 0.0f;
        ui::focus_ring(list, r, 16.0f, f);
        ui::surface(list, r, 16.0f, ui::Color::rgb(0xffffff, 0.075f + 0.065f * f));
        float x = r.x + 26.0f;
        const bool marked = std::find(marks_.begin(), marks_.end(), e.path) != marks_.end();
        if (marking)
        {
            const ui::Rect box{x, r.cy() - 14.0f, 28.0f, 28.0f};
            list.bordered_rect(box, 7.0f, marked ? ui::color::action : ui::Color{0, 0, 0, 0}, 2.0f,
                               ui::Color::rgb(0xffffff, 0.55f));
            if (marked)
                ui::icon_check(list, box.cx(), box.cy(), 18.0f, ui::color::night1, 3.0f);
            x += 48.0f;
        }
        const files::Kind kind = files::kind_of(e);
        // On the lit row the fill turns light grey: darken the chip and lift the muted text
        // so the kind and the date stay readable from the couch.
        ui::chip(list, type, x, r.cy(), files::kind_label(kind),
                 hui::gfx::mix(ui::Color::rgb(0xffffff, kind == files::Kind::folder ? 0.16f : 0.08f),
                               ui::Color::rgb(0x000000, 0.42f), f),
                 hui::gfx::mix(ui::color::text2, ui::color::text, f), 16.0f);
        x += 140.0f;
        ui::text_fit(list, type.medium, e.name, x, r.cy() + 9.0f, 26.0f, r.x + r.w - x - 470.0f, ui::color::text);
        if (!e.directory)
            ui::text(list, type.medium, files::format_size(e.size_bytes), r.x + r.w - 250.0f, r.cy() + 8.0f, 22.0f,
                     ui::color::text, ui::Align::right);
        ui::text_fit(list, type.regular, when(e.modified), r.x + r.w - 26.0f, r.cy() + 8.0f, 20.0f, 200.0f,
                     hui::gfx::mix(ui::color::text3, ui::color::text, f), ui::Align::right);
    }
    list.pop_clip();
}

void FilesScreen::draw_strip(hui::gfx::DrawList &list) const
{
    const ui::Type &type = ctx_.type;
    const float width = 1920.0f - 2.0f * ui::kGutter;
    const ui::Rect strip{ui::kGutter, kStripY, width, kStripHeight};
    const std::vector<const files::OpsTracker::Tracked *> active = ops_.active();
    const bool bad = ops_.failures() >= 3 || (failed_unseen_ && active.empty() && posts_ == 0);
    ui::surface(list, strip, 16.0f, ui::Color::rgb(0xffffff, active.empty() ? 0.05f : 0.09f));
    const bool running = !active.empty() || posts_ > 0;
    const float pulse = running ? 0.5f + 0.5f * std::sin(ctx_.clock * 3.0f) : 0.0f;
    list.circle(strip.x + 30.0f, strip.cy(), 7.0f,
                bad ? ui::color::tier_red : ui::Color::rgb(0xffffff, running ? 0.4f + 0.6f * pulse : 0.3f));
    const std::string text = strip_text();
    const std::size_t dot = text.rfind("  \xC2\xB7  ");
    const std::string main = dot == std::string::npos ? text : text.substr(0, dot);
    const std::string hint = dot == std::string::npos ? std::string() : text.substr(dot + 6);
    const float hint_w = hint.empty() ? 0.0f : std::min(560.0f, type.semibold.measure(hint, 19.0f));
    ui::text_fit(list, type.medium, main, strip.x + 56.0f, strip.cy() + (running && !active.empty() && posts_ == 0 ? -3.0f : 7.0f), 21.0f,
                 strip.w - 56.0f - hint_w - 50.0f, bad ? ui::color::tier_red : ui::color::text);
    if (!hint.empty())
        ui::text_fit(list, type.semibold, hint, strip.x + strip.w - 24.0f, strip.cy() + 7.0f, 19.0f, 560.0f,
                     running ? ui::color::text : ui::color::text2, ui::Align::right);
    if (!active.empty() && posts_ == 0)
    {
        const float f = files::op_fraction(active.front()->op);
        const ui::Rect bar{strip.x + 56.0f, strip.cy() + 11.0f, strip.w - 56.0f - hint_w - 50.0f, 6.0f};
        if (f >= 0.0f)
            ui::progress_bar(list, bar, f, ui::color::action);
        else
            ui::progress_bar(list, bar, 0.0f, ui::Color::rgb(0xffffff, 0.3f)); // "Preparing"
    }
}

void FilesScreen::draw_ops_panel(Frame &frame) const
{
    const float a = std::clamp(ops_in_.value, 0.0f, 1.0f);
    if (a < 0.01f)
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx_.type;
    draw_scrim(list, a);
    const ui::Rect r{240.0f, 120.0f + (1.0f - a) * 20.0f, 1440.0f, 820.0f};
    list.push_opacity(a);
    ui::modal(list, r);
    ui::text(list, type.light, tr("Operations"), r.x + 52.0f, r.y + 84.0f, 40.0f, ui::color::text);
    ui::text(list, type.regular, tr("Options or Circle: Close"), r.x + r.w - 52.0f, r.y + 80.0f, 20.0f,
             ui::color::text3, ui::Align::right);
    std::string note = ops_note_;
    if (note.empty() && ops_.failures() > 0)
        note = tr("Last status check failed: {error}", {{"error", ops_.poll_error()}});
    if (!note.empty())
        ui::text_fit(list, type.medium, note, r.x + 52.0f, r.y + 124.0f, 21.0f, r.w - 104.0f,
                     ops_note_.empty() ? ui::color::tier_yellow : ui::color::tier_red);
    const std::vector<const files::OpsTracker::Tracked *> rows = panel_ops();
    const std::size_t active = ops_.active().size();
    const float top = r.y + 176.0f;
    if (rows.empty())
    {
        ui::paragraph(list, type.regular,
                      tr("No file operations since Wings opened. Copies, moves, deletes and extractions "
                         "show up here while they run, and the last ten finished ones stay listed."),
                      r.x + 52.0f, top + 40.0f, 24.0f, r.w - 104.0f, 34.0f, ui::color::text2, 3);
    }
    const float scroll = std::max(0.0f, (static_cast<float>(ops_focus_) + 1.0f - kPanelRows) * kPanelPitch);
    list.push_clip({r.x, top - 4.0f, r.w, kPanelRows * kPanelPitch + 8.0f});
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const files::OpsTracker::Tracked &t = *rows[i];
        const FileOp &op = t.op;
        const float y = top + static_cast<float>(i) * kPanelPitch - scroll;
        if (y + kPanelPitch < top - 8.0f || y > top + kPanelRows * kPanelPitch)
            continue;
        const ui::Rect box{r.x + 36.0f, y, r.w - 72.0f, kPanelPitch - 10.0f};
        const bool lit = static_cast<int>(i) == ops_focus_ && !ops_confirm_;
        ui::focus_ring(list, box, 14.0f, lit ? 1.0f : 0.0f);
        ui::surface(list, box, 14.0f, ui::Color::rgb(0xffffff, t.active() ? 0.09f : 0.04f));
        const float x = box.x + 24.0f;
        const std::string section = i < active ? std::string(tr("Active")) : std::string(tr("Finished"));
        ui::text(list, type.semibold, section, box.x + box.w - 24.0f, box.y + 30.0f, 16.0f,
                 lit ? ui::color::text : ui::color::text3,
                 ui::Align::right);
        const bool done = op.status == "completed" || op.status == "accepted";
        ui::text_fit(list, type.medium, done ? files::op_done_title(op) : files::op_title(op), x, box.y + 34.0f,
                     23.0f, box.w * 0.48f,
                     t.active() ? ui::color::text : ui::color::text2);
        std::string state = op_state(op);
        if (t.active() && !files::op_waiting_password(op) && !op.cancel_requested)
            state += "  \xC2\xB7  " + progress_text(t);
        ui::text_fit(list, type.regular, state, box.x + box.w * 0.52f, box.y + 34.0f, 20.0f, box.w * 0.40f,
                     op.status == "failed" ? ui::color::tier_red : (lit ? ui::color::text : ui::color::text2));
        std::string detail = op.destination.empty() ? op.source : op.source + "  \xE2\x86\x92  " + op.destination;
        if (lit && t.active())
        {
            if (files::op_waiting_password(op))
                detail = tr("Cross: Enter the password  \xC2\xB7  Square: Cancel");
            else if (files::op_can_cancel(op) && !op.cancel_requested)
                detail = tr("Square: Cancel  \xC2\xB7  Triangle: Open the destination");
            else if (!op.cancelable)
                detail = tr("Pegasus DL cannot stop this operation  \xC2\xB7  Triangle: Open the destination");
        }
        else if (lit)
        {
            detail = tr("Triangle: Open the folder");
        }
        // The lit row's detail is the button hint (Square: Cancel ...): full white, never muted.
        ui::text_fit(list, lit ? type.semibold : type.regular, detail, x, box.y + 66.0f, 19.0f, box.w - 48.0f,
                     lit ? ui::color::text : ui::color::text3);
    }
    list.pop_clip();
    if (!rows.empty())
        ui::text(list, type.regular, tr("Finished operations are those Pegasus DL listed since it started (last 10)."),
                 r.x + 52.0f, r.y + r.h - 28.0f, 18.0f, ui::color::text3);

    // Stop it? The safe answer has the focus; Square again stops it.
    if (ops_confirm_)
    {
        const files::OpsTracker::Tracked *t = ops_.find(ops_confirm_id_);
        const ui::Rect c{r.x + 200.0f, r.y + 220.0f, r.w - 400.0f, 380.0f};
        list.rounded_rect({r.x, r.y, r.w, r.h}, ui::kPanelRadius, ui::Color::rgb(0x000000, 0.45f));
        ui::modal(list, c);
        const std::string what = t != nullptr ? files::op_title(t->op) : std::string();
        ui::text_fit(list, type.light, tr("Stop {what}?", {{"what", what}}), c.x + 44.0f, c.y + 76.0f, 34.0f,
                     c.w - 88.0f, ui::color::text);
        std::string body = tr("It stops where it is. Items already copied, moved or deleted stay that way.");
        if (t != nullptr && !t->op.destination.empty())
            body += " " + tr("A partial file may remain at {path}.", {{"path", t->op.destination}});
        ui::paragraph(list, type.regular, body, c.x + 44.0f, c.y + 128.0f, 23.0f, c.w - 88.0f, 32.0f,
                      ui::color::text2, 4);
        const float by = c.y + c.h - 36.0f - ui::kButtonHeight;
        const ui::Rect stop{c.x + c.w - 44.0f - 240.0f, by, 240.0f, ui::kButtonHeight};
        const ui::Rect keep{stop.x - 20.0f - 260.0f, by, 260.0f, ui::kButtonHeight};
        ui::button(list, type, keep, tr("Keep going"), ui::ButtonKind::primary, ops_confirm_button_ == 0 ? 1.0f : 0.0f);
        ui::button(list, type, stop, tr("Stop it"), ui::ButtonKind::danger, ops_confirm_button_ == 1 ? 1.0f : 0.0f);
        ui::text(list, type.regular, tr("Square again: Stop it"), c.x + 44.0f, by + 36.0f, 19.0f, ui::color::text3);
    }
    list.pop_opacity();
}

void FilesScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const Snapshot &state = ctx_.store.state();
    std::string summary = tr("Sorted by {key}", {{"key", files::sort_label(sort_)}});
    summary += sort_descending_ ? " \xE2\x86\x93" : " \xE2\x86\x91";
    draw_heading(ctx_, list, tr("Files"), summary);
    if (!state.online())
    {
        draw_waiting(ctx_, list, 330.0f);
    }
    else
    {
        draw_header(list, focused);
        draw_list(list, focused);
        draw_strip(list);
    }
    dest_.draw(ctx_, frame);
    draw_ops_panel(frame);
    confirm_.draw(ctx_, frame);
    list_.draw(ctx_, frame);
    prompt_.draw(ctx_, frame);
}

std::vector<hui::ui::Hint> FilesScreen::hints() const
{
    using hui::ui::Button;
    if (dest_.is_open())
        return {{Button::cross, tr("Open")},
                {Button::triangle, tr("Choose")},
                {Button::square, tr("If it exists")},
                {Button::circle, tr("Up")}};
    if (ops_open_)
    {
        if (ops_confirm_)
            return {{Button::square, tr("Stop it")}, {Button::cross, tr("Select")}, {Button::circle, tr("Keep going")}};
        return {{Button::square, tr("Cancel it")},
                {Button::triangle, tr("Open folder")},
                {Button::circle, tr("Close")}};
    }
    if (area_ != Area::list)
        return {{Button::cross, tr("Open")},
                {Button::circle, tr("Back")},
                {Button::options, tr("Operations")},
                {Button::l1, tr("Tabs"), Button::r1}};
    std::vector<hui::ui::Hint> items = {{Button::cross, tr("Open")},
                                        {Button::square, tr("Select")},
                                        {Button::triangle, tr("Actions")}};
    items.push_back({Button::circle, !filter_.empty() || !marks_.empty() ? tr("Clear") : tr("Up")});
    items.push_back({Button::options, tr("Operations")});
    if (listed_.size() >= 10)
        items.push_back({Button::touchpad, tr("Filter")});
    items.push_back({Button::l1, tr("Tabs"), Button::r1});
    return items;
}

} // namespace orbit
