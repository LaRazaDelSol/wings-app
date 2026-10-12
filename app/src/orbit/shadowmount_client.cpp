// Wings TV app - A small client for ShadowMountPlus' local HTTP API.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/shadowmount_client.hpp"

#include "orbit/json.hpp"

#include <cctype>
#include <cstdlib>

namespace orbit
{

namespace
{

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

// The first word of a value ("0 ; off" -> "0"), as Orbit Store reads it.
std::string_view first_word(std::string_view text)
{
    text = trim(text);
    std::size_t end = 0;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
        ++end;
    return text.substr(0, end);
}

// Common checks on a reply. Fills the error and returns the parsed object when it is usable.
bool open_reply(const http::Response &response, bool *reached, std::string *error, json::Value *out)
{
    *reached = response.transported();
    if (!*reached)
    {
        *error = response.error.empty() ? std::string("no answer") : response.error;
        return false;
    }
    std::string problem;
    if (!json::parse(response.body, out, &problem) || !out->is_object())
    {
        *error = "unexpected reply";
        return false;
    }
    // Every reply carries a numeric status: 0 is success, a positive errno on failure.
    if (response.status < 200 || response.status >= 300 || !out->has("status") ||
        out->number("status", -1.0) != 0.0)
    {
        *error = out->text("error").empty() ? "ShadowMount refused the request" : out->text("error");
        return false;
    }
    return true;
}

} // namespace

ShadowMountConfig parse_shadowmount_config(std::string_view ini)
{
    ShadowMountConfig config;
    std::size_t at = 0;
    while (at < ini.size())
    {
        std::size_t end = ini.find('\n', at);
        if (end == std::string_view::npos)
            end = ini.size();
        const std::string_view line = trim(ini.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = first_word(line.substr(eq + 1));
        if (key == "api_enabled")
        {
            if (value == "0" || value == "false" || value == "no" || value == "off")
                config.enabled = false;
        }
        else if (key == "api_port")
        {
            // Like Orbit Store: a port that is not a plain 1..65535 number means no API.
            const std::string text(value);
            char *stop = nullptr;
            const long n = std::strtol(text.c_str(), &stop, 10);
            if (text.empty() || *stop != '\0' || n < 1 || n > 65535)
                config.enabled = false;
            else
                config.port = static_cast<int>(n);
        }
    }
    return config;
}

VersionReply parse_version_reply(const http::Response &response)
{
    VersionReply out;
    json::Value body;
    if (!open_reply(response, &out.reached, &out.error, &body))
        return out;
    out.ok = true;
    out.version = body.text("shadowmount_version");
    if (body.has("capabilities") && body["capabilities"].is_array())
    {
        out.can_rescan = false;
        for (const json::Value &item : body["capabilities"].items())
            out.can_rescan = out.can_rescan || (item.is_string() && item.as_string() == "rescan");
    }
    if (!out.can_rescan)
    {
        out.ok = false;
        out.error = "this ShadowMount has no rescan";
    }
    return out;
}

ScanReply parse_scan_reply(const http::Response &response)
{
    ScanReply out;
    json::Value body;
    if (!open_reply(response, &out.reached, &out.error, &body))
        return out;
    out.ok = true;
    out.queued = body.flag("scan_queued", false);
    out.deferred = body.flag("scan_deferred", false);
    out.reason = body.text("scan_deferred_reason");
    return out;
}

GamesReply parse_games_reply(const http::Response &response)
{
    GamesReply out;
    json::Value body;
    if (!open_reply(response, &out.reached, &out.error, &body))
        return out;
    if (!body["games"].is_array())
    {
        out.error = "unexpected reply";
        return out;
    }
    out.ok = true;
    for (const json::Value &game : body["games"].items())
    {
        const std::string &id = game.text("title_id");
        if (!id.empty())
            out.title_ids.insert(id);
    }
    return out;
}

ShadowMountConfig ShadowMountClient::config() const
{
    if (!env_.read_config)
        return {};
    const std::string text = env_.read_config();
    return text.empty() ? ShadowMountConfig{} : parse_shadowmount_config(text);
}

http::Response ShadowMountClient::post(const char *route, const char *body, int timeout_ms,
                                        bool *disabled)
{
    http::Response none;
    *disabled = false;
    if (!env_.transport)
    {
        none.error = "ShadowMount API not available here";
        return none;
    }
    const ShadowMountConfig cfg = config();
    if (!cfg.enabled)
    {
        *disabled = true;
        none.error = "ShadowMount API is turned off";
        return none;
    }
    std::unique_ptr<http::Transport> transport =
        env_.transport(static_cast<std::uint16_t>(cfg.port), timeout_ms);
    if (!transport)
    {
        none.error = "no connection";
        return none;
    }
    http::Request request;
    request.method = "POST";
    request.path = route;
    request.body = body;
    request.max_bytes = 4u << 20;
    return transport->send(request);
}

VersionReply ShadowMountClient::version()
{
    bool disabled = false;
    return parse_version_reply(
        post("/api/v1/version", "{}", kShadowMountProbeTimeoutMs, &disabled));
}

ScanReply ShadowMountClient::scan()
{
    bool disabled = false;
    return parse_scan_reply(post("/api/v1/scan", "{\"reset_attempts\":false}",
                                 kShadowMountCallTimeoutMs, &disabled));
}

GamesReply ShadowMountClient::games()
{
    bool disabled = false;
    return parse_games_reply(post("/api/v1/games", "{\"include_size\":false}",
                                  kShadowMountCallTimeoutMs, &disabled));
}

} // namespace orbit
