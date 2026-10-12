// Wings TV app - "Where to?": the destination of a copy, move or extraction.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only split view of the Files tab: a summary of what is being sent on the
// left, a folder browser over Pegasus DL's /api/files/list on the right (the
// same listing the Files tab uses, rooted at "/"), with the drive chips on top.
// Row 0 is pinned: "Move here" (or Copy / Extract). Files are hidden but
// counted. Square cycles what happens to items that already exist (Keep both,
// Replace, Skip); Triangle, or Cross on the pinned row, chooses. Circle goes
// up one folder, falls back further up when a parent cannot be listed, and at
// "/" closes the browser (the selection stays). Nothing blocks input while a
// folder is being read.

#pragma once

#include "core/input.hpp"
#include "orbit/files_logic.hpp"
#include "orbit/model.hpp"
#include "orbit/panels.hpp"
#include "orbit/ui.hpp"
#include "ui/feedback.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orbit
{

struct Context;
struct Frame;
class Store;

class DestinationBrowser
{
  public:
    enum class Result : std::uint8_t
    {
        none,
        chosen,
        cancelled,
    };
    enum class Verb : std::uint8_t
    {
        copy,
        move,
        extract,
    };
    struct Request
    {
        Verb verb = Verb::copy;
        std::vector<FileItem> items; // what is sent (paths, sizes, folders)
        std::string from;            // the folder they are in
        std::string start;           // where the browser opens
        std::string collision = "keep-both";
    };

    void open(Store &store, Request request, const std::vector<StorageMount> &locations,
              files::Trace *trace);
    bool is_open() const
    {
        return open_;
    }
    Result update(Store &store, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(Context &ctx, Frame &frame) const;

    // After chosen.
    const std::string &destination() const
    {
        return chosen_;
    }
    const std::string &collision() const
    {
        return collision_;
    }

    // Tests and previews.
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
    bool row_enabled(int index) const
    {
        return index >= 0 && index < row_count() && rows_[static_cast<std::size_t>(index)].enabled;
    }
    int focus() const
    {
        return focus_;
    }
    int hidden_files() const
    {
        return hidden_files_;
    }
    bool on_chips() const
    {
        return on_chips_;
    }
    const files::DestCheck &check() const
    {
        return check_;
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
        here,       // the pinned "Move here"
        new_folder,
        up,
        folder,
        retry,
        files_note, // "...and 2 files here (hidden)"
    };
    struct Row
    {
        Kind kind = Kind::folder;
        std::string label;
        std::string detail;
        std::string path;
        bool enabled = true;
    };
    void load(Store &store, const std::string &path, bool walk_up, const std::string &focus_name);
    void rebuild_rows(const std::string &focus_name);
    void activate(Store &store, hui::ui::Feedback &feedback, Result &result);
    void choose(hui::ui::Feedback &feedback, Result &result);
    void go_up(Store &store, hui::ui::Feedback &feedback, Result &result);
    const char *verb_here() const;
    std::string title() const;
    void trace(const std::string &line);

    bool open_ = false;
    Request request_;
    std::vector<StorageMount> locations_;
    std::vector<files::Drive> drives_;
    files::Trace *trace_ = nullptr;
    std::string path_;
    bool writable_ = true;
    bool loading_ = false;
    std::string error_;
    std::vector<FileItem> folders_;
    int hidden_files_ = 0;
    std::vector<Row> rows_;
    int focus_ = 0;
    bool on_chips_ = false;
    int chip_ = 0;
    std::uint64_t request_id_ = 0;
    double request_started_ = 0.0;
    std::string chosen_;
    std::string collision_ = "keep-both";
    files::DestCheck check_;
    double need_bytes_ = 0.0;
    bool any_folder_ = false;
    TextPrompt prompt_;
    ui::Ease in_;
    ui::Ease scroll_;
    std::vector<float> row_focus_;
};

} // namespace orbit
