// Wings TV app - Is a failed "start this link" worth asking again later?
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Download all parts" asks Pegasus for one part at a time. When a part fails,
// the series needs to know whether the failure may pass by itself (Pegasus busy
// or slow, a host error) or is a definite answer (not supported, not found, the
// link is gone). Pure, so it is tested on its own.

#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace orbit
{

// status: the HTTP status (0 when Pegasus was not reached); error: the text;
// unreachable: Result::unreachable() (connection refused, timed out, ...).
inline bool transient_link_error(int status, std::string_view error, bool unreachable)
{
    if (unreachable)
        return true;
    std::string text(error);
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&text](const char *word) { return text.find(word) != std::string::npos; };
    // Answers that will not change, whatever the status says.
    if (has("not found") || has("expired") || has("gone") || has("removed") || has("deleted") ||
        has("not supported") || has("unsupported") || has("blocked") || has("invalid"))
        return false;
    if (status >= 500 || status == 408 || status == 425 || status == 429)
        return true;
    if (has("timed out") || has("timeout") || has("busy") || has("try again") || has("temporarily") ||
        has("in progress") || has("connection refused") || has("connect failed") || has("no response"))
        return true;
    // "A capture session is already running" and the like: Pegasus does one at a time.
    if (has("already") && (has("capture") || has("session") || has("resolv") || has("running") || has("active")))
        return true;
    return false;
}

} // namespace orbit
