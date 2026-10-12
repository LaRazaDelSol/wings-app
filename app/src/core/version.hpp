// ps5-homebrew-ui - The app version, read from the packaged param.json, and which build of it this is.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace hui
{

// The "contentVersion" value (PlayStation NN.NNN.NNN, e.g. "01.000.000") from
// param.json text, or "" when it is missing or malformed. Release tags use the
// same numbers (v1.0.0), so what the app shows matches the GitHub release.
std::string content_version(std::string_view param_json);

// Reads param.json from path; "" when it cannot be read.
std::string read_content_version(const std::string &path);

// "00.001.001" -> {0, 1, 1}. False for anything but NN.NNN.NNN.
bool parse_content_version(std::string_view text, int out[3]);
// "v0.1.1" -> {0, 1, 1}. A release tag is v<major>.<minor>.<patch>: digits only, one to three each.
bool parse_release_tag(std::string_view tag, int out[3]);
// -1, 0, 1.
int compare_versions(const int a[3], const int b[3]);
// "00.001.001" -> "0.1.1", the form the release page and its tag use. "" for a malformed one.
std::string release_version_text(std::string_view content_version);

// Which build of that version this is: assets/build-info.json, written by the build
// (tools/wings-stamp.py): {"build":"20261012-203105","channel":"test   "}.
struct BuildInfo
{
    bool known = false;   // the stamp was there and valid
    bool release = false; // channel "release" (a test package says "test")
    std::string build;    // UTC build time, YYYYMMDD-HHMMSS
};
BuildInfo parse_build_info(std::string_view json);
BuildInfo read_build_info(const std::string &path); // not known when it cannot be read

// What the player reads: "0.1.1" for a release build, "0.1.1 · test 20261012-203105" for a test build.
// An unknown stamp shows the version alone.
std::string version_label(std::string_view content_version, const BuildInfo &info);
// Identity of this exact build ("00.001.001+20261012-203105"; the version alone when the stamp is
// unknown). Saved data is tagged with it, so a new build fetches fresh data.
std::string build_tag(std::string_view content_version, const BuildInfo &info);

} // namespace hui
