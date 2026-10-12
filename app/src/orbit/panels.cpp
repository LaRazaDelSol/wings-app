// Wings TV app - Panels screens open on top of themselves.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/panels.hpp"

#include "orbit/i18n.hpp"
#include "orbit/screens.hpp"

#include <algorithm>
#include <cmath>

namespace orbit
{

namespace
{
constexpr float kScrimAlpha = 0.62f;
constexpr float kButtonWidth = 230.0f;
} // namespace

void draw_scrim(hui::gfx::DrawList &list, float amount)
{
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f,
                      ui::Color::rgb(0x000000, kScrimAlpha * std::clamp(amount, 0.0f, 1.0f)));
}

// ---- TextPrompt ----

void TextPrompt::open(hui::ui::Feedback &feedback, const std::string &title,
                      const std::string &initial, const Options &options)
{
    if (!themed_)
    {
        prompt_.style.theme = keyboard_theme();
        prompt_.keyboard.style.space_label = tr("Space");
        prompt_.keyboard.style.done_label = tr("Done");
        prompt_.style.cancel_label = tr("Cancel");
        prompt_.style.width = 1100.0f;
        prompt_.style.auto_capital = false;
        themed_ = true;
    }
    // Every prompt has the same editing keys (the search box's set): R2 / Options
    // submit, L1 / R1 move the caret, Square deletes, L2 shifts, R3 switches to
    // symbols, L3 and the Clear button empty the text, a hint under the keys for
    // each. A secret (password, token) is masked and Triangle shows / hides it,
    // so it gives up Triangle's space (the Space key is still on the board).
    // Set on every call: nothing leaks from one prompt to the next.
    using hui::Action;
    const bool secret = options.password;
    prompt_.keyboard.style.bindings = hui::ui::KeyboardBindings::prompt(secret);
    prompt_.style.caret_keys = true;
    prompt_.style.submit_on_menu = true;
    prompt_.style.clear_button = true;
    prompt_.style.reveal_key = secret ? Action::north : Action::count;
    prompt_.style.clear_label = tr("Clear");
    prompt_.style.hints = {
        {{Action::page_prev, Action::page_next}, tr("Move cursor")},
        {{Action::west}, tr("Delete")},
        {{Action::north}, secret ? tr("Show") : tr("Space")},
        {{Action::jump_prev}, tr("Shift")},
        {{Action::r3}, tr("Symbols")},
        {{Action::l3}, tr("Clear")},
        {{Action::jump_next, Action::menu},
         options.search ? tr("Search")
                        : (options.done_label.empty() ? tr("Done") : options.done_label)},
        {{Action::back}, tr("Cancel")},
    };
    prompt_.field.set_helper(options.note);
    prompt_.style.chips_label = tr("Recent searches");
    prompt_.set_chips(options.chips);
    prompt_.style.max_length = options.max_length;
    prompt_.style.password = options.password;
    prompt_.style.allow_empty = options.allow_empty;
    prompt_.style.empty_error = tr("Type something first");
    prompt_.style.done_label = options.done_label.empty() ? std::string(tr("Done")) : options.done_label;
    prompt_.keyboard.style.done_label = prompt_.style.done_label;
    prompt_.field.set_placeholder(options.placeholder);
    prompt_.set_title(title);
    prompt_.open(feedback, initial);
}

TextPrompt::Result TextPrompt::update(const hui::InputFrame &input, float dt,
                                      hui::ui::Feedback &feedback)
{
    Result result = Result::none;
    if (prompt_.is_open())
    {
        const hui::ui::Event event = prompt_.handle(input, feedback);
        if (event == hui::ui::Event::activated)
            result = Result::accepted;
        else if (event == hui::ui::Event::cancelled)
            result = Result::cancelled;
    }
    prompt_.update(dt);
    return result;
}

void TextPrompt::draw(Context &ctx, Frame &frame) const
{
    if (!prompt_.visible())
        return;
    frame.glass = true;
    hui::ui::Canvas canvas{frame.overlay, ctx.fonts, frame.glass_texture, ctx.clock};
    prompt_.draw(canvas);
}

// ---- ConfirmDialog ----

void ConfirmDialog::open(std::string title, std::string message, std::string confirm_label,
                         std::string cancel_label, bool destructive)
{
    open_ = true;
    single_ = false;
    destructive_ = destructive;
    focus_ = destructive ? 0 : 1;
    title_ = std::move(title);
    message_ = std::move(message);
    confirm_ = std::move(confirm_label);
    cancel_ = std::move(cancel_label);
    if (cancel_.empty())
        cancel_ = tr("Cancel");
    in_.snap(0.0f);
}

void ConfirmDialog::notice(std::string title, std::string message, std::string ok_label)
{
    open(std::move(title), std::move(message), ok_label.empty() ? std::string(tr("OK")) : ok_label,
         {}, false);
    single_ = true;
    focus_ = 1;
}

ConfirmDialog::Result ConfirmDialog::update(const hui::InputFrame &input, float dt,
                                            hui::ui::Feedback &feedback)
{
    Result result = Result::none;
    in_.target = open_ ? 1.0f : 0.0f;
    in_.update(dt, 18.0f);
    if (open_)
    {
        if (!single_ && (input.nav == Direction::left || input.nav == Direction::right))
        {
            const int next = input.nav == Direction::right ? 1 : 0;
            if (next != focus_)
            {
                focus_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            open_ = false;
            feedback.play(hui::audio::Cue::select);
            result = (single_ || focus_ == 1) ? Result::confirmed : Result::cancelled;
            if (single_)
                result = Result::confirmed;
        }
        else if (input.is_pressed(Action::back))
        {
            open_ = false;
            feedback.play(hui::audio::Cue::modal_close);
            result = single_ ? Result::confirmed : Result::cancelled;
        }
    }
    for (int i = 0; i < 2; ++i)
    {
        float &value = button_focus_[static_cast<std::size_t>(i)];
        const float target = open_ && i == focus_ ? 1.0f : 0.0f;
        value += (target - value) * (1.0f - std::exp(-18.0f * dt));
    }
    return result;
}

void ConfirmDialog::draw(Context &ctx, Frame &frame) const
{
    if (!open_ && in_.value < 0.01f)
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx.type;
    const float a = std::clamp(in_.value, 0.0f, 1.0f);
    draw_scrim(list, a);
    const float width = 900.0f;
    // Measure the message to size the panel.
    const float message_lines = 5.0f;
    const float height = 72.0f + 50.0f + message_lines * 36.0f + 40.0f + ui::kButtonHeight + 44.0f;
    const ui::Rect r{960.0f - width * 0.5f, 540.0f - height * 0.5f + (1.0f - a) * 20.0f, width, height};
    list.push_opacity(a);
    ui::modal(list, r);
    ui::text_fit(list, type.light, title_, r.x + 48.0f, r.y + 82.0f, 38.0f, r.w - 96.0f,
                 ui::color::text);
    ui::paragraph(list, type.regular, message_, r.x + 48.0f, r.y + 140.0f, 25.0f, r.w - 96.0f,
                  36.0f, ui::color::text2, 5);
    const float y = r.y + r.h - 44.0f - ui::kButtonHeight;
    const ui::Rect confirm{r.x + r.w - 48.0f - kButtonWidth, y, kButtonWidth, ui::kButtonHeight};
    const ui::Rect cancel{confirm.x - 20.0f - kButtonWidth, y, kButtonWidth, ui::kButtonHeight};
    ui::button(list, type, confirm, confirm_,
               destructive_ ? ui::ButtonKind::danger : ui::ButtonKind::primary, button_focus_[1]);
    if (!single_)
        ui::button(list, type, cancel, cancel_, ui::ButtonKind::secondary, button_focus_[0]);
    list.pop_opacity();
}

// ---- ListDialog ----

void ListDialog::open(std::string title, std::string subtitle, std::vector<ListItem> items,
                      Mode mode, std::string confirm_label)
{
    open_ = true;
    mode_ = mode;
    title_ = std::move(title);
    subtitle_ = std::move(subtitle);
    items_ = std::move(items);
    confirm_ = std::move(confirm_label);
    focus_ = 0;
    chosen_ = -1;
    in_.snap(0.0f);
    scroll_.snap(0.0f);
    row_focus_.assign(items_.size() + 1, 0.0f);
}

std::vector<int> ListDialog::checked() const
{
    std::vector<int> out;
    for (std::size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].checked)
            out.push_back(static_cast<int>(i));
    }
    return out;
}

namespace
{
constexpr float kDialogRow = 68.0f;
constexpr int kVisibleRows = 6;
} // namespace

ListDialog::Result ListDialog::update(const hui::InputFrame &input, float dt,
                                      hui::ui::Feedback &feedback)
{
    Result result = Result::none;
    in_.target = open_ ? 1.0f : 0.0f;
    in_.update(dt, 18.0f);
    const int count = static_cast<int>(items_.size());
    const int last = mode_ == Mode::multi ? count : count - 1;
    if (open_)
    {
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next <= last)
            {
                focus_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            if (mode_ == Mode::single)
            {
                if (items_[static_cast<std::size_t>(focus_)].enabled)
                {
                    chosen_ = focus_;
                    open_ = false;
                    feedback.play(hui::audio::Cue::select);
                    result = Result::chosen;
                }
                else
                {
                    feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
                }
            }
            else if (focus_ == count)
            {
                if (!checked().empty())
                {
                    open_ = false;
                    feedback.play(hui::audio::Cue::select);
                    result = Result::confirmed;
                }
                else
                {
                    feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
                }
            }
            else
            {
                ListItem &item = items_[static_cast<std::size_t>(focus_)];
                if (item.enabled)
                {
                    item.checked = !item.checked;
                    feedback.play(hui::audio::Cue::toggle);
                }
            }
        }
        else if (input.is_pressed(Action::back))
        {
            open_ = false;
            feedback.play(hui::audio::Cue::modal_close);
            result = Result::cancelled;
        }
    }
    row_focus_.resize(items_.size() + 1, 0.0f);
    for (std::size_t i = 0; i < row_focus_.size(); ++i)
    {
        const float target = open_ && static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
        row_focus_[i] += (target - row_focus_[i]) * (1.0f - std::exp(-18.0f * dt));
    }
    scroll_.target = std::max(
        0.0f, (static_cast<float>(std::min(focus_, count - 1)) + 1.0f - kVisibleRows) * kDialogRow);
    scroll_.update(dt, 14.0f);
    return result;
}

void ListDialog::draw(Context &ctx, Frame &frame) const
{
    if (!open_ && in_.value < 0.01f)
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx.type;
    const float a = std::clamp(in_.value, 0.0f, 1.0f);
    draw_scrim(list, a);
    const int count = static_cast<int>(items_.size());
    const int rows = std::min(count, kVisibleRows);
    const bool multi = mode_ == Mode::multi;
    const float width = 960.0f;
    const float header = subtitle_.empty() ? 120.0f : 160.0f;
    const float footer = multi ? ui::kButtonHeight + 60.0f : 40.0f;
    const float list_height = static_cast<float>(std::max(rows, 1)) * kDialogRow;
    const float height = header + list_height + footer;
    const ui::Rect r{960.0f - width * 0.5f, 540.0f - height * 0.5f + (1.0f - a) * 20.0f, width,
                     height};
    list.push_opacity(a);
    ui::modal(list, r);
    ui::text_fit(list, type.light, title_, r.x + 48.0f, r.y + 82.0f, 38.0f, r.w - 96.0f,
                 ui::color::text);
    if (!subtitle_.empty())
        ui::text_fit(list, type.regular, subtitle_, r.x + 48.0f, r.y + 124.0f, 22.0f, r.w - 96.0f,
                     ui::color::text3);
    const ui::Rect body{r.x, r.y + header, r.w, list_height};
    list.push_clip({body.x, body.y - 4.0f, body.w, body.h + 8.0f});
    for (int i = 0; i < count; ++i)
    {
        const ListItem &item = items_[static_cast<std::size_t>(i)];
        const float y = body.y + static_cast<float>(i) * kDialogRow - scroll_.value;
        if (y + kDialogRow < body.y - 8.0f || y > body.y + body.h + 8.0f)
            continue;
        const ui::Rect row{r.x + 32.0f, y + 4.0f, r.w - 64.0f, kDialogRow - 8.0f};
        const float f = row_focus_.size() > static_cast<std::size_t>(i)
                            ? row_focus_[static_cast<std::size_t>(i)]
                            : 0.0f;
        ui::focus_ring(list, row, 14.0f, f);
        ui::surface(list, row, 14.0f, ui::Color::rgb(0xffffff, 0.07f + 0.07f * f));
        float x = row.x + 24.0f;
        if (multi)
        {
            const ui::Rect box{x, row.cy() - 13.0f, 26.0f, 26.0f};
            list.bordered_rect(box, 7.0f,
                               item.checked ? ui::color::action : ui::Color{0, 0, 0, 0}, 2.0f,
                               ui::Color::rgb(0xffffff, 0.6f));
            if (item.checked)
                ui::icon_check(list, box.cx(), box.cy(), 18.0f, ui::color::night1, 3.0f);
            x += 46.0f;
        }
        const ui::Color ink = item.enabled ? ui::color::text : ui::color::text3;
        const float right_w = item.right.empty() ? 0.0f : 220.0f;
        if (item.detail.empty())
        {
            ui::text_fit(list, type.medium, item.label, x, row.cy() + 8.0f, 24.0f,
                         row.x + row.w - right_w - x - 24.0f, ink);
        }
        else
        {
            ui::text_fit(list, type.medium, item.label, x, row.cy() - 2.0f, 24.0f,
                         row.x + row.w - right_w - x - 24.0f, ink);
            // The lit row is light grey: the muted detail lifts to stay readable on it.
            ui::text_fit(list, type.regular, item.detail, x, row.cy() + 22.0f, 18.0f,
                         row.x + row.w - right_w - x - 24.0f, hui::gfx::mix(ui::color::text3, ui::color::text, f));
        }
        if (!item.right.empty())
            ui::text_fit(list, type.regular, item.right, row.x + row.w - 24.0f, row.cy() + 7.0f,
                         20.0f, right_w, hui::gfx::mix(ui::color::text2, ui::color::text, f), ui::Align::right);
    }
    list.pop_clip();
    if (multi)
    {
        const std::size_t ticked = checked().size();
        std::string label = confirm_.empty() ? std::string(tr("Confirm")) : confirm_;
        if (ticked > 0)
            label += "  (" + std::to_string(ticked) + ")";
        const float w = std::max(260.0f, ui::button_width(type, label) + 20.0f);
        const ui::Rect button{r.x + r.w - 48.0f - w, r.y + r.h - 30.0f - ui::kButtonHeight, w,
                              ui::kButtonHeight};
        const float f = row_focus_.size() > static_cast<std::size_t>(count)
                            ? row_focus_[static_cast<std::size_t>(count)]
                            : 0.0f;
        ui::button(list, type, button, label, ui::ButtonKind::primary, f, ticked > 0);
    }
    list.pop_opacity();
}


// ---- FolderPicker ----

namespace
{
constexpr int kPickerRows = 7;
constexpr float kPickerRow = 68.0f;

std::string join_folder(const std::string &base, const std::string &name)
{
    if (base.empty() || base == "/")
        return "/" + name;
    return base + "/" + name;
}

std::string parent_folder(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return std::string();
    return path.substr(0, slash);
}
} // namespace

void FolderPicker::open(Store &store, std::string title, const std::string &start)
{
    open_ = true;
    title_ = std::move(title);
    chosen_.clear();
    error_.clear();
    folders_.clear();
    rows_.clear();
    path_.clear();
    focus_ = 0;
    in_.snap(0.0f);
    scroll_.snap(0.0f);
    loading_ = true;
    const std::uint64_t id = ++request_;
    // Storage locations first (they decide what is writable), then the start folder.
    store.call<std::vector<StorageMount>>(
        Store::lane_poll, [](Backend &b) { return b.mounts(); },
        [this, &store, id, start](orbit::Result<std::vector<StorageMount>> &&r)
        {
            if (id != request_ || !open_)
                return;
            if (r.ok())
                mounts_ = std::move(r.value);
            else
                error_ = r.error;
            if (start.empty() || !r.ok())
            {
                loading_ = false;
                path_.clear();
                rebuild_rows();
            }
            else
            {
                load(store, start);
            }
        });
}

void FolderPicker::load_mounts(Store &)
{
    ++request_;
    path_.clear();
    folders_.clear();
    loading_ = false;
    focus_ = 0;
    rebuild_rows();
}

void FolderPicker::load(Store &store, const std::string &path, bool walk_up)
{
    loading_ = true;
    error_.clear();
    const std::uint64_t id = ++request_;
    store.call<FolderListing>(
        Store::lane_poll, [path](Backend &b) { return b.browse(path); },
        [this, &store, id, path, walk_up](orbit::Result<FolderListing> &&r)
        {
            if (id != request_ || !open_)
                return;
            loading_ = false;
            if (!r.ok() && walk_up)
            {
                // Going up into a folder that cannot be listed: keep going up.
                const std::string up = parent_folder(path);
                if (up.empty())
                    load_mounts(store);
                else
                    load(store, up, true);
                error_ = tr("Could not open {path}: {error}", {{"path", path}, {"error", r.error}});
                return;
            }
            if (!r.ok())
            {
                // A folder that cannot be listed: stay where we were.
                error_ = r.error;
                if (rows_.empty())
                    rebuild_rows();
                return;
            }
            path_ = r.value.path.empty() ? path : r.value.path;
            folders_.clear();
            for (FileItem &item : r.value.entries)
            {
                if (item.directory)
                    folders_.push_back(std::move(item));
            }
            std::sort(folders_.begin(), folders_.end(),
                      [](const FileItem &a, const FileItem &b) { return a.name < b.name; });
            focus_ = 0;
            scroll_.snap(0.0f);
            rebuild_rows();
        });
}

bool FolderPicker::writable_here() const
{
    if (path_.empty())
        return false;
    const StorageMount *best = nullptr;
    for (const StorageMount &m : mounts_)
    {
        const bool inside = path_ == m.path || m.path == "/" || path_.rfind(m.path + "/", 0) == 0;
        if (inside && (best == nullptr || m.path.size() > best->path.size()))
            best = &m;
    }
    return best == nullptr ? true : best->writable;
}

void FolderPicker::rebuild_rows()
{
    rows_.clear();
    if (path_.empty())
    {
        for (const StorageMount &m : mounts_)
        {
            Row row;
            row.kind = Kind::mount;
            row.label = m.name.empty() ? m.path : m.name;
            row.path = m.path;
            row.detail = m.path;
            if (m.total_bytes > 0.0)
                row.detail += "  \xC2\xB7  " + tr("{free} free of {total}",
                                                  {{"free", format_bytes(m.free_bytes)},
                                                   {"total", format_bytes(m.total_bytes)}});
            if (!m.writable)
                row.detail += "  \xC2\xB7  " + std::string(tr("read-only"));
            rows_.push_back(std::move(row));
        }
    }
    else
    {
        const bool writable = writable_here();
        Row use;
        use.kind = Kind::use_here;
        use.label = tr("Use this folder");
        use.detail =
            writable ? path_ : std::string(tr("This location is read-only. Open a folder on writable storage."));
        use.enabled = writable;
        rows_.push_back(std::move(use));
        if (writable)
        {
            Row mk;
            mk.kind = Kind::new_folder;
            mk.label = tr("New folder\xE2\x80\xA6");
            mk.detail = tr("Create a folder here");
            rows_.push_back(std::move(mk));
        }
        Row up;
        up.kind = Kind::parent;
        up.label = tr("Up one level");
        up.detail = parent_folder(path_).empty() ? std::string(tr("Storage locations"))
                                                 : parent_folder(path_);
        rows_.push_back(std::move(up));
        for (const FileItem &item : folders_)
        {
            Row row;
            row.kind = Kind::folder;
            row.label = item.name;
            row.path = item.path.empty() ? join_folder(path_, item.name) : item.path;
            rows_.push_back(std::move(row));
        }
    }
    focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    row_focus_.assign(rows_.size(), 0.0f);
}

void FolderPicker::activate(Store &store, int index, hui::ui::Feedback &feedback)
{
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return;
    const Row row = rows_[static_cast<std::size_t>(index)];
    if (!row.enabled)
    {
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        return;
    }
    feedback.play(hui::audio::Cue::select);
    switch (row.kind)
    {
    case Kind::use_here:
        chosen_ = path_;
        result_choose_ = true;
        open_ = false;
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
    case Kind::parent:
    {
        const std::string up = parent_folder(path_);
        if (up.empty())
            load_mounts(store);
        else
            load(store, up, true);
        break;
    }
    case Kind::mount:
    case Kind::folder:
        load(store, row.path);
        break;
    }
}

FolderPicker::Result FolderPicker::update(Store &store, const hui::InputFrame &input, float dt,
                                          hui::ui::Feedback &feedback)
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
            const std::string full = join_folder(path_, prompt_.text());
            const std::string here = path_;
            const std::uint64_t id = ++request_;
            loading_ = true;
            store.call<bool>(
                Store::lane_poll, [full](Backend &b) { return b.mkdir(full); },
                [this, &store, id, here](orbit::Result<bool> &&r)
                {
                    if (id != request_ || !open_)
                        return;
                    loading_ = false;
                    if (!r.ok())
                    {
                        error_ = r.error;
                        return;
                    }
                    load(store, here);
                });
        }
        if (!typing && typed == TextPrompt::Result::none)
        {
            const int count = static_cast<int>(rows_.size());
            if (input.nav == Direction::up || input.nav == Direction::down)
            {
                const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
                if (next >= 0 && next < count)
                {
                    focus_ = next;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            if (!loading_ && input.is_pressed(Action::confirm))
            {
                activate(store, focus_, feedback);
                if (result_choose_)
                {
                    result_choose_ = false;
                    result = Result::chosen;
                }
            }
            else if (input.is_pressed(Action::back))
            {
                if (path_.empty())
                {
                    open_ = false;
                    feedback.play(hui::audio::Cue::modal_close);
                    result = Result::cancelled;
                }
                else
                {
                    const std::string up = parent_folder(path_);
                    if (up.empty())
                        load_mounts(store);
                    else
                        load(store, up, true);
                    feedback.play(hui::audio::Cue::focus);
                }
            }
        }
    }
    row_focus_.resize(rows_.size(), 0.0f);
    for (std::size_t i = 0; i < row_focus_.size(); ++i)
    {
        const float target = open_ && static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
        row_focus_[i] += (target - row_focus_[i]) * (1.0f - std::exp(-18.0f * dt));
    }
    scroll_.target = std::max(0.0f, (static_cast<float>(focus_) + 1.0f - kPickerRows) * kPickerRow);
    scroll_.update(dt, 14.0f);
    return result;
}

void FolderPicker::draw(Context &ctx, Frame &frame) const
{
    if (!open_ && in_.value < 0.01f && !prompt_.is_open())
        return;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx.type;
    const float a = std::clamp(in_.value, 0.0f, 1.0f);
    draw_scrim(list, a);
    const int count = static_cast<int>(rows_.size());
    const int visible = std::max(1, std::min(count, kPickerRows));
    const float width = 1100.0f;
    const float header = 170.0f;
    const float list_h = static_cast<float>(visible) * kPickerRow;
    const float height = header + list_h + 50.0f;
    const ui::Rect r{960.0f - width * 0.5f, 540.0f - height * 0.5f + (1.0f - a) * 20.0f, width,
                     height};
    list.push_opacity(a);
    ui::modal(list, r);
    ui::text_fit(list, type.light, title_, r.x + 48.0f, r.y + 82.0f, 38.0f, r.w - 96.0f,
                 ui::color::text);
    ui::text_fit(list, type.regular, path_.empty() ? std::string(tr("Storage locations")) : path_,
                 r.x + 48.0f, r.y + 124.0f, 22.0f, r.w - 96.0f, ui::color::text3);
    if (loading_)
        ui::text(list, type.regular, tr("Loading\xE2\x80\xA6"), r.x + 48.0f, r.y + 156.0f, 20.0f,
                 ui::color::text3);
    else if (!error_.empty())
        ui::text_fit(list, type.regular, error_, r.x + 48.0f, r.y + 156.0f, 20.0f, r.w - 96.0f,
                     ui::color::tier_red);
    const ui::Rect body{r.x, r.y + header, r.w, list_h};
    list.push_clip({body.x, body.y - 4.0f, body.w, body.h + 8.0f});
    for (int i = 0; i < count; ++i)
    {
        const Row &row = rows_[static_cast<std::size_t>(i)];
        const float y = body.y + static_cast<float>(i) * kPickerRow - scroll_.value;
        if (y + kPickerRow < body.y - 8.0f || y > body.y + body.h + 8.0f)
            continue;
        const ui::Rect box{r.x + 32.0f, y + 4.0f, r.w - 64.0f, kPickerRow - 8.0f};
        const float f = static_cast<std::size_t>(i) < row_focus_.size()
                            ? row_focus_[static_cast<std::size_t>(i)]
                            : 0.0f;
        ui::focus_ring(list, box, 14.0f, f);
        ui::surface(list, box, 14.0f, ui::Color::rgb(0xffffff, 0.07f + 0.07f * f));
        const bool action = row.kind == Kind::use_here || row.kind == Kind::new_folder ||
                            row.kind == Kind::parent;
        const ui::Color ink = row.enabled ? ui::color::text : ui::color::text3;
        const float x = box.x + 24.0f;
        if (row.detail.empty())
        {
            ui::text_fit(list, action ? type.semibold : type.medium, row.label, x, box.cy() + 8.0f,
                         24.0f, box.w - 60.0f, ink);
        }
        else
        {
            ui::text_fit(list, action ? type.semibold : type.medium, row.label, x, box.cy() - 2.0f,
                         24.0f, box.w - 60.0f, ink);
            ui::text_fit(list, type.regular, row.detail, x, box.cy() + 22.0f, 18.0f, box.w - 60.0f,
                         hui::gfx::mix(ui::color::text3, ui::color::text, f));
        }
    }
    list.pop_clip();
    list.pop_opacity();
    // The name prompt draws itself above everything.
    prompt_.draw(ctx, frame);
}

} // namespace orbit
