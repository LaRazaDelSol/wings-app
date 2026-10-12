// Wings TV app - The download folder: the one-time default and the first-download confirm.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/download_folder.hpp"

namespace orbit
{

FolderNeed folder_need(bool have_settings, const std::string &download_root,
                       bool already_defaulted)
{
    FolderNeed need;
    if (!have_settings)
    {
        need.root_unknown = true;
        return need;
    }
    need.set_default = download_root.empty() && !already_defaulted;
    return need;
}

bool needs_first_confirm(bool folder_confirmed, bool have_settings)
{
    return have_settings && !folder_confirmed;
}

FirstDownloadGate::Verdict FirstDownloadGate::check(bool folder_confirmed, bool have_settings) const
{
    if (answered_ || !needs_first_confirm(folder_confirmed, have_settings))
        return Verdict::proceed;
    return Verdict::ask;
}

} // namespace orbit
