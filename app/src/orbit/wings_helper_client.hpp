// Wings TV app - talking to the Wings update helper, the small payload that downloads and places a newer
// Wings package with the rights the TV app does not have.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The helper listens on 127.0.0.1 (kWingsHelperPort) and answers plain HTTP under /wings-helper/v1/:
//   GET  status   -> {"helper","api","phase","version","received","size","error","target",
//                     "installed_version","old_kept"}
//   POST download {"version","url","sha256","size","replace","running"} -> 202 (started), 409 (busy), 400;
//                 fetches and checks the package, then phase "downloaded" (kept across helper restarts)
//   POST install  the same body -> 202, or 409 "not_downloaded"; hashes the downloaded file again and places
//                 it. Never downloads.
//   POST cancel   stops a download between chunks (the partial file goes)
//   POST stop     ends the helper (refused with 409 only while it renames files)
//   POST tidy     {"running"} removes the kept .old once the running version is the placed one or newer
// Every request carries "X-Wings-Helper: 1"; POST bodies are JSON.
// When nothing answers, the app sends the helper ELF it ships to the payload loader (the same socket send
// Pegasus DL uses) and waits for it to come up. A helper from an older Wings is stopped and sent again.
#pragma once

#include "orbit/http.hpp"
#include "../../helper/version.h" // WINGS_HELPER_BUILD, shared with the helper

#include <cstdint>
#include <functional>
#include <string>

namespace orbit
{

inline constexpr std::uint16_t kWingsHelperPort = 9178;
inline constexpr int kWingsHelperApi = WINGS_HELPER_API; // 2: separate download and install routes
// The "helper" field of the build this Wings ships. It comes from the helper's own header (app/helper/version.h,
// WINGS_HELPER_BUILD), the one place it is set; bump it there whenever the helper changes.
inline constexpr const char *kWingsHelperBuild = WINGS_HELPER_BUILD;
inline constexpr const char *kWingsHelperElf = "/app0/assets/helper/wings-helper.elf";
inline constexpr int kWingsHelperProbeMs = 500;   // one status read
inline constexpr int kWingsHelperStartPollMs = 250; // after a send: a status read this often...
inline constexpr int kWingsHelperStartPolls = 40;   // ...for ten seconds
inline constexpr int kWingsHelperSendTries = 3;     // the loader refusing a send: tries, one second apart

enum class WingsHelperPhase : std::uint8_t
{
    unknown, // not readable
    idle,
    downloading,
    verifying,
    downloaded, // a checked download waits to be installed (version names it)
    placing,
    placed,
    error
};

// Where the package would go, as the helper sees /data/homebrew.
enum class WingsHelperTarget : std::uint8_t
{
    unknown,
    none,    // no package there
    wings,   // the file Wings placed (its record matches)
    manual,  // a file the player placed: replacing it needs the player's yes
    folder,  // the unpacked folder form: never installed beside
    blocked  // something else is in the way
};

struct WingsHelperStatus
{
    std::string build; // "helper"
    int api = 0;
    WingsHelperPhase phase = WingsHelperPhase::unknown;
    std::string version; // the release being (or last) installed
    std::uint64_t received = 0;
    std::uint64_t size = 0;
    std::string error;
    WingsHelperTarget target = WingsHelperTarget::unknown;
    std::string installed_version;
    bool old_kept = false;
    bool busy() const
    {
        return phase == WingsHelperPhase::downloading || phase == WingsHelperPhase::verifying ||
               phase == WingsHelperPhase::placing;
    }
};

struct WingsHelperInstall
{
    std::string version; // the release tag
    std::string url;     // the official package address for that tag
    std::string sha256;  // agreed: the API digest and the published .sha256
    std::uint64_t size = 0;
    bool replace = false; // the player said yes to replacing their own copy
    std::string running;  // this app's contentVersion
};

// ---- pure (tests) ----
bool decode_wings_helper_status(const std::string &json, WingsHelperStatus *out);
const char *wings_helper_phase_name(WingsHelperPhase phase);
// One line for the log: phase, release, helper build, target, installed version, old file, bytes. Never secrets.
std::string describe_wings_helper_status(const WingsHelperStatus &s);
std::string encode_wings_helper_install(const WingsHelperInstall &request);
std::string encode_wings_helper_tidy(const std::string &running);
// The request for one route ("status" is a GET, the others POST `body`).
http::Request wings_helper_request(const std::string &route, const std::string &body = std::string());

// Sends the ELF at the path to the payload loader. False with a sentence in *why.
using WingsHelperSender = std::function<bool(const std::string &elf_path, std::string *why)>;

class WingsHelperClient
{
  public:
    // `build` is the helper build this Wings ships ("" accepts any build of the same api).
    WingsHelperClient(http::Transport &transport, WingsHelperSender sender, std::string elf_path = kWingsHelperElf,
                      std::string build = std::string());

    // Waits (tests replace it so nothing really sleeps).
    std::function<void(int ms)> sleep_ms;

    // One status read; false when nothing (readable) answers.
    bool status(WingsHelperStatus *out);
    // The shipped helper answering: probes, stops a helper of another build (cancelling its download first; one
    // that is renaming files is waited for), sends the ELF, waits for it.
    bool ensure_running(WingsHelperStatus *out, std::string *why);
    // Set by ensure_running when it stopped a helper of another build: that helper's status line (for the log).
    std::string stopped_stale;

    enum class Answer : std::uint8_t
    {
        accepted,    // 202: the helper started
        busy,        // 409 "busy": a job is running
        old_waiting, // 409 "old_waiting": PPSA99178.ffpkg.old from an earlier update is still there
        refused,     // 400 or another status; *why has the helper's words
        unreachable, // nothing answered
        not_downloaded // install: 409 "not_downloaded", no checked download of that release
    };
    // Starts the download (and check) of the release; the helper then reports "downloaded".
    Answer download(const WingsHelperInstall &request, std::string *why);
    // Places the downloaded release. Never downloads.
    Answer install(const WingsHelperInstall &request, std::string *why);
    bool cancel();
    bool stop();
    bool tidy(const std::string &running);

  private:
    bool current(const WingsHelperStatus &s) const;
    Answer start(const std::string &route, const WingsHelperInstall &request, std::string *why);
    http::Response post(const std::string &route, const std::string &body);
    void wait(int ms);

    http::Transport &transport_;
    WingsHelperSender sender_;
    std::string elf_path_;
    std::string build_;
};

} // namespace orbit
