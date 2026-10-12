// Wings TV app - What Wings does when a download finishes (a saved preference).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace orbit
{

enum class AfterDownload : std::uint8_t
{
    scan,   // ask ShadowMount (local API) to look for the new game (the default)
    nothing // leave it to ShadowMount's own schedule
};

inline const char *after_download_key(AfterDownload mode)
{
    return mode == AfterDownload::nothing ? "nothing" : "scan";
}

// False (and *mode untouched) for an unknown word.
inline bool parse_after_download(std::string_view text, AfterDownload *mode)
{
    if (text == "scan")
        *mode = AfterDownload::scan;
    else if (text == "nothing")
        *mode = AfterDownload::nothing;
    else
        return false;
    return true;
}

} // namespace orbit
