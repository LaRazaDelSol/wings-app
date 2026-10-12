// Wings TV app - Panels screens open on top of themselves: text entry, confirmation, choice lists.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Immediate-mode like the rest of the shell: each panel owns its small state
// machine, a screen calls update() while it is open (and treats itself as
// modal), and draw() records into the frame's overlay. They follow Orbit's
// dialog pattern (in-screen state, default focus off the dangerous button).

#pragma once

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "orbit/model.hpp"
#include "orbit/ui.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/feedback.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

struct Context;
struct Frame;
class Store;

// A line of text typed with the on-screen keyboard (the kit's InputPrompt).
class TextPrompt
{
  public:
    struct Options
    {
        int max_length = 200;
        bool password = false;
        bool allow_empty = false;
        // Every prompt has the search box's editing keys (R2 / Options submit,
        // L1 / R1 move the caret, L3 and a button clear, R3 symbols, hints).
        // `search` only names the submit hint "Search"; `password` marks a secret:
        // masked, Triangle shows it.
        bool search = false;
        std::string note; // a line under the field (e.g. "Tried 6 known passwords")
        std::vector<std::string> chips; // short answers under the field (recent searches)
        std::string placeholder;
        std::string done_label; // empty: "Done"
    };
    enum class Result : std::uint8_t
    {
        none,
        accepted,
        cancelled,
    };

    void open(hui::ui::Feedback &feedback, const std::string &title, const std::string &initial,
              const Options &options);
    bool is_open() const
    {
        return prompt_.is_open();
    }
    // Drives the prompt for a frame. Only the frame it closes returns a result.
    Result update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    const std::string &text() const
    {
        return prompt_.text();
    }
    // Counts the edits since the prompt opened (typing, deleting, clearing).
    std::uint32_t changed_serial() const
    {
        return prompt_.edits();
    }
    // The caret and the bindings the open prompt uses (tests).
    const hui::ui::InputPrompt &prompt() const
    {
        return prompt_;
    }
    hui::ui::InputPrompt &prompt()
    {
        return prompt_;
    }
    // Tests: put characters in the field as if they were typed on the keyboard.
    void type_text(std::string_view text)
    {
        if (prompt_.field.insert(text))
            prompt_.note_edit();
        prompt_.keyboard.set_length(prompt_.field.length());
    }
    void draw(Context &ctx, Frame &frame) const;

  private:
    hui::ui::InputPrompt prompt_;
    bool themed_ = false;
};

// A question with two buttons (or one, as a notice).
class ConfirmDialog
{
  public:
    enum class Result : std::uint8_t
    {
        none,
        confirmed,
        cancelled,
    };

    // destructive: the focus starts on Cancel, never on the dangerous button.
    void open(std::string title, std::string message, std::string confirm_label,
              std::string cancel_label, bool destructive);
    // A notice: one button, "OK" by default; Back closes it too.
    void notice(std::string title, std::string message, std::string ok_label = {});
    bool is_open() const
    {
        return open_;
    }
    Result update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(Context &ctx, Frame &frame) const;

  private:
    bool open_ = false;
    bool single_ = false;
    bool destructive_ = false;
    int focus_ = 1; // 0 cancel, 1 confirm
    std::string title_;
    std::string message_;
    std::string confirm_;
    std::string cancel_;
    ui::Ease in_;
    std::vector<float> button_focus_ = {0.0f, 0.0f};
};

struct ListItem
{
    std::string label;
    std::string detail;
    std::string right;
    bool checked = false; // multi-select
    bool enabled = true;
};

// A list in a panel: pick one (Cross chooses), or tick several and confirm.
class ListDialog
{
  public:
    enum class Mode : std::uint8_t
    {
        single,
        multi,
    };
    enum class Result : std::uint8_t
    {
        none,
        chosen,    // single: chosen() is the index
        confirmed, // multi: checked() are the ticked items
        cancelled,
    };

    void open(std::string title, std::string subtitle, std::vector<ListItem> items, Mode mode,
              std::string confirm_label = {});
    bool is_open() const
    {
        return open_;
    }
    Result update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(Context &ctx, Frame &frame) const;

    int chosen() const
    {
        return chosen_;
    }
    std::vector<int> checked() const;
    const std::vector<ListItem> &items() const
    {
        return items_;
    }
    int focus() const
    {
        return focus_;
    }

  private:
    bool open_ = false;
    Mode mode_ = Mode::single;
    std::string title_;
    std::string subtitle_;
    std::string confirm_;
    std::vector<ListItem> items_;
    int focus_ = 0; // an item, or items_.size() for the confirm button (multi)
    int chosen_ = -1;
    ui::Ease in_;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
};

// A folder chooser over Pegasus DL's /api/mounts and /api/browse: storage
// locations first, then folders; "Use this folder" where the storage is
// writable, "New folder" (on-screen keyboard) and a way back up. It is the
// download-folder picker and the destination picker of the file manager.
class FolderPicker
{
  public:
    enum class Result : std::uint8_t
    {
        none,
        chosen,
        cancelled,
    };

    // start: a folder to open on, or empty for the list of storage locations.
    void open(Store &store, std::string title, const std::string &start);
    bool is_open() const
    {
        return open_;
    }
    Result update(Store &store, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(Context &ctx, Frame &frame) const;
    const std::string &chosen() const
    {
        return chosen_;
    }
    // Tests: where the picker is, and what it shows.
    const std::string &path() const
    {
        return path_;
    }
    bool loading() const
    {
        return loading_;
    }
    const std::string &error() const
    {
        return error_;
    }
    int row_count() const
    {
        return static_cast<int>(rows_.size());
    }
    std::string row_label(int index) const
    {
        return index >= 0 && index < row_count() ? rows_[static_cast<std::size_t>(index)].label
                                                 : std::string();
    }
    void type_into_prompt(std::string_view text)
    {
        prompt_.type_text(text);
    }
    bool prompt_open() const
    {
        return prompt_.is_open();
    }

  private:
    enum class Kind : std::uint8_t
    {
        use_here,
        new_folder,
        parent,
        mount,
        folder,
    };
    struct Row
    {
        Kind kind = Kind::folder;
        std::string label;
        std::string detail;
        std::string path;
        bool enabled = true;
    };
    // walk_up: going up. A parent that cannot be listed (the console refuses
    // /mnt) falls back to its own parent, then the storage locations, so Up
    // and Circle always move somewhere (Pegasus's browseDir does the same).
    void load(Store &store, const std::string &path, bool walk_up = false);
    void load_mounts(Store &store);
    void rebuild_rows();
    bool writable_here() const;
    void activate(Store &store, int index, hui::ui::Feedback &feedback);

    bool open_ = false;
    bool loading_ = false;
    std::string title_;
    std::string path_; // empty: the storage locations
    std::string chosen_;
    std::string error_;
    std::vector<StorageMount> mounts_;
    std::vector<FileItem> folders_;
    std::vector<Row> rows_;
    int focus_ = 0;
    std::uint64_t request_ = 0;
    bool result_choose_ = false;
    TextPrompt prompt_;
    ui::Ease in_;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
};

// The dim veil behind every panel.
void draw_scrim(hui::gfx::DrawList &list, float amount);

} // namespace orbit
