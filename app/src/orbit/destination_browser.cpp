// Wings TV app - "Where to?": the destination of a copy, move or extraction.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/destination_browser.hpp"

#include "orbit/i18n.hpp"
#include "orbit/screens.hpp"

#include <algorithm>
#include <cmath>

namespace orbit
{

namespace
{
constexpr float kRowPitch = 70.0f;
constexpr float kRowHeight = 62.0f;
constexpr int kVisibleRows = 8;
const ui::Rect kPanel{80.0f, 112.0f, 1760.0f, 838.0f};
constexpr float kSplit = 620.0f; // the summary column's width

const char *collision_label(const std::string &collision)
{
    if (collision == "replace")
        return tr("Replace");
    if (collision == "skip")
        return tr("Skip");
    return tr("Keep both");
}
} // namespace

void DestinationBrowser::trace(const std::string &line)
{
    if (trace_ != nullptr)
        trace_->add(line);
}

const char *DestinationBrowser::verb_here() const
{
    switch (request_.verb)
    {
    case Verb::move:
        return tr("Move here");
    case Verb::extract:
        return tr("Extract here");
    case Verb::copy:
        break;
    }
    return tr("Copy here");
}

std::string DestinationBrowser::title() const
{
    const long n = static_cast<long>(request_.items.size());
    switch (request_.verb)
    {
    case Verb::move:
        return trn(n, "Move {count} item", "Move {count} items");
    case Verb::extract:
        return trn(n, "Extract {count} archive", "Extract {count} archives");
    case Verb::copy:
        break;
    }
    return trn(n, "Copy {count} item", "Copy {count} items");
}

void DestinationBrowser::open(Store &store, Request request,
                              const std::vector<StorageMount> &locations, files::Trace *trace)
{
    open_ = true;
    request_ = std::move(request);
    locations_ = locations;
    drives_ = files::drives(locations_);
    trace_ = trace;
    collision_ = request_.collision.empty() ? std::string("keep-both") : request_.collision;
    chosen_.clear();
    on_chips_ = false;
    chip_ = 0;
    focus_ = 0;
    in_.snap(0.0f);
    scroll_.snap(0.0f);
    need_bytes_ = 0.0;
    any_folder_ = false;
    for (const FileItem &item : request_.items)
    {
        if (item.directory)
            any_folder_ = true;
        else
            need_bytes_ += item.size_bytes;
    }
    load(store, files::normalize(request_.start.empty() ? "/data" : request_.start), true,
         std::string());
}

void DestinationBrowser::load(Store &store, const std::string &path, bool walk_up,
                              const std::string &focus_name)
{
    path_ = path;
    loading_ = true;
    error_.clear();
    folders_.clear();
    hidden_files_ = 0;
    const std::uint64_t id = ++request_id_;
    request_started_ = now_seconds();
    trace("where-to list #" + std::to_string(id) + " " + path + " asked");
    rebuild_rows(std::string());
    store.call<FolderListing>(
        Store::lane_poll, [path](Backend &b) { return b.file_list(path); },
        [this, &store, id, path, walk_up, focus_name](orbit::Result<FolderListing> &&r)
        {
            const int ms = static_cast<int>((now_seconds() - request_started_) * 1000.0);
            if (id != request_id_ || !open_)
            {
                trace("where-to list #" + std::to_string(id) + " " + path + " dropped-stale");
                return;
            }
            loading_ = false;
            if (!r.ok())
            {
                trace("where-to list #" + std::to_string(id) + " " + path + " error after " +
                      std::to_string(ms) + " ms: " + r.error);
                const std::string up = files::parent(path);
                if (walk_up && !up.empty())
                {
                    // A parent the console will not list: keep going up (Pegasus browseDir).
                    load(store, up, true, files::base_name(path));
                    return;
                }
                error_ = r.error;
                rebuild_rows(std::string());
                return;
            }
            trace("where-to list #" + std::to_string(id) + " " + path + " ok " +
                  std::to_string(r.value.entries.size()) + " entries, " + std::to_string(ms) + " ms");
            path_ = files::normalize(r.value.path.empty() ? path : r.value.path);
            writable_ = r.value.writable;
            for (FileItem &item : r.value.entries)
            {
                if (item.directory)
                    folders_.push_back(std::move(item));
                else
                    ++hidden_files_;
            }
            files::sort_entries(folders_, files::SortKey::name, false);
            rebuild_rows(focus_name);
        });
}

void DestinationBrowser::rebuild_rows(const std::string &focus_name)
{
    rows_.clear();
    const StorageMount *mount = files::mount_of(path_, locations_);
    const bool mount_writable = mount == nullptr || mount->writable;
    const double free_bytes = mount != nullptr && mount->total_bytes > 0.0 ? mount->free_bytes : -1.0;
    std::vector<std::string> sources;
    std::vector<bool> dirs;
    for (const FileItem &item : request_.items)
    {
        sources.push_back(item.path);
        dirs.push_back(item.directory);
    }
    // An archive is usually extracted next to itself: no "already here" for it.
    const bool extract = request_.verb == Verb::extract;
    check_ = files::check_destination(extract ? std::vector<std::string>() : sources,
                                      extract ? std::vector<bool>() : dirs, path_,
                                      request_.verb == Verb::move,
                                      writable_ && mount_writable, free_bytes, need_bytes_);
    Row here;
    here.kind = Kind::here;
    here.path = path_;
    here.label = std::string(verb_here()) + ": " + path_;
    if (loading_)
    {
        here.enabled = false;
        here.detail = tr("Reading this folder\xE2\x80\xA6");
    }
    else if (!error_.empty())
    {
        here.enabled = false;
        here.detail = tr("Could not open this folder: {error}", {{"error", error_}});
    }
    else
    {
        here.enabled = check_.verdict != files::Verdict::refused;
        here.detail = check_.reason;
    }
    rows_.push_back(std::move(here));
    if (!error_.empty())
    {
        Row retry;
        retry.kind = Kind::retry;
        retry.label = tr("Try again");
        retry.path = path_;
        rows_.push_back(std::move(retry));
    }
    if (!loading_ && error_.empty() && writable_ && mount_writable)
    {
        Row mk;
        mk.kind = Kind::new_folder;
        mk.label = tr("New folder\xE2\x80\xA6");
        mk.detail = tr("Create a folder here, then open it");
        rows_.push_back(std::move(mk));
    }
    const std::string up = files::parent(path_);
    if (!up.empty())
    {
        Row row;
        row.kind = Kind::up;
        row.label = tr("Up to {path}", {{"path", up}});
        row.path = up;
        rows_.push_back(std::move(row));
    }
    int focus_row = -1;
    for (const FileItem &item : folders_)
    {
        Row row;
        row.kind = Kind::folder;
        row.label = item.name;
        row.path = item.path.empty() ? files::join(path_, item.name) : item.path;
        // Moving a folder into itself is refused; show it, but say why.
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            if (dirs[i] && files::inside(row.path, sources[i]))
            {
                row.enabled = false;
                row.detail = tr("This is one of the items being sent");
            }
        }
        if (!focus_name.empty() && item.name == focus_name)
            focus_row = static_cast<int>(rows_.size());
        rows_.push_back(std::move(row));
    }
    if (hidden_files_ > 0)
    {
        Row note;
        note.kind = Kind::files_note;
        note.enabled = false;
        note.label = trn(hidden_files_, "\xE2\x80\xA6" "and {count} file here (hidden)",
                         "\xE2\x80\xA6" "and {count} files here (hidden)");
        note.detail = tr("Only folders can be chosen as a destination");
        rows_.push_back(std::move(note));
    }
    if (focus_row >= 0)
        focus_ = focus_row;
    else if (!focus_name.empty() || loading_)
        focus_ = 0;
    focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    row_focus_.assign(rows_.size(), 0.0f);
}

void DestinationBrowser::choose(hui::ui::Feedback &feedback, Result &result)
{
    if (rows_.empty() || !rows_[0].enabled)
    {
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        return;
    }
    chosen_ = path_;
    open_ = false;
    result = Result::chosen;
    trace("where-to chosen " + chosen_ + " collision=" + collision_);
    feedback.play(hui::audio::Cue::select);
}

void DestinationBrowser::go_up(Store &store, hui::ui::Feedback &feedback, Result &result)
{
    const std::string up = files::parent(path_);
    if (up.empty())
    {
        // At the top: leave the destination step; the selection stays.
        open_ = false;
        result = Result::cancelled;
        feedback.play(hui::audio::Cue::modal_close);
        return;
    }
    load(store, up, true, files::base_name(path_));
    feedback.play(hui::audio::Cue::focus);
}

void DestinationBrowser::activate(Store &store, hui::ui::Feedback &feedback, Result &result)
{
    if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size()))
        return;
    const Row row = rows_[static_cast<std::size_t>(focus_)];
    if (!row.enabled)
    {
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        return;
    }
    switch (row.kind)
    {
    case Kind::here:
        choose(feedback, result);
        break;
    case Kind::new_folder:
    {
        TextPrompt::Options options;
        options.max_length = 120;
        options.placeholder = tr("Folder name");
        options.done_label = tr("Create");
        prompt_.open(feedback, tr("New folder in {path}", {{"path", path_}}), "", options);
        break;
    }
    case Kind::up:
        go_up(store, feedback, result);
        break;
    case Kind::retry:
        load(store, row.path, false, std::string());
        feedback.play(hui::audio::Cue::select);
        break;
    case Kind::folder:
        load(store, row.path, false, std::string());
        feedback.play(hui::audio::Cue::select);
        break;
    case Kind::files_note:
        break;
    }
}

DestinationBrowser::Result DestinationBrowser::update(Store &store, const hui::InputFrame &input,
                                                      float dt, hui::ui::Feedback &feedback)
{
    static const hui::InputFrame idle{};
    Result result = Result::none;
    in_.target = open_ ? 1.0f : 0.0f;
    in_.update(dt, 18.0f);
    if (open_)
    {
        const bool typing = prompt_.is_open();
        const TextPrompt::Result typed = prompt_.update(typing ? input : idle, dt, feedback);
        if (typed == TextPrompt::Result::accepted)
        {
            const std::string parent = path_;
            const std::string name = prompt_.text();
            store.call<FileItem>(
                Store::lane_files, [parent, name](Backend &b) { return b.file_mkdir(parent, name); },
                [this, &store, parent, name](orbit::Result<FileItem> &&r)
                {
                    if (!open_)
                        return;
                    if (!r.ok())
                    {
                        error_.clear();
                        check_.reason = r.error;
                        if (!rows_.empty())
                            rows_[0].detail = tr("Could not create the folder: {error}", {{"error", r.error}});
                        return;
                    }
                    // Open the new folder: that is where the items go.
                    load(store, r.value.path.empty() ? files::join(parent, name) : r.value.path, false,
                         std::string());
                });
        }
        if (!typing && typed == TextPrompt::Result::none)
        {
            const int count = static_cast<int>(rows_.size());
            if (on_chips_)
            {
                const int chips = static_cast<int>(drives_.size());
                if (input.nav == Direction::left || input.nav == Direction::right)
                {
                    const int next = chip_ + (input.nav == Direction::right ? 1 : -1);
                    if (next >= 0 && next < chips)
                    {
                        chip_ = next;
                        feedback.play(hui::audio::Cue::focus);
                    }
                    else
                    {
                        refuse(feedback, input);
                    }
                }
                else if (input.nav == Direction::down)
                {
                    on_chips_ = false;
                    feedback.play(hui::audio::Cue::focus);
                }
                else if (input.nav == Direction::up)
                {
                    refuse(feedback, input);
                }
                if (input.is_pressed(Action::confirm) && chip_ >= 0 && chip_ < chips)
                {
                    on_chips_ = false;
                    load(store, drives_[static_cast<std::size_t>(chip_)].path, false, std::string());
                    feedback.play(hui::audio::Cue::select);
                }
                else if (input.is_pressed(Action::back))
                {
                    on_chips_ = false;
                    feedback.play(hui::audio::Cue::focus);
                }
            }
            else
            {
                if (input.nav == Direction::up || input.nav == Direction::down)
                {
                    const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
                    if (next >= 0 && next < count)
                    {
                        focus_ = next;
                        feedback.play(hui::audio::Cue::focus);
                    }
                    else if (next < 0)
                    {
                        // Up from the first row: the drive chips.
                        on_chips_ = true;
                        chip_ = 0;
                        for (std::size_t i = 0; i < drives_.size(); ++i)
                        {
                            if (drives_[i].path != "/" && files::inside(path_, drives_[i].path))
                                chip_ = static_cast<int>(i);
                        }
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
                if (input.is_pressed(Action::confirm))
                    activate(store, feedback, result);
                else if (input.is_pressed(Action::north))
                    choose(feedback, result);
                else if (input.is_pressed(Action::back))
                    go_up(store, feedback, result);
                else if (input.is_pressed(Action::west))
                {
                    collision_ = collision_ == "keep-both" ? "replace"
                                 : collision_ == "replace" ? "skip"
                                                           : "keep-both";
                    feedback.play(hui::audio::Cue::toggle);
                }
                else if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next))
                {
                    const int step = input.is_pressed(Action::jump_next) ? kVisibleRows : -kVisibleRows;
                    focus_ = std::clamp(focus_ + step, 0, std::max(0, count - 1));
                    feedback.play(hui::audio::Cue::focus);
                }
            }
        }
    }
    row_focus_.resize(rows_.size(), 0.0f);
    for (std::size_t i = 0; i < row_focus_.size(); ++i)
    {
        const float target = open_ && !on_chips_ && static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
        row_focus_[i] += (target - row_focus_[i]) * (1.0f - std::exp(-18.0f * dt));
    }
    scroll_.target =
        std::max(0.0f, (static_cast<float>(focus_) + 1.0f - static_cast<float>(kVisibleRows)) * kRowPitch);
    scroll_.update(dt, 14.0f);
    return result;
}

void DestinationBrowser::draw(Context &ctx, Frame &frame) const
{
    if (!open_ && in_.value < 0.01f && !prompt_.is_open())
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx.type;
    const float a = std::clamp(in_.value, 0.0f, 1.0f);
    draw_scrim(list, a);
    const ui::Rect r{kPanel.x, kPanel.y + (1.0f - a) * 20.0f, kPanel.w, kPanel.h};
    list.push_opacity(a);
    ui::modal(list, r);

    // ---- the summary ----
    const float lx = r.x + 52.0f;
    const float lw = kSplit - 90.0f;
    ui::text_fit(list, type.light, title(), lx, r.y + 84.0f, 40.0f, lw, ui::color::text);
    std::string what;
    double bytes = 0.0;
    for (const FileItem &item : request_.items)
    {
        if (!item.directory)
            bytes += item.size_bytes;
    }
    if (request_.items.size() == 1)
        what = request_.items[0].name;
    else
        what = tr("{count} items", {{"count", std::to_string(request_.items.size())}});
    // Source and destination carry matching labels so the two paths are never confused.
    ui::text(list, type.semibold, tr("From"), lx, r.y + 134.0f, 19.0f, ui::color::text3);
    ui::text_fit(list, type.medium, what, lx, r.y + 170.0f, 26.0f, lw, ui::color::text);
    std::string size_line;
    if (bytes > 0.0)
        size_line = (any_folder_ ? tr("at least {size}", {{"size", files::format_size(bytes)}})
                                 : files::format_size(bytes)) +
                    "  ";
    size_line += tr("from {path}", {{"path", request_.from}});
    ui::text_fit(list, type.regular, size_line, lx, r.y + 204.0f, 21.0f, lw, ui::color::text2);

    ui::text(list, type.semibold, tr("Destination"), lx, r.y + 250.0f, 19.0f, ui::color::text3);
    ui::paragraph(list, type.medium, path_, lx, r.y + 284.0f, 25.0f, lw, 32.0f, ui::color::text, 2);
    const StorageMount *mount = files::mount_of(path_, locations_);
    std::string drive = tr("Drive: {name}", {{"name", "/"}});
    for (const files::Drive &d : drives_)
    {
        if (d.path != "/" && files::inside(path_, d.path))
            drive = tr("Drive: {name} ({path})", {{"name", d.label}, {"path", d.path}});
    }
    ui::text_fit(list, type.regular, drive, lx, r.y + 362.0f, 21.0f, lw, ui::color::text2);
    if (mount != nullptr && mount->total_bytes > 0.0)
    {
        std::string free_line = tr("Free: {free}", {{"free", files::format_size(mount->free_bytes)}});
        if (need_bytes_ > 0.0)
            free_line += "  \xC2\xB7  " + tr(any_folder_ ? "needs at least {size}" : "needs {size}",
                                             {{"size", files::format_size(need_bytes_)}});
        ui::text_fit(list, type.regular, free_line, lx, r.y + 396.0f, 21.0f, lw, ui::color::text2);
    }
    if (!check_.reason.empty() && !loading_ && error_.empty())
        ui::paragraph(list, type.medium, check_.reason, lx, r.y + 446.0f, 21.0f, lw, 28.0f,
                      check_.verdict == files::Verdict::refused ? ui::color::tier_red
                                                                : ui::color::tier_yellow,
                      2);
    if (request_.verb == Verb::move)
    {
        const StorageMount *from = files::mount_of(request_.from, locations_);
        if (from != nullptr && mount != nullptr && from->path != mount->path)
            ui::paragraph(list, type.regular,
                          tr("Different drive: the data is copied, then the source is deleted. "
                             "This takes a long time for large games."),
                          lx, r.y + 530.0f, 20.0f, lw, 27.0f, ui::color::text3, 3);
    }
    // What happens to items that already exist: Square changes it.
    ui::text(list, type.semibold, tr("If an item already exists"), lx, r.y + r.h - 150.0f, 19.0f,
             ui::color::text3);
    const ui::Rect field{lx, r.y + r.h - 130.0f, lw, ui::kFieldHeight};
    ui::select_field(list, type, field, collision_label(collision_), 0.0f);
    if (collision_ == "replace")
        ui::text_fit(list, type.regular, tr("Existing items are overwritten  \xC2\xB7  Square changes it"), lx,
                     r.y + r.h - 40.0f, 18.0f, lw, ui::color::tier_yellow);
    else
        ui::text(list, type.regular, tr("Square changes it"), lx, r.y + r.h - 40.0f, 18.0f, ui::color::text2);
    list.rounded_rect({r.x + kSplit, r.y + 36.0f, 1.0f, r.h - 72.0f}, 0.0f, ui::color::line);

    // ---- the browser ----
    const float bx = r.x + kSplit + 40.0f;
    const float bw = r.x + r.w - 40.0f - bx;
    ui::text(list, type.light, tr("Where to?"), bx, r.y + 74.0f, 34.0f, ui::color::text);
    if (loading_)
    {
        const std::string slow = now_seconds() - request_started_ > 4.0
                                     ? std::string(tr("Pegasus DL is slow to answer. Still waiting\xE2\x80\xA6"))
                                     : std::string(tr("Loading\xE2\x80\xA6"));
        ui::text_fit(list, type.regular, slow, bx + bw, r.y + 72.0f, 20.0f, 600.0f, ui::color::text3,
                     ui::Align::right);
    }
    // Drive chips.
    float cx = bx;
    const float chip_y = r.y + 100.0f;
    for (std::size_t i = 0; i < drives_.size(); ++i)
    {
        const files::Drive &d = drives_[i];
        std::string label = d.label;
        if (d.free_bytes >= 0.0)
            label += "  " + tr("{free} free", {{"free", files::format_size(d.free_bytes)}});
        const float w = type.medium.measure(label, 19.0f) + 40.0f;
        if (cx + w > bx + bw)
            break;
        const ui::Rect chip{cx, chip_y, w, 46.0f};
        const bool focused = on_chips_ && static_cast<int>(i) == chip_;
        const bool current = d.path != "/" ? files::inside(path_, d.path) : path_ == "/";
        ui::focus_ring(list, chip, 23.0f, focused ? 1.0f : 0.0f);
        list.rounded_rect(chip, 23.0f, ui::Color::rgb(0xffffff, current ? 0.2f : 0.08f));
        ui::text(list, type.medium, label, chip.x + 20.0f, chip.cy() + 7.0f, 19.0f, ui::color::text);
        cx += w + 12.0f;
    }
    ui::text_fit(list, type.regular, path_, bx, r.y + 186.0f, 22.0f, bw, ui::color::text2);
    const float top = r.y + 210.0f;
    const ui::Rect body{bx, top, bw, static_cast<float>(kVisibleRows) * kRowPitch};
    list.push_clip({body.x - 8.0f, body.y - 6.0f, body.w + 16.0f, body.h + 12.0f});
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const Row &row = rows_[i];
        const float y = top + static_cast<float>(i) * kRowPitch - scroll_.value;
        if (y + kRowHeight < body.y - 8.0f || y > body.y + body.h + 8.0f)
            continue;
        const ui::Rect box{bx, y, bw, kRowHeight};
        const float f = i < row_focus_.size() ? row_focus_[i] : 0.0f;
        ui::focus_ring(list, box, 14.0f, f);
        const bool pinned = row.kind == Kind::here;
        ui::surface(list, box, 14.0f,
                    pinned && row.enabled ? ui::Color::rgb(0xffffff, 0.16f + 0.08f * f)
                                          : ui::Color::rgb(0xffffff, 0.06f + 0.07f * f));
        const ui::Color ink = row.enabled ? ui::color::text : ui::color::text3;
        float x = box.x + 22.0f;
        if (row.kind == Kind::folder)
        {
            ui::chip(list, type, x, box.cy(), tr("Folder"),
                     hui::gfx::mix(ui::Color::rgb(0xffffff, 0.14f), ui::Color::rgb(0x000000, 0.42f), f),
                     hui::gfx::mix(ui::color::text2, ui::color::text, f), 15.0f);
            x += 92.0f;
        }
        const auto &face = pinned || row.kind == Kind::new_folder || row.kind == Kind::up ||
                                   row.kind == Kind::retry
                               ? type.semibold
                               : type.medium;
        const float tw = box.x + box.w - 24.0f - x - (pinned ? 150.0f : 0.0f);
        if (row.detail.empty())
        {
            ui::text_fit(list, face, row.label, x, box.cy() + 8.0f, 23.0f, tw, ink);
        }
        else
        {
            ui::text_fit(list, face, row.label, x, box.cy() - 3.0f, 23.0f, tw, ink);
            ui::text_fit(list, type.regular, row.detail, x, box.cy() + 21.0f, 17.0f, tw,
                         !row.enabled && pinned ? ui::color::tier_red
                                                   : hui::gfx::mix(ui::color::text3, ui::color::text2, f));
        }
        if (pinned)
            ui::text(list, type.semibold, tr("Triangle"), box.x + box.w - 24.0f, box.cy() + 7.0f, 18.0f,
                     hui::gfx::mix(ui::color::text3, ui::color::text, f), ui::Align::right);
    }
    list.pop_clip();
    list.pop_opacity();
    prompt_.draw(ctx, frame);
}

} // namespace orbit
