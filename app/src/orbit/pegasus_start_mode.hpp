// Wings TV app - How Wings treats Pegasus DL when it is not running (a saved preference).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace orbit
{

enum class PegasusStartMode : std::uint8_t
{
    ask,      // offer the three choices on the waiting screen (the default)
    autoload, // send the verified, cached file once per launch when Pegasus is unreachable
    manual    // the player loads it; Wings stops offering
};

inline const char *pegasus_start_mode_key(PegasusStartMode mode)
{
    switch (mode)
    {
    case PegasusStartMode::autoload:
        return "auto";
    case PegasusStartMode::manual:
        return "manual";
    default:
        return "ask";
    }
}

// False (and *mode untouched) for an unknown word.
inline bool parse_pegasus_start_mode(std::string_view text, PegasusStartMode *mode)
{
    if (text == "ask")
        *mode = PegasusStartMode::ask;
    else if (text == "auto")
        *mode = PegasusStartMode::autoload;
    else if (text == "manual")
        *mode = PegasusStartMode::manual;
    else
        return false;
    return true;
}

} // namespace orbit
