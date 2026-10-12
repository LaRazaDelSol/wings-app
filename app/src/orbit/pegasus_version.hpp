// Wings TV app - Is the running Pegasus DL version one Wings was tested with?
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link only: this compares the version Pegasus already reported on GET /api/health with the range
// below. It never fetches anything, downloads nothing and starts nothing.
#pragma once

#include <string>
#include <string_view>

namespace orbit
{

// The Pegasus DL versions Wings has been tested against (inclusive). The one place to widen it.
inline constexpr const char *kPegasusTestedFrom = "1.11.0";
inline constexpr const char *kPegasusTestedTo = "1.11.0";

enum class PegasusVersionFit
{
    unknown, // not connected, or text that is not a dotted version
    tested,  // inside the tested range
    untested // a readable version outside it (older or newer)
};

// Reads "1.11.0", "v1.11.0" or "1.11.0 release"; "1.11" reads as 1.11.0. False when no dotted
// number leads the text.
bool parse_pegasus_version(std::string_view text, int out[3]);
PegasusVersionFit pegasus_version_fit(std::string_view version);

// The plain note for the About row: empty when tested or unknown (translated).
std::string pegasus_version_note(PegasusVersionFit fit);
// "1.11.0" or "1.11.0 to 1.12.0" (translated).
std::string pegasus_tested_range_text();

} // namespace orbit
