// Wings TV app - Preferences kept on the console by Wings only (never sent to Pegasus DL).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link ranking preferences (which link Wings puts first) and the popularity-data
// switch. Plain strings, so any screen or ranking code can read them without
// depending on the link model. Persisted as key=value lines in the checked
// save container (prefs kind), file name chosen by the caller.

#pragma once

#include "orbit/after_download_mode.hpp"
#include "orbit/art_sources.hpp"
#include "orbit/pegasus_start_mode.hpp"
#include "orbit/tile_details.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

struct WingsLocalPrefs
{
    // Empty means "no preference". Firmware is a family label such as "10.xx".
    std::string firmware;
    // One of kRegionChoices ("USA", "EUR", ...).
    std::string region;
    // One of kFormatChoices ("exFAT", "FPKG", "PKG", "Dump").
    std::string format;
    // Put links a ready debrid account takes ahead of the others.
    bool prefer_debrid = true;
    // Popularity data: on by default for a fresh install; a saved "popularity=0" is kept.
    bool popularity_enabled = true;
    std::string popularity_url;
    // Store grid: when tiles show their tier pill and region chips (files from
    // before this choice have no key and load the default).
    TileDetails tile_details = TileDetails::on_focus;
    // What "Waiting for Pegasus DL" does about loading Pegasus DL (ask, auto, manual).
    PegasusStartMode pegasus_start = PegasusStartMode::ask;
    // What Wings does when a download finishes: ask ShadowMount to scan (default) or nothing.
    AfterDownload after_download = AfterDownload::scan;
    // Look for a newer Wings release when the app starts (wings_update.hpp). On by default; files from before
    // this choice have no key and load the default.
    bool update_check = true;
    // Where cover icons and the 16:9 game-page art come from (art_sources.hpp). Defaults: the catalogue's own
    // covers, and every art source.
    IconSource icon_source = IconSource::catalogue;
    HeroSource hero_source = HeroSource::all;
    // Editorial data (editorial.hpp): the short notes on game pages and the curated carousel on the store. On by
    // default; folder_confirmed is set once the player has confirmed the games folder (files from before have no keys).
    bool notes_enabled = true;
    bool carousel_enabled = true;
    bool folder_confirmed = false;
    // Retry a download by itself when the connection drops (download_auto_retry.hpp). On by default; files
    // from before this choice have no key and load the default.
    bool auto_reconnect = true;
    // Keep the .pkg file after Wings installed it (pkg_install.hpp). Off by default: a .pkg that
    // installed is deleted. A failed install always keeps the file.
    bool keep_pkg_files = false;

    bool operator==(const WingsLocalPrefs &other) const;
};

// The values the Settings pickers offer (the first entry of the picker, "Any", is the empty string).
extern const char *const kFirmwareChoices[];
extern const int kFirmwareChoiceCount;
extern const char *const kRegionChoices[];
extern const int kRegionChoiceCount;
extern const char *const kFormatChoices[];
extern const int kFormatChoiceCount;

// True for "", or a value in the list.
bool valid_firmware(std::string_view value);
bool valid_region(std::string_view value);
bool valid_format(std::string_view value);

std::string encode_local_prefs(const WingsLocalPrefs &prefs);
// Unknown keys and invalid choices are ignored (the default stays); false only for an empty payload.
bool decode_local_prefs(std::string_view payload, WingsLocalPrefs *out);
// First readable, valid file wins. The default stays when none loads.
bool load_local_prefs(const std::vector<std::string> &paths, WingsLocalPrefs *out);
// Writes every path that can be written; returns how many were.
int save_local_prefs(const std::vector<std::string> &paths, const WingsLocalPrefs &prefs);

} // namespace orbit
