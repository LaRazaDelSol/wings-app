// Wings TV app - A plain second line for downloads that failed while choosing what to install.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pegasus' own error text is always kept and shown; this only adds an explanation under it.
#pragma once

#include <string>
#include <string_view>

namespace orbit
{

// True when the error text looks like an extraction / installable-selection failure
// (case-insensitive: "install", "installable", "multiple", "no pkg", "too many").
bool looks_like_install_failure(std::string_view error);

// The extra line (translated), or "" when the error does not look like one.
std::string install_failure_hint(std::string_view error);

} // namespace orbit
