// Wings TV app - Failed-install wording (see install_hint.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/install_hint.hpp"

#include "orbit/i18n.hpp"

#include <cctype>

namespace orbit
{

bool looks_like_install_failure(std::string_view error)
{
    std::string lower(error);
    for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // "installable" contains "install"; both are listed so the intent stays readable.
    static const char *const kMarks[] = {"install", "installable", "multiple", "no pkg", "too many"};
    for (const char *mark : kMarks)
        if (lower.find(mark) != std::string::npos)
            return true;
    return false;
}

std::string install_failure_hint(std::string_view error)
{
    if (!looks_like_install_failure(error))
        return std::string();
    return tr("Pegasus could not decide what to install from this archive. Files may have been removed by "
              "Pegasus. Open Files to look, or install manually.");
}

} // namespace orbit
