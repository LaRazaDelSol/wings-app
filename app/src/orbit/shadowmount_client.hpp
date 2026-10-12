// Wings TV app - A small client for ShadowMountPlus' local HTTP API (127.0.0.1 only).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ShadowMountPlus (drakmor/ShadowMountPlus, GPL-3.0) mounts Wings and the games on this
// console. Its optional local API is read here the way Orbit Store's library backend reads it:
// the port comes from /data/shadowmount/config.ini (`api_port`, default 10101; `api_enabled=0`
// turns it off), every call is a POST with a JSON object body, and `status` 0 means success.
// Wings uses three routes: /api/v1/version, /api/v1/scan and /api/v1/games. Nothing here
// bundles or links ShadowMount; a console without it simply answers "not reachable".
//
// Calls block: run them off the frame (shadowmount_scan.hpp does).

#pragma once

#include "orbit/http.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace orbit
{

constexpr int kShadowMountDefaultPort = 10101;
constexpr int kShadowMountProbeTimeoutMs = 2000;
constexpr int kShadowMountCallTimeoutMs = 10000;

struct ShadowMountConfig
{
    bool enabled = true; // false: api_enabled=0, or an api_port that is not 1..65535
    int port = kShadowMountDefaultPort;
};

// Reads the few keys Wings needs from config.ini text. Unknown and malformed lines are ignored.
ShadowMountConfig parse_shadowmount_config(std::string_view ini);

struct VersionReply
{
    bool reached = false; // an HTTP answer came back
    bool ok = false;      // status 0
    bool can_rescan = true; // "rescan" capability, or no capability list at all
    std::string version;
    std::string error;
};

struct ScanReply
{
    bool reached = false;
    bool ok = false; // status 0
    bool queued = false;
    bool deferred = false;
    std::string reason; // "", game_active, runtime_prepared, rest_mode
    std::string error;
};

struct GamesReply
{
    bool reached = false;
    bool ok = false;
    std::set<std::string> title_ids;
    std::string error;
};

VersionReply parse_version_reply(const http::Response &response);
ScanReply parse_scan_reply(const http::Response &response);
GamesReply parse_games_reply(const http::Response &response);

using TransportFactory =
    std::function<std::unique_ptr<http::Transport>(std::uint16_t port, int timeout_ms)>;

// What the client needs from the outside; every piece may be left empty.
struct ShadowMountEnv
{
    // Builds a transport to 127.0.0.1:port. Empty: the API is never called (preview, tests).
    TransportFactory transport;
    // The text of /data/shadowmount/config.ini, or empty when it cannot be read.
    std::function<std::string()> read_config;
    // Runs a blocking job off the frame thread. Empty: runs it inline (tests).
    std::function<void(std::function<void()>)> run;
};

class ShadowMountClient
{
  public:
    explicit ShadowMountClient(ShadowMountEnv env) : env_(std::move(env))
    {
    }
    // False when no transport was supplied: calls then report "not reachable" at once.
    bool wired() const
    {
        return static_cast<bool>(env_.transport);
    }
    ShadowMountConfig config() const;
    // Hands a blocking job to the env's runner (inline when there is none).
    void run(std::function<void()> job) const
    {
        if (env_.run)
            env_.run(std::move(job));
        else
            job();
    }

    VersionReply version(); // 2 s timeout
    ScanReply scan();       // POST /api/v1/scan {"reset_attempts":false}
    GamesReply games();     // POST /api/v1/games {"include_size":false}

  private:
    http::Response post(const char *route, const char *body, int timeout_ms, bool *disabled);
    ShadowMountEnv env_;
};

} // namespace orbit
