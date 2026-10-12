// Wings TV app - Editorial data: short per-game notes and the curated store carousel.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two small JSON files Wings fetches over https from the project site, like the popularity data:
//   notes.v1.json   {schemaVersion:1, notes:{titleId:"text, 1..160 characters"}}   (<= 600 notes, <= 64000 bytes)
//   curated.v1.json {schemaVersion:1, items:[titleId, ...]}                         (ordered, <= 24 ids)
// Both are switched by Wings preferences (notes_enabled, carousel_enabled). EditorialService loads the saved copy and
// fetches both in one background job through a DocFetcher; a failure keeps the saved copy and a missing file (404)
// just means "none". Everything here is platform neutral and tested on the host.

#pragma once

#include "orbit/popularity.hpp"
#include "orbit/thread.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

constexpr std::size_t kEditorialNotesMaxBytes = 64000;
constexpr std::size_t kEditorialCuratedMaxBytes = 4096;
constexpr std::size_t kEditorialMaxNotes = 600;
constexpr std::size_t kEditorialMaxNoteChars = 160;
constexpr std::size_t kEditorialMaxCurated = 24;
constexpr const char *kEditorialNotesUrl = "https://larazadelsol.github.io/wings-app/notes.v1.json";
constexpr const char *kEditorialCuratedUrl = "https://larazadelsol.github.io/wings-app/curated.v1.json";

// "PPSA01234" / "CUSA01234": the only title id shape the files may use.
bool valid_editorial_id(std::string_view id);

class EditorialData
{
  public:
    // Each fills its own half of `*into` (the other half is left as it is). Oversize text, bad JSON and a
    // missing/newer schemaVersion are errors; single bad entries are skipped. An empty result is fine.
    static bool parse_notes(std::string_view text, EditorialData *into, std::string *error);
    static bool parse_curated(std::string_view text, EditorialData *into, std::string *error);

    // The note of the first of the game's title ids that has one; "" none.
    std::string note(const std::vector<std::string> &title_ids) const;
    // The carousel's title ids in the file's order (deduplicated, valid only).
    const std::vector<std::string> &curated_ids() const { return curated_; }
    std::size_t note_count() const { return notes_.size(); }
    void clear_notes() { notes_.clear(); }
    void clear_curated() { curated_.clear(); }

  private:
    std::map<std::string, std::string> notes_;
    std::vector<std::string> curated_;
};

struct EditorialStatus
{
    std::string message; // "Updated", "Using the saved copy", "Could not refresh: ..." (never a popup)
    std::int64_t updated = 0;
    bool busy = false;
};

// Shared by the worker (data, status) and the frame thread (everything else).
class EditorialHub
{
  public:
    void set_enabled(bool notes, bool carousel);
    bool notes_enabled() const;
    bool carousel_enabled() const;
    void set_data(std::shared_ptr<const EditorialData> data); // null: forget
    std::shared_ptr<const EditorialData> data() const;         // regardless of the switches
    // The game's note while notes are on; "" otherwise.
    std::string note(const std::vector<std::string> &title_ids) const;
    // The carousel's ids while the carousel is on; empty otherwise.
    std::vector<std::string> curated_ids() const;
    void set_status(const EditorialStatus &status);
    EditorialStatus status() const;
    std::uint64_t serial() const; // changes with the data and the switches
    // Settings and startup call this; the app wires the hook to its EditorialService (none in the preview and in
    // tests: then only the switches change).
    void apply_prefs(bool notes, bool carousel)
    {
        set_enabled(notes, carousel);
        if (apply_hook)
            apply_hook(notes, carousel);
    }
    std::function<void(bool, bool)> apply_hook;

  private:
    mutable Mutex mutex_;
    bool notes_ = false;
    bool carousel_ = false;
    std::shared_ptr<const EditorialData> data_;
    EditorialStatus status_;
    std::uint64_t serial_ = 0;
};
EditorialHub &editorial_hub();

struct EditorialServiceConfig
{
    std::vector<std::string> cache_paths; // first readable wins; both writable ones are written
    std::string notes_url = kEditorialNotesUrl;
    std::string curated_url = kEditorialCuratedUrl;
    int ttl_s = 24 * 3600;
    std::string app_tag;                  // as PopularityServiceConfig::app_tag
    std::function<std::int64_t()> now;    // unix seconds (default: the system clock)
};

// Runs the cache load and the fetch of both files on one background thread.
class EditorialService
{
  public:
    EditorialService(EditorialHub &hub, DocFetcher &fetcher, EditorialServiceConfig config);
    ~EditorialService();
    // Frame thread. Sets the hub's switches; the first time either is on, loads the saved copy and fetches when it
    // is missing, from another build or older than the TTL.
    void apply(bool notes, bool carousel);
    void wait_idle(); // tests

  private:
    void run();
    EditorialHub &hub_;
    DocFetcher &fetcher_;
    EditorialServiceConfig config_;
    Thread thread_;
    bool started_ = false;
};

} // namespace orbit
