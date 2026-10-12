// Wings TV app - Cover art glue that needs neither OpenSSL nor the console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HTTPS fetching lives in wings/art (staged under src/platform/wings_art,
// console build only). This header holds the parts the screens, the preview and
// the unit tests share with it:
//   - PosterIndex: package id -> poster URL, filled from the Store's snapshot on
//     the frame thread and read by the art worker thread (the resolver);
//   - CoverArtControl / CoverArtStatus: what the Settings "Cover art" section
//     needs, implemented in main.cpp on top of wings::art::ArtService;
//   - WingsPrefs: the one persisted choice (DNS-over-HTTPS), in the checked
//     save container of core/save_file.

#pragma once

#include "orbit/backend.hpp"
#include "orbit/model.hpp"
#include "orbit/thread.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit
{

// The sentence Settings shows beside the DNS-over-HTTPS switch.
inline constexpr const char *kDohPrivacySentence =
    "Looking up cover servers this way sends their host names (for example "
    "image.api.playstation.com) to Cloudflare (1.1.1.1) and Google (8.8.8.8).";

class PosterIndex
{
  public:
    // Frame thread: replace the map when the catalogue changed (serial differs).
    // Only https:// poster URLs are kept. Returns true when it was replaced.
    bool sync(std::uint64_t serial, const std::vector<Package> &packages);

    // Any thread. "pkg:<id>" -> https URL, or "" when unknown (then the caller
    // falls through to the backend, which serves installed-game icons).
    std::string resolve(std::string_view key, ArtKind kind) const;

    std::size_t size() const;

  private:
    mutable Mutex mutex_;
    std::unordered_map<std::string, std::string> urls_;
    std::uint64_t serial_ = ~0ull;
};

struct CoverArtStatus
{
    bool available = false; // false in the preview and tests without the module
    bool doh = false;       // DNS-over-HTTPS switched on
    bool working = false;   // the service is ready to fetch
    std::string detail;     // the service's own user-facing sentence
};

class CoverArtControl
{
  public:
    virtual ~CoverArtControl() = default;
    virtual CoverArtStatus status() = 0; // frame thread, once per frame
    virtual void set_doh(bool on) = 0;   // persists the choice
    virtual void clear_cache() = 0;
    virtual void retry_failed() = 0;
};

struct WingsPrefs
{
    bool art_doh = true; // default on; a saved "0" (the player turned it off) is kept
};

// The container payload is "key=value" lines; unknown keys are ignored.
std::string encode_prefs(const WingsPrefs &prefs);
bool decode_prefs(std::string_view payload, WingsPrefs *out);
// First readable file wins. False when none could be read or all were damaged.
bool load_prefs(const std::vector<std::string> &paths, WingsPrefs *out);
// Writes every path whose directory exists or can be created; returns how many.
int save_prefs(const std::vector<std::string> &paths, const WingsPrefs &prefs);

} // namespace orbit
