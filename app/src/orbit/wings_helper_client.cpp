// Wings TV app - talking to the Wings update helper (see wings_helper_client.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/wings_helper_client.hpp"

#include "orbit/json.hpp"

#include <unistd.h>

#include <utility>

namespace orbit
{

namespace
{

constexpr const char *kRoutePrefix = "/wings-helper/v1/";

WingsHelperPhase phase_of(const std::string &s)
{
    if (s == "idle")
        return WingsHelperPhase::idle;
    if (s == "downloading")
        return WingsHelperPhase::downloading;
    if (s == "verifying")
        return WingsHelperPhase::verifying;
    if (s == "downloaded")
        return WingsHelperPhase::downloaded;
    if (s == "placing")
        return WingsHelperPhase::placing;
    if (s == "placed")
        return WingsHelperPhase::placed;
    if (s == "error")
        return WingsHelperPhase::error;
    return WingsHelperPhase::unknown;
}

WingsHelperTarget target_of(const std::string &s)
{
    if (s == "none")
        return WingsHelperTarget::none;
    if (s == "wings")
        return WingsHelperTarget::wings;
    if (s == "manual")
        return WingsHelperTarget::manual;
    if (s == "folder")
        return WingsHelperTarget::folder;
    if (s == "blocked")
        return WingsHelperTarget::blocked;
    return WingsHelperTarget::unknown;
}

std::uint64_t count_of(const json::Value &v)
{
    const double n = v.as_number(0.0);
    return n > 0.0 ? static_cast<std::uint64_t>(n) : 0u;
}

} // namespace

bool decode_wings_helper_status(const std::string &text, WingsHelperStatus *out)
{
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object())
        return false;
    WingsHelperStatus s;
    s.build = v.text("helper");
    s.api = static_cast<int>(v["api"].as_int(0));
    s.phase = phase_of(v.text("phase"));
    if (s.api <= 0 || s.phase == WingsHelperPhase::unknown)
        return false;
    s.version = v.text("version");
    s.received = count_of(v["received"]);
    s.size = count_of(v["size"]);
    s.error = v.text("error");
    s.target = target_of(v.text("target"));
    s.installed_version = v.text("installed_version");
    s.old_kept = v["old_kept"].as_bool(false);
    *out = std::move(s);
    return true;
}

const char *wings_helper_phase_name(WingsHelperPhase phase)
{
    switch (phase)
    {
    case WingsHelperPhase::idle:
        return "idle";
    case WingsHelperPhase::downloading:
        return "downloading";
    case WingsHelperPhase::verifying:
        return "verifying";
    case WingsHelperPhase::downloaded:
        return "downloaded";
    case WingsHelperPhase::placing:
        return "placing";
    case WingsHelperPhase::placed:
        return "placed";
    case WingsHelperPhase::error:
        return "error";
    case WingsHelperPhase::unknown:
    default:
        return "unknown";
    }
}

std::string describe_wings_helper_status(const WingsHelperStatus &s)
{
    static const char *const targets[] = {"unknown", "none", "wings", "manual", "folder", "blocked"};
    const std::size_t t = static_cast<std::size_t>(s.target);
    std::string line = std::string("phase=") + wings_helper_phase_name(s.phase) +
                       " version=" + (s.version.empty() ? "-" : s.version) +
                       " helper=" + (s.build.empty() ? "-" : s.build) + " api=" + std::to_string(s.api) +
                       " target=" + (t < sizeof targets / sizeof targets[0] ? targets[t] : "unknown") +
                       " installed=" + (s.installed_version.empty() ? "-" : s.installed_version) +
                       " old_kept=" + (s.old_kept ? "yes" : "no") + " bytes=" + std::to_string(s.received) + "/" +
                       std::to_string(s.size);
    if (!s.error.empty())
        line += " error=" + s.error;
    return line;
}

std::string encode_wings_helper_install(const WingsHelperInstall &r)
{
    return "{\"version\":" + json::quote(r.version) + ",\"url\":" + json::quote(r.url) +
           ",\"sha256\":" + json::quote(r.sha256) + ",\"size\":" + std::to_string(r.size) +
           ",\"replace\":" + (r.replace ? "true" : "false") + ",\"running\":" + json::quote(r.running) + "}";
}

std::string encode_wings_helper_tidy(const std::string &running)
{
    return "{\"running\":" + json::quote(running) + "}";
}

http::Request wings_helper_request(const std::string &route, const std::string &body)
{
    http::Request r;
    r.method = route == "status" ? "GET" : "POST";
    r.path = std::string(kRoutePrefix) + route;
    r.body = r.method == "GET" ? std::string() : (body.empty() ? std::string("{}") : body);
    r.headers = "X-Wings-Helper: 1\r\n";
    r.max_bytes = 16u << 10;
    r.timeout_ms = route == "status" ? kWingsHelperProbeMs : 2000;
    return r;
}

WingsHelperClient::WingsHelperClient(http::Transport &transport, WingsHelperSender sender, std::string elf_path,
                                     std::string build)
    : transport_(transport), sender_(std::move(sender)), elf_path_(std::move(elf_path)), build_(std::move(build))
{
}

void WingsHelperClient::wait(int ms)
{
    if (sleep_ms)
        sleep_ms(ms);
    else
        ::usleep(static_cast<useconds_t>(ms) * 1000u);
}

bool WingsHelperClient::current(const WingsHelperStatus &s) const
{
    return s.api == kWingsHelperApi && (build_.empty() || s.build == build_);
}

bool WingsHelperClient::status(WingsHelperStatus *out)
{
    const http::Response r = transport_.send(wings_helper_request("status"));
    return r.transported() && r.status == 200 && decode_wings_helper_status(r.body, out);
}

http::Response WingsHelperClient::post(const std::string &route, const std::string &body)
{
    return transport_.send(wings_helper_request(route, body));
}

bool WingsHelperClient::ensure_running(WingsHelperStatus *out, std::string *why)
{
    stopped_stale.clear();
    WingsHelperStatus s;
    if (status(&s))
    {
        if (current(s))
        {
            *out = s;
            return true;
        }
        // A helper left by another Wings package: its code is not the one this Wings talks to, so it is replaced.
        // A download is cancelled first; renaming files is never interrupted, so a helper placing is waited for.
        stopped_stale = describe_wings_helper_status(s);
        if (s.phase == WingsHelperPhase::downloading || s.phase == WingsHelperPhase::verifying)
            cancel();
        bool gone = false;
        for (int i = 0; i < kWingsHelperStartPolls && !gone; ++i)
        {
            WingsHelperStatus again;
            if (!status(&again))
            {
                gone = true;
                break;
            }
            if (again.phase != WingsHelperPhase::placing)
                post("stop", "{}");
            wait(kWingsHelperStartPollMs);
            gone = !status(&again);
        }
        if (!gone)
        {
            *why = "An older Wings helper is still finishing. Try again in a minute.";
            return false;
        }
    }
    std::string error;
    bool sent = false;
    for (int attempt = 0; attempt < kWingsHelperSendTries && !sent; ++attempt)
    {
        if (attempt > 0)
            wait(1000);
        error.clear();
        sent = sender_ && sender_(elf_path_, &error);
    }
    if (!sent)
    {
        *why = error.empty() ? std::string("The payload loader is not running.") : error;
        return false;
    }
    for (int i = 0; i < kWingsHelperStartPolls; ++i)
    {
        wait(kWingsHelperStartPollMs);
        if (status(&s))
        {
            if (!current(s))
            {
                *why = "The Wings helper that answered is not the one this Wings ships.";
                return false;
            }
            *out = s;
            return true;
        }
    }
    *why = "The Wings helper did not start.";
    return false;
}

WingsHelperClient::Answer WingsHelperClient::download(const WingsHelperInstall &request, std::string *why)
{
    return start("download", request, why);
}

WingsHelperClient::Answer WingsHelperClient::install(const WingsHelperInstall &request, std::string *why)
{
    return start("install", request, why);
}

WingsHelperClient::Answer WingsHelperClient::start(const std::string &route, const WingsHelperInstall &request,
                                                   std::string *why)
{
    const http::Response r = post(route, encode_wings_helper_install(request));
    if (!r.transported())
        return Answer::unreachable;
    if (r.status == 202 || r.status == 200)
        return Answer::accepted;
    // Refusals: {"error","reason"}, reason one of
    // invalid|busy|newer|folder|blocked|manual|old_waiting|space|not_downloaded.
    json::Value v;
    const bool readable = json::parse(r.body, &v) && v.is_object();
    const std::string reason = readable ? v.text("reason") : std::string();
    if (r.status == 409 && reason == "busy")
        return Answer::busy;
    if (readable && !v.text("error").empty())
        *why = v.text("error");
    else
        *why = reason.empty() ? "HTTP " + std::to_string(r.status) : reason;
    if (r.status == 409 && reason == "not_downloaded")
        return Answer::not_downloaded;
    return r.status == 409 && reason == "old_waiting" ? Answer::old_waiting : Answer::refused;
}

bool WingsHelperClient::cancel()
{
    const http::Response r = post("cancel", "{}");
    return r.transported() && r.status >= 200 && r.status <= 299;
}

bool WingsHelperClient::stop()
{
    const http::Response r = post("stop", "{}");
    return r.transported() && r.status >= 200 && r.status <= 299;
}

bool WingsHelperClient::tidy(const std::string &running)
{
    const http::Response r = post("tidy", encode_wings_helper_tidy(running));
    return r.transported() && r.status >= 200 && r.status <= 299;
}

} // namespace orbit
