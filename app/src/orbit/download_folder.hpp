// Wings TV app - The download folder: the one-time default and the first-download confirm.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure logic, no drawing. Two jobs (docs B-13):
//  1. When Pegasus reports an EMPTY downloadRoot, Wings sets a default folder
//     once (Store::ensure_default_download_folder). A non-empty root is never touched.
//  2. The first download the player tries asks them to confirm or change the
//     folder; the answer is remembered (WingsLocalPrefs::folder_confirmed) so it
//     is asked once. FirstDownloadGate is the check every download entry point calls.

#pragma once

#include <functional>
#include <string>
#include <utility>

namespace orbit
{

namespace download_folder
{
// The folder Wings sets when Pegasus has none.
inline constexpr const char *default_path = "/data/pegasus-dl/downloads";
} // namespace download_folder

struct FolderNeed
{
    bool set_default = false;  // create the default folder and make it the root
    bool root_unknown = false; // settings not read yet: decide nothing
};

// have_settings: Pegasus' settings were read. download_root: what it reported.
// already_defaulted: this run already tried (success or failure; never retried).
FolderNeed folder_need(bool have_settings, const std::string &download_root,
                       bool already_defaulted);

// First-download gate. Frame thread only.
class FirstDownloadGate
{
  public:
    enum class Verdict
    {
        proceed, // start the download now
        ask,     // show the "Download folder" confirm first, then resume
    };

    // Unknown settings never block: the root is not known, there is nothing to
    // confirm yet, and an offline Pegasus fails the download by itself.
    Verdict check(bool folder_confirmed, bool have_settings) const;

    // The player answered (kept the folder or picked another): never ask again
    // this run, even before the persisted flag is saved. The caller also sets
    // WingsLocalPrefs::folder_confirmed = true and saves.
    void answered()
    {
        answered_ = true;
    }

    // Convenience: remember what to do once the confirm closes (the download
    // that was interrupted). take_pending() returns it (empty if none).
    void defer(std::function<void()> resume)
    {
        pending_ = std::move(resume);
    }
    std::function<void()> take_pending()
    {
        std::function<void()> out = std::move(pending_);
        pending_ = nullptr;
        return out;
    }

  private:
    bool answered_ = false;
    std::function<void()> pending_;
};

// Free function form of the same rule (used by the gate and by tests).
bool needs_first_confirm(bool folder_confirmed, bool have_settings);

} // namespace orbit
