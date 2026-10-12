// ps5-homebrew-ui - The app version, read from the packaged param.json, and which build of it this is.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/version.hpp"

#include "core/save_file.hpp"

#include <cctype>

namespace hui
{

namespace
{

bool digit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

// The text of "key": "..." (no escapes in what the build writes), or "" when it is not there.
std::string_view string_value(std::string_view json, std::string_view key)
{
    const std::size_t at = json.find(key);
    if (at == std::string_view::npos)
        return {};
    const std::size_t colon = json.find(':', at + key.size());
    if (colon == std::string_view::npos)
        return {};
    const std::size_t open = json.find('"', colon);
    if (open == std::string_view::npos)
        return {};
    const std::size_t close = json.find('"', open + 1);
    if (close == std::string_view::npos)
        return {};
    return json.substr(open + 1, close - open - 1);
}

} // namespace

std::string content_version(std::string_view param_json)
{
    const std::string_view value = string_value(param_json, "\"contentVersion\"");
    int parts[3];
    return parse_content_version(value, parts) ? std::string(value) : std::string();
}

std::string read_content_version(const std::string &path)
{
    std::string text;
    return save::read_file(path, &text) ? content_version(text) : std::string();
}

bool parse_content_version(std::string_view value, int out[3])
{
    // NN.NNN.NNN, as the build and release tooling require.
    if (value.size() != 10 || value[2] != '.' || value[6] != '.')
        return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (i != 2 && i != 6 && !digit(value[i]))
            return false;
    }
    out[0] = (value[0] - '0') * 10 + (value[1] - '0');
    out[1] = (value[3] - '0') * 100 + (value[4] - '0') * 10 + (value[5] - '0');
    out[2] = (value[7] - '0') * 100 + (value[8] - '0') * 10 + (value[9] - '0');
    return true;
}

bool parse_release_tag(std::string_view tag, int out[3])
{
    if (tag.size() < 6 || tag.size() > 12 || tag[0] != 'v')
        return false;
    int part = 0;
    int digits = 0;
    int value = 0;
    for (std::size_t i = 1; i <= tag.size(); ++i)
    {
        if (i < tag.size() && digit(tag[i]))
        {
            value = value * 10 + (tag[i] - '0');
            if (++digits > 3)
                return false;
        }
        else if ((i == tag.size() || tag[i] == '.') && digits > 0 && part < 3)
        {
            out[part++] = value;
            digits = 0;
            value = 0;
        }
        else
        {
            return false;
        }
    }
    return part == 3;
}

int compare_versions(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; ++i)
    {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

std::string release_version_text(std::string_view content_version)
{
    int v[3];
    if (!parse_content_version(content_version, v))
        return {};
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

BuildInfo parse_build_info(std::string_view json)
{
    BuildInfo info;
    const std::string_view build = string_value(json, "\"build\"");
    std::string_view channel = string_value(json, "\"channel\"");
    // YYYYMMDD-HHMMSS
    if (build.size() != 15 || build[8] != '-')
        return info;
    for (std::size_t i = 0; i < build.size(); ++i)
    {
        if (i != 8 && !digit(build[i]))
            return info;
    }
    // The channel is padded to a fixed width so the stamp can be changed in place.
    while (!channel.empty() && channel.back() == ' ')
        channel.remove_suffix(1);
    if (channel != "test" && channel != "release")
        return info;
    info.known = true;
    info.release = channel == "release";
    info.build = std::string(build);
    return info;
}

BuildInfo read_build_info(const std::string &path)
{
    std::string text;
    if (!save::read_file(path, &text, 1024))
        return {};
    return parse_build_info(text);
}

std::string version_label(std::string_view content_version, const BuildInfo &info)
{
    std::string label = release_version_text(content_version);
    if (!label.empty() && info.known && !info.release)
        label += " \xC2\xB7 test " + info.build;
    return label;
}

std::string build_tag(std::string_view content_version, const BuildInfo &info)
{
    std::string tag(content_version);
    if (info.known)
        tag += "+" + info.build;
    return tag;
}

} // namespace hui
