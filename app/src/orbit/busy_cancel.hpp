// Wings TV app - Circle on a waiting box (B-18): the hint every such box shows.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every box that waits on Pegasus DL or the network takes Circle and goes back
// at once; the request in flight is ignored when it answers (Store::cancel for
// Store lanes). This is the line those boxes show.

#pragma once

#include "orbit/i18n.hpp"

#include <string>

namespace orbit
{

// "Asking Pegasus DL...  (Circle: stop)"
inline std::string with_stop_hint(const char *text)
{
    std::string line = text;
    line += "  (";
    line += tr("Circle: stop");
    line += ")";
    return line;
}

} // namespace orbit
