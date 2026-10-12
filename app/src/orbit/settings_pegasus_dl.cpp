// Wings TV app - Settings > Pegasus DL > "Pegasus DL on this console" rows.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Status, an opt-in download and a hand-off (pegasus_manager.hpp). Nothing here starts anything by
// itself: every action is a row the player selects and, for the download and the send, confirms.

#include "orbit/screens.hpp"

#include "orbit/i18n.hpp"
#include "orbit/pegasus_manager.hpp"
#include "orbit/pegasus_version.hpp"
#include "orbit/theme.hpp"

#include <algorithm>
#include <cstdio>

namespace orbit
{

void SettingsScreen::build_pegasus_manager(std::vector<Row> &out, std::vector<Act> *acts) const
{
    const Snapshot &state = ctx_.store.state();
    const PegasusStatus pm = pegasus_manager().status();
    const bool running = state.online();
    const std::string version = running && state.have_info ? state.info.version : std::string();
    bool first = true;
    const auto add = [&](std::string title, std::string detail, std::string right, Act act = Act::none,
                         bool disabled = false, float progress = -1.0f)
    {
        Row row;
        if (first)
            row.section = tr("Pegasus DL on this console");
        first = false;
        row.accent = theme::accent_network;
        row.title = std::move(title);
        row.detail = std::move(detail);
        row.right = std::move(right);
        row.badge = tr("Wings");
        row.disabled = disabled;
        row.info = act == Act::none && !disabled;
        row.progress = progress;
        out.push_back(std::move(row));
        if (acts != nullptr)
            acts->push_back(act);
    };
    const auto megabytes = [](std::uint64_t bytes)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return std::string(buf);
    };

    if (!pm.attached)
    {
        add(tr("Pegasus DL on this console"), tr("Not available in this build"), "-", Act::none, true);
        return;
    }

    add(tr("Running version"), tr("What the Pegasus DL on this console reports"),
        running ? (version.empty() ? std::string(tr("Running")) : version) : std::string(tr("Not running")));

    std::string latest_detail;
    std::string latest_right;
    if (pm.have_latest)
    {
        latest_right = pm.latest.tag;
        latest_detail = tr("Published {date}", {{"date", pm.latest.published_at.substr(0, 10)}});
        if (pm.latest.downloadable)
            latest_detail += "  \xC2\xB7  " + megabytes(pm.latest.size);
    }
    else
    {
        latest_right = pm.check_error.empty() ? std::string(tr("Not checked")) : std::string(tr("Unavailable"));
        latest_detail = !pm.check_error.empty() ? pm.check_error
                        : pm.net_ok          ? std::string(tr("Press Check to read the author's release list"))
                                             : (pm.net_note.empty()
                                                    ? std::string(tr("Turn on cover downloads in Wings first"))
                                                    : pm.net_note);
    }
    add(tr("Latest official release"), latest_detail, latest_right);
    add(tr("Tested with Wings"), tr("Versions this Wings was checked against"), pegasus_tested_range_text());
    add(tr("Recommendation"),
        tr("Compares the running version with the latest official release and the tested range"),
        pegasus_advice_text(pegasus_advice(running, version, pm.have_latest ? pm.latest.tag : std::string())));

    // What the waiting screen does when Pegasus DL is not running (pegasus_offer.cpp).
    {
        const char *mode_text = prefs_.pegasus_start == PegasusStartMode::autoload ? tr("Auto-load")
                                : prefs_.pegasus_start == PegasusStartMode::manual ? tr("I load it myself")
                                                                                   : tr("Ask");
        add(tr("Pegasus DL start-up"),
            tr("Ask: offer to load it. Auto-load: once per launch, only when it is not running, from the "
               "verified download. Wings never starts it while it answers."),
            mode_text, Act::pegasus_startup);
    }

    // Layer 1: read the list (no file is downloaded).
    const bool busy = pm.busy();
    add(tr("Check for a new release"),
        pm.net_ok ? tr("Asks github.com for the list of official releases. Nothing is downloaded.")
                  : (pm.net_note.empty() ? std::string(tr("Needs the same network switch as cover art"))
                                         : pm.net_note),
        pm.phase == PegasusPhase::checking ? std::string(tr("Working...")) : std::string(tr("Check")),
        Act::pegasus_check, busy || !pm.net_ok);

    // Layer 2: the download, only on a confirmed press.
    if (pm.phase == PegasusPhase::downloading || pm.phase == PegasusPhase::verifying)
    {
        const float fraction =
            pm.latest.size > 0 ? std::min(1.0f, static_cast<float>(pm.got_bytes) / static_cast<float>(pm.latest.size))
                               : 0.0f;
        add(pm.phase == PegasusPhase::verifying ? tr("Verifying the checksum") : tr("Downloading Pegasus DL"),
            tr("{got} of {size}", {{"got", megabytes(pm.got_bytes)}, {"size", megabytes(pm.latest.size)}}),
            tr("Cancel"), Act::pegasus_cancel, pm.phase == PegasusPhase::verifying, fraction);
    }
    else if (pm.have_latest && pm.latest.downloadable && !pm.downloaded)
    {
        add(tr("Get Pegasus DL"),
            tr("From the author's official release page on github.com, only when you confirm. Wings never "
               "bundles it."),
            tr("Download {tag}", {{"tag", pm.latest.tag}}), Act::pegasus_download, busy || !pm.net_ok);
    }
    else if (pm.have_latest && !pm.latest.downloadable)
    {
        add(tr("Get Pegasus DL"), tr("That release has no file Wings can verify, so it is not offered"), "-",
            Act::none, true);
    }

    // Layer 3: where it is, and what to do with it.
    if (pm.downloaded)
    {
        add(tr("Downloaded and verified"), pm.path, pm.latest.tag);
        add(tr("What to do next"),
            tr("Wings sends it to the payload loader on this console from the waiting screen, only when you "
               "choose that and Pegasus DL is not running."),
            "-");
    }
    if (!pm.message.empty())
        add(pm.message_is_error ? tr("Last attempt failed") : tr("Last result"), pm.message, "-");
}

} // namespace orbit
