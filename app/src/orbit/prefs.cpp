// Wings TV app - Preferences kept on the console by Wings only.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/prefs.hpp"

#include "core/save_file.hpp"

namespace orbit
{

const char *const kFirmwareChoices[] = {"4.xx", "5.xx", "6.xx", "7.xx", "8.xx",
                                        "9.xx", "10.xx", "11.xx", "12.xx", "13.xx"};
const int kFirmwareChoiceCount = 10;
const char *const kRegionChoices[] = {"USA", "EUR", "JPN", "ASIA", "HK", "KOR"};
const int kRegionChoiceCount = 6;
const char *const kFormatChoices[] = {"exFAT", "FPKG", "PKG", "Dump"};
const int kFormatChoiceCount = 4;

namespace
{

bool in_list(std::string_view value, const char *const *list, int count)
{
    if (value.empty())
        return true;
    for (int i = 0; i < count; ++i)
    {
        if (value == list[i])
            return true;
    }
    return false;
}

// Keeps a stored URL to one short line.
bool plain_line(std::string_view value)
{
    if (value.size() > 300)
        return false;
    for (const char c : value)
    {
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
    }
    return true;
}

} // namespace

bool WingsLocalPrefs::operator==(const WingsLocalPrefs &other) const
{
    return firmware == other.firmware && region == other.region && format == other.format &&
           prefer_debrid == other.prefer_debrid &&
           popularity_enabled == other.popularity_enabled && popularity_url == other.popularity_url &&
           tile_details == other.tile_details && pegasus_start == other.pegasus_start &&
           after_download == other.after_download;
}

bool valid_firmware(std::string_view value)
{
    return in_list(value, kFirmwareChoices, kFirmwareChoiceCount);
}

bool valid_region(std::string_view value)
{
    return in_list(value, kRegionChoices, kRegionChoiceCount);
}

bool valid_format(std::string_view value)
{
    return in_list(value, kFormatChoices, kFormatChoiceCount);
}

std::string encode_local_prefs(const WingsLocalPrefs &prefs)
{
    std::string out;
    out += "firmware=" + prefs.firmware + "\n";
    out += "region=" + prefs.region + "\n";
    out += "format=" + prefs.format + "\n";
    out += std::string("prefer_debrid=") + (prefs.prefer_debrid ? "1" : "0") + "\n";
    out += std::string("popularity=") + (prefs.popularity_enabled ? "1" : "0") + "\n";
    out += "popularity_url=" + prefs.popularity_url + "\n";
    out += std::string("tile_details=") + tile_details_key(prefs.tile_details) + "\n";
    out += std::string("pegasus_start=") + pegasus_start_mode_key(prefs.pegasus_start) + "\n";
    out += std::string("after_download=") + after_download_key(prefs.after_download) + "\n";
    return out;
}

bool decode_local_prefs(std::string_view payload, WingsLocalPrefs *out)
{
    if (payload.empty())
        return false;
    WingsLocalPrefs prefs;
    std::size_t at = 0;
    while (at < payload.size())
    {
        std::size_t end = payload.find('\n', at);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string_view line = payload.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        if (key == "firmware" && valid_firmware(value))
            prefs.firmware = std::string(value);
        else if (key == "region" && valid_region(value))
            prefs.region = std::string(value);
        else if (key == "format" && valid_format(value))
            prefs.format = std::string(value);
        else if (key == "prefer_debrid")
            prefs.prefer_debrid = value != "0";
        else if (key == "popularity")
            prefs.popularity_enabled = value == "1";
        else if (key == "popularity_url" && plain_line(value))
            prefs.popularity_url = std::string(value);
        else if (key == "tile_details")
            parse_tile_details(value, &prefs.tile_details);
        else if (key == "pegasus_start")
            parse_pegasus_start_mode(value, &prefs.pegasus_start);
        else if (key == "after_download")
            parse_after_download(value, &prefs.after_download);
    }
    *out = prefs;
    return true;
}

bool load_local_prefs(const std::vector<std::string> &paths, WingsLocalPrefs *out)
{
    for (const std::string &path : paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 16384))
            continue;
        const hui::save::Decoded decoded = hui::save::decode(hui::save::Kind::prefs, data);
        if (decoded.ok && decoded.version == 1 && decode_local_prefs(decoded.payload, out))
            return true;
    }
    return false;
}

int save_local_prefs(const std::vector<std::string> &paths, const WingsLocalPrefs &prefs)
{
    const std::string data = hui::save::encode(hui::save::Kind::prefs, 1, encode_local_prefs(prefs));
    int written = 0;
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            ++written;
    }
    return written;
}

} // namespace orbit
