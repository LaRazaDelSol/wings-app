// Wings TV app - Pegasus DL version range check (see pegasus_version.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_version.hpp"

#include "orbit/i18n.hpp"

namespace orbit
{

bool parse_pegasus_version(std::string_view text, int out[3])
{
    std::size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V'))
        ++i;
    out[0] = out[1] = out[2] = 0;
    for (int part = 0; part < 3; ++part)
    {
        if (i >= text.size() || text[i] < '0' || text[i] > '9')
            return false;
        long value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
        {
            value = value * 10 + (text[i] - '0');
            if (value > 100000)
                return false;
            ++i;
        }
        out[part] = static_cast<int>(value);
        if (part < 2)
        {
            if (i >= text.size() || text[i] != '.')
                return part == 1; // "1.11" is 1.11.0; a bare "1" is not a version
            ++i;
        }
    }
    return true;
}

namespace
{
int compare(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
} // namespace

PegasusVersionFit pegasus_version_fit(std::string_view version)
{
    int v[3], from[3], to[3];
    if (!parse_pegasus_version(version, v))
        return PegasusVersionFit::unknown;
    if (!parse_pegasus_version(kPegasusTestedFrom, from) || !parse_pegasus_version(kPegasusTestedTo, to))
        return PegasusVersionFit::unknown;
    return compare(v, from) >= 0 && compare(v, to) <= 0 ? PegasusVersionFit::tested
                                                        : PegasusVersionFit::untested;
}

std::string pegasus_version_note(PegasusVersionFit fit)
{
    if (fit == PegasusVersionFit::untested)
        return tr("Untested version, things may not work.");
    return std::string();
}

std::string pegasus_tested_range_text()
{
    const std::string from = kPegasusTestedFrom, to = kPegasusTestedTo;
    if (from == to)
        return from;
    return tr("{from} to {to}", {{"from", from}, {"to", to}});
}

} // namespace orbit
