// Wings TV app - fetching and placing a newer Wings package (see wings_install.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/wings_install.hpp"

#include "core/save_file.hpp"
#include "core/version.hpp"
#include "orbit/i18n.hpp"
#include "orbit/wings_helper_client.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace orbit
{

namespace
{

std::string join(const std::string &dir, const char *name)
{
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string megabytes(std::uint64_t bytes)
{
    char out[32];
    std::snprintf(out, sizeof out, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return out;
}

double steady_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// "key=value" lines: the value of `key`, "" when absent.
std::string field(const std::string &text, const char *key)
{
    const std::string want = std::string(key) + "=";
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        if (text.compare(pos, want.size(), want) == 0)
        {
            std::string value = text.substr(pos + want.size(), nl - pos - want.size());
            while (!value.empty() && value.back() == '\r')
                value.pop_back();
            return value;
        }
        pos = nl + 1;
    }
    return std::string();
}

} // namespace

// ---- pure rules ----

bool wings_same_release(const std::string &helper_version, const std::string &tag)
{
    return !helper_version.empty() && (helper_version == tag || "v" + helper_version == tag);
}

WingsHelperSituation wings_helper_situation(const WingsHelperStatus &s, const std::string &tag)
{
    if (s.busy())
        return wings_same_release(s.version, tag) ? WingsHelperSituation::following : WingsHelperSituation::other_job;
    // An install that finished after Wings stopped following it (the record ties the file to this release).
    if (s.target == WingsHelperTarget::wings && wings_same_release(s.installed_version, tag))
        return WingsHelperSituation::already_placed;
    if (s.old_kept)
        return WingsHelperSituation::old_waiting;
    // A checked download of this release waits (also after the helper restarted): go straight to Install.
    if (s.phase == WingsHelperPhase::downloaded && wings_same_release(s.version, tag))
        return WingsHelperSituation::downloaded;
    return WingsHelperSituation::ready;
}

std::string encode_wings_placed(const WingsPlacedNote &n)
{
    const char *by = n.by == WingsPlacedBy::helper ? "helper" : n.by == WingsPlacedBy::pegasus ? "pegasus" : "direct";
    return "tag=" + n.tag + "\nby=" + by + "\n";
}

bool decode_wings_placed(const std::string &text, WingsPlacedNote *out)
{
    WingsPlacedNote n;
    n.tag = field(text, "tag");
    int v[3];
    if (!hui::parse_release_tag(n.tag, v))
        return false;
    const std::string by = field(text, "by");
    if (by == "helper")
        n.by = WingsPlacedBy::helper;
    else if (by == "pegasus")
        n.by = WingsPlacedBy::pegasus;
    else if (by == "direct")
        n.by = WingsPlacedBy::direct;
    else
        return false;
    *out = n;
    return true;
}

void WingsRate::reset()
{
    samples_.clear();
}

void WingsRate::sample(double t_s, std::uint64_t bytes)
{
    if (!samples_.empty() && (bytes < samples_.back().bytes || t_s < samples_.back().t))
        samples_.clear();
    if (!samples_.empty() && t_s == samples_.back().t)
        samples_.back().bytes = bytes; // the same instant: keep the newest count
    else
        samples_.push_back({t_s, bytes});
    // Keep one sample at or before the window start so the window is always fully covered.
    while (samples_.size() > 2 && samples_[1].t <= t_s - kWingsRateWindowS)
        samples_.erase(samples_.begin());
}

double WingsRate::bytes_per_s() const
{
    if (samples_.size() < 2)
        return 0.0;
    const Sample &a = samples_.front();
    const Sample &b = samples_.back();
    const double dt = b.t - a.t;
    if (dt < 0.5)
        return 0.0;
    return static_cast<double>(b.bytes - a.bytes) / dt;
}

std::int64_t WingsRate::eta_s(std::uint64_t total) const
{
    if (samples_.empty())
        return -1;
    const std::uint64_t got = samples_.back().bytes;
    if (total > 0 && got >= total)
        return 0;
    const double speed = bytes_per_s();
    if (total == 0 || speed <= 0.0)
        return -1;
    const double left = static_cast<double>(total - got) / speed;
    return static_cast<std::int64_t>(std::ceil(left)); // round up: "0 s left" only when done
}

// ---- the installer ----

WingsInstaller &wings_installer()
{
    static WingsInstaller installer;
    return installer;
}

WingsInstaller::~WingsInstaller()
{
    cancel_ = true;
    thread_.join();
}

void WingsInstaller::say(const std::string &line) const
{
    if (config_.log)
        config_.log(line);
}

bool WingsInstaller::attached() const
{
    Lock l(mutex_);
    return attached_;
}

WingsInstallStatus WingsInstaller::status() const
{
    Lock l(mutex_);
    return status_;
}

void WingsInstaller::set_phase(WingsInstallPhase phase, std::string message)
{
    Lock l(mutex_);
    status_.phase = phase;
    status_.message = std::move(message);
}

void WingsInstaller::attach(WingsInstallConfig config)
{
    cancel_ = true;
    thread_.join();
    cancel_ = false;
    bool tidy = false;
    {
        Lock l(mutex_);
        config_ = std::move(config);
        status_ = WingsInstallStatus();
        release_ = WingsRelease();
        noted_tag_.clear();
        attached_ = true;
        restore_placed_locked(&tidy);
    }
    // After a successful update the old package kept next to the new one is no longer needed (until the new
    // version runs it is the way back). Only the helper can remove it; it is asked in the background.
    if (tidy && config_.helper != nullptr)
        start([this] { run_tidy(); });
}

void WingsInstaller::run_tidy()
{
    if (config_.helper == nullptr)
        return;
    WingsHelperStatus hs;
    std::string why;
    if (config_.helper->ensure_running(&hs, &why) && config_.helper->tidy(config_.running))
        say("[WINGS-UPDATE] asked the helper to remove the old package kept from the update");
    else
        say("[WINGS-UPDATE] the old package was not tidied: " + why);
}

// Wings was closed and opened again after placing a package, but the new package is not the one running yet, so
// the old version still runs: the flow stays at "installed" (it used to start again at "Download update").
void WingsInstaller::restore_placed_locked(bool *tidy)
{
    const std::string note_path = join(config_.note_dir, "placed.v1");
    std::string text;
    WingsPlacedNote note;
    if (!hui::save::read_file(note_path, &text, 4096) || !decode_wings_placed(text, &note))
        return;
    const WingsAdvice advice = wings_advice(config_.running, note.tag);
    if (advice == WingsAdvice::unreadable)
        return; // cannot tell; the note is kept
    if (advice != WingsAdvice::newer)
    {
        std::remove(note_path.c_str()); // that version (or a later one) runs: the update is finished
        *tidy = true;
        return;
    }
    status_.phase = WingsInstallPhase::placed;
    status_.tag = note.tag;
    status_.placed_by = note.by;
    status_.old_kept = true;
    status_.target = WingsTarget::wings_copy;
    status_.got_bytes = 0;
    status_.total_bytes = 0;
    status_.message = tr("Close Wings and open it again to finish.");
    noted_tag_ = note.tag;
}

void WingsInstaller::note_release(const WingsRelease &release)
{
    Lock l(mutex_);
    if (!attached_ || status_.busy())
        return;
    noted_tag_ = release.tag;
}

bool WingsInstaller::start(std::function<void()> job)
{
    thread_.join();
    if (!thread_.start(std::move(job), 256u << 10))
    {
        set_phase(WingsInstallPhase::failed, tr("Could not start the background job"));
        return false;
    }
    return true;
}

bool WingsInstaller::start_download(const WingsRelease &release)
{
    {
        Lock l(mutex_);
        if (!attached_ || status_.busy() || config_.downloader == nullptr || !release.asset.downloadable)
            return false;
        if (status_.phase == WingsInstallPhase::placed)
            return false;
        status_.phase = WingsInstallPhase::checking;
        status_.message = tr("Reading the published checksum...");
        status_.tag = release.tag;
        status_.got_bytes = 0;
        status_.total_bytes = release.asset.size;
        status_.needs_replace_confirm = false;
        status_.install_tried = false;
        cancel_ = false;
    }
    const WingsRelease copy = release;
    return start([this, copy] { run_download(copy); });
}

bool WingsInstaller::install(bool replace_own_confirmed)
{
    {
        Lock l(mutex_);
        if (!attached_ || status_.busy() || status_.phase != WingsInstallPhase::verified)
            return false;
        status_.phase = WingsInstallPhase::downloading;
        status_.message = tr("Downloading the update...");
        status_.got_bytes = 0;
        status_.install_tried = true;
        cancel_ = false; // a stale stop from before the confirm does not cancel this install
    }
    thread_.join();
    return start([this, replace_own_confirmed] { run_install(replace_own_confirmed); });
}

void WingsInstaller::cancel()
{
    cancel_ = true;
}

void WingsInstaller::wait_idle()
{
    thread_.join();
}

namespace
{

WingsTarget target_from(WingsHelperTarget t)
{
    switch (t)
    {
    case WingsHelperTarget::none:
        return WingsTarget::none;
    case WingsHelperTarget::wings:
        return WingsTarget::wings_copy;
    case WingsHelperTarget::manual:
        return WingsTarget::own_copy;
    case WingsHelperTarget::folder:
        return WingsTarget::folder;
    case WingsHelperTarget::blocked:
        return WingsTarget::blocked;
    case WingsHelperTarget::unknown:
    default:
        return WingsTarget::unknown;
    }
}

std::string manual_message(const std::string &why)
{
    std::string text = tr("Wings cannot install this update itself right now. The steps below do it by hand.");
    if (!why.empty())
        text += " (" + why + ")";
    return text;
}

} // namespace

void WingsInstaller::run_download(const WingsRelease &release)
{
    const auto done = [&](WingsInstallPhase phase, std::string message)
    {
        say("[WINGS-UPDATE] " + message);
        set_phase(phase, std::move(message));
    };
    {
        Lock l(mutex_);
        release_ = release;
        status_.tag = release.tag;
        status_.total_bytes = release.asset.size;
        status_.got_bytes = 0;
        status_.placed_by = WingsPlacedBy::none;
        status_.old_kept = false;
        status_.needs_replace_confirm = false;
        status_.install_tried = false;
        status_.target = WingsTarget::unknown;
        status_.package_url = release.asset.url;
    }
    // Never fetch a package that is not newer than the running one (the release may have changed since the check).
    if (wings_advice(config_.running, release.tag) != WingsAdvice::newer)
    {
        done(WingsInstallPhase::failed,
             tr("This release is not newer than the Wings you are running, so it was not downloaded. Nothing was "
                "changed."));
        return;
    }
    // The first address is the official one for this tag, built here: nothing from a reply is followed.
    const std::string url = wings_asset_url(release.tag);
    const std::string digest_url = wings_digest_url(release.tag);
    if (!release.asset.downloadable || url.empty() || url != release.asset.url ||
        !wings_url_allowed(url, false, release.tag) || digest_url != release.asset.digest_url ||
        !wings_url_allowed(digest_url, false, release.tag))
    {
        done(WingsInstallPhase::failed, tr("This release has no package Wings can verify. Nothing was changed."));
        return;
    }
    if (config_.downloader == nullptr)
    {
        done(WingsInstallPhase::failed, tr("Not available in this build"));
        return;
    }
    std::string why;
    if (!config_.downloader->ready(&why))
    {
        done(WingsInstallPhase::failed, why.empty() ? std::string(tr("The network is off. Turn on cover "
                                                                    "downloads first."))
                                                    : why);
        return;
    }
    if (cancel_)
    {
        done(WingsInstallPhase::idle, tr("Download cancelled. Nothing was changed."));
        return;
    }

    // 1. The published checksum text must name our file and agree with the API's digest.
    {
        std::string body;
        StreamHooks hooks;
        hooks.redirect_ok = [](const std::string &to) { return wings_url_allowed(to, true, {}); };
        hooks.cancel = [this] { return cancel_.load(); };
        int status = 0;
        const std::string error = config_.downloader->get_stream(
            digest_url, 4096, hooks,
            [&body](const std::uint8_t *data, std::size_t n)
            {
                if (body.size() + n > 4096)
                    return false;
                body.append(reinterpret_cast<const char *>(data), n);
                return true;
            },
            &status);
        if (cancel_)
        {
            done(WingsInstallPhase::idle, tr("Download cancelled. Nothing was changed."));
            return;
        }
        std::string hex;
        if (!error.empty() || status < 200 || status > 299 || !parse_wings_checksum_text(body, &hex))
        {
            done(WingsInstallPhase::failed,
                 tr("The published checksum for this release could not be read. Nothing was downloaded."));
            return;
        }
        if (hex != release.asset.sha256)
        {
            done(WingsInstallPhase::failed, wings_asset_issue_text(WingsAssetIssue::digest_differs));
            return;
        }
    }

    // 2. The helper, and where the package would go (a folder copy is never installed beside).
    if (config_.helper == nullptr)
    {
        done(WingsInstallPhase::manual, manual_message(std::string()));
        return;
    }
    WingsHelperStatus hs;
    const bool up = config_.helper->ensure_running(&hs, &why);
    if (!config_.helper->stopped_stale.empty())
        say("[WINGS-UPDATE] replaced a helper from another Wings package: " + config_.helper->stopped_stale);
    if (!up)
    {
        done(WingsInstallPhase::manual, manual_message(why));
        return;
    }
    if (!settle_helper(release, &hs))
        return;
    const WingsTarget target = target_from(hs.target);
    {
        Lock l(mutex_);
        status_.target = target;
    }
    if (target == WingsTarget::folder)
    {
        done(WingsInstallPhase::blocked,
             tr("Wings is installed as a folder (/data/homebrew/PPSA99178). Wings does not install a package "
                "beside it. Update the folder yourself, or remove it and use Download update again."));
        return;
    }
    if (target == WingsTarget::blocked)
    {
        done(WingsInstallPhase::blocked,
             tr("Something other than the package is at /data/homebrew/PPSA99178.ffpkg. Move it away first. "
                "Nothing was changed."));
        return;
    }
    {
        Lock l(mutex_);
        status_.needs_replace_confirm = target == WingsTarget::own_copy;
    }
    if (wings_helper_situation(hs, release.tag) == WingsHelperSituation::downloaded)
    {
        // Downloaded and checked earlier (the helper keeps it across restarts): straight to Install.
        {
            Lock l(mutex_);
            status_.total_bytes = status_.got_bytes = hs.size;
        }
        done(WingsInstallPhase::verified, tr("Downloaded and checked. Install update puts it in place."));
        return;
    }
    // 3. The helper downloads the package and checks it (size, SHA-256, image). Nothing is installed yet.
    if (ask_helper(release, false, false, &hs))
        follow(release, false);
}

void WingsInstaller::run_install(bool replace_own_confirmed)
{
    const auto done = [&](WingsInstallPhase phase, std::string message)
    {
        say("[WINGS-UPDATE] " + message);
        set_phase(phase, std::move(message));
    };
    WingsRelease release;
    WingsTarget target = WingsTarget::unknown;
    {
        Lock l(mutex_);
        release = release_;
        target = status_.target;
        status_.install_tried = true;
    }
    if (target == WingsTarget::own_copy && !replace_own_confirmed)
    {
        done(WingsInstallPhase::verified,
             tr("The package in /data/homebrew is a copy you placed yourself. Confirm to replace it."));
        return;
    }
    if (wings_advice(config_.running, release.tag) != WingsAdvice::newer)
    {
        done(WingsInstallPhase::failed,
             tr("This release is not newer than the Wings you are running, so it was not downloaded. Nothing was "
                "changed."));
        return;
    }
    if (config_.helper == nullptr)
    {
        done(WingsInstallPhase::manual, manual_message(std::string()));
        return;
    }
    // The helper ends itself when idle for a while: the confirm may have taken longer.
    WingsHelperStatus hs;
    std::string why;
    const bool up = config_.helper->ensure_running(&hs, &why);
    if (!config_.helper->stopped_stale.empty())
        say("[WINGS-UPDATE] replaced a helper from another Wings package: " + config_.helper->stopped_stale);
    if (!up)
    {
        done(WingsInstallPhase::manual, manual_message(why));
        return;
    }
    if (ask_helper(release, true, replace_own_confirmed, &hs))
        follow(release, true);
}

// Asks the helper to download (installing false) or to install the downloaded release. True: it started.
bool WingsInstaller::ask_helper(const WingsRelease &release, bool installing, bool replace, WingsHelperStatus *hsp)
{
    const auto done = [&](WingsInstallPhase phase, std::string message)
    {
        say("[WINGS-UPDATE] " + message);
        set_phase(phase, std::move(message));
    };
    WingsHelperStatus &hs = *hsp;
    std::string why;
    WingsHelperInstall request;
    request.version = release.tag;
    request.url = release.asset.url;
    request.sha256 = release.asset.sha256;
    request.size = release.asset.size;
    request.replace = replace;
    request.running = config_.running;
    // A refusal for something the helper can sort out (a job running, the old file) is looked at and asked again.
    for (int attempt = 0;; ++attempt)
    {
        if (!settle_helper(release, &hs))
            return false;
        const WingsHelperClient::Answer answer =
            installing ? config_.helper->install(request, &why) : config_.helper->download(request, &why);
        if (answer == WingsHelperClient::Answer::accepted)
            return true;
        if ((answer == WingsHelperClient::Answer::busy || answer == WingsHelperClient::Answer::old_waiting) &&
            attempt < 2)
        {
            say(std::string("[WINGS-UPDATE] the helper answered ") +
                (answer == WingsHelperClient::Answer::busy ? "busy" : "old_waiting"));
            if (!config_.helper->status(&hs))
            {
                done(WingsInstallPhase::failed,
                     tr("The Wings helper stopped answering before the update started. Try again."));
                return false;
            }
            continue;
        }
        switch (answer)
        {
        case WingsHelperClient::Answer::busy:
            done(WingsInstallPhase::failed, tr("The Wings helper is still busy. Try again in a minute."));
            return false;
        case WingsHelperClient::Answer::old_waiting:
            done(WingsInstallPhase::failed, tr("An earlier update still waits for Wings to restart. Close Wings and "
                                               "open it again, then try again."));
            return false;
        case WingsHelperClient::Answer::not_downloaded:
            {
                Lock l(mutex_);
                status_.install_tried = false; // the failure belongs to the Download step
            }
            done(WingsInstallPhase::failed, tr("Download the update first. Nothing was installed."));
            return false;
        case WingsHelperClient::Answer::refused:
            done(WingsInstallPhase::failed,
                 tr("The Wings helper refused the update: {why}. Nothing was changed.", {{"why", why}}));
            return false;
        case WingsHelperClient::Answer::unreachable:
        default:
            done(WingsInstallPhase::manual, manual_message(std::string()));
            return false;
        }
    }
}

void WingsInstaller::log_helper(const char *when, const WingsHelperStatus &s) const
{
    say(std::string("[WINGS-UPDATE] helper ") + when + ": " + describe_wings_helper_status(s));
}

void WingsInstaller::already_placed(const WingsRelease &release, const WingsHelperStatus &hs)
{
    record_placed(release.tag);
    {
        Lock l(mutex_);
        status_.placed_by = WingsPlacedBy::helper;
        status_.old_kept = hs.old_kept;
        status_.target = WingsTarget::wings_copy;
        status_.install_tried = true;
        status_.got_bytes = status_.total_bytes;
    }
    const std::string message = tr("This update is already installed. Close Wings and open it again to finish.");
    say("[WINGS-UPDATE] " + message);
    set_phase(WingsInstallPhase::placed, message);
}

bool WingsInstaller::wait_for_helper(WingsHelperStatus *hs)
{
    const auto pause = [this]
    {
        if (config_.sleep_ms)
            config_.sleep_ms(config_.poll_ms);
        else
            ::usleep(static_cast<useconds_t>(config_.poll_ms) * 1000u);
    };
    const WingsInstallStatus before = status();
    int misses = 0;
    while (hs->busy())
    {
        if (cancel_)
        {
            set_phase(WingsInstallPhase::idle, tr("Cancelled. Nothing was changed."));
            return false;
        }
        {
            Lock l(mutex_);
            status_.phase = WingsInstallPhase::downloading;
            status_.total_bytes = hs->size;
            status_.got_bytes = std::min(hs->received, hs->size);
            status_.message = tr("The Wings helper is finishing another update. Waiting for it...");
        }
        pause();
        WingsHelperStatus s;
        if (!config_.helper->status(&s))
        {
            if (++misses >= config_.lost_polls)
            {
                const std::string message = tr("The Wings helper stopped answering. Try again.");
                say("[WINGS-UPDATE] " + message);
                set_phase(WingsInstallPhase::failed, message);
                return false;
            }
            continue;
        }
        misses = 0;
        *hs = s;
    }
    log_helper("after waiting", *hs);
    {
        Lock l(mutex_);
        status_.phase = before.phase;
        status_.message = before.message;
        status_.got_bytes = before.got_bytes;
        status_.total_bytes = before.total_bytes;
    }
    return true;
}

bool WingsInstaller::settle_helper(const WingsRelease &release, WingsHelperStatus *hs)
{
    bool tidied = false;
    for (;;)
    {
        log_helper("status", *hs);
        switch (wings_helper_situation(*hs, release.tag))
        {
        case WingsHelperSituation::ready:
        case WingsHelperSituation::downloaded:
            return true;
        case WingsHelperSituation::following:
            // An install of this release is already under way (Wings was reopened): follow it.
            {
                Lock l(mutex_);
                status_.install_tried = true;
            }
            follow(release, hs->phase == WingsHelperPhase::placing);
            return false;
        case WingsHelperSituation::other_job:
            if (!wait_for_helper(hs))
                return false;
            break;
        case WingsHelperSituation::already_placed:
            already_placed(release, *hs);
            return false;
        case WingsHelperSituation::old_waiting:
        {
            // The file kept by an earlier update. The helper removes it once the version it placed runs, or
            // when that file was replaced by hand since; otherwise it is still the way back and stays.
            WingsHelperStatus again;
            if (!tidied && config_.helper->tidy(config_.running) && config_.helper->status(&again))
            {
                say("[WINGS-UPDATE] asked the helper to remove the old package kept from an earlier update");
                tidied = true;
                *hs = again;
                break;
            }
            const std::string message = tr("An earlier update still waits for Wings to restart. Close Wings and "
                                           "open it again, then try again.");
            say("[WINGS-UPDATE] " + message);
            set_phase(WingsInstallPhase::failed, message);
            return false;
        }
        }
    }
}

// Follows the helper's status until the job ends: downloaded (download), placed (install), error, or cancelled.
void WingsInstaller::follow(const WingsRelease &release, bool installing)
{
    const auto done = [&](WingsInstallPhase phase, std::string message)
    {
        say("[WINGS-UPDATE] " + message);
        set_phase(phase, std::move(message));
    };
    const auto pause = [this]
    {
        if (config_.sleep_ms)
            config_.sleep_ms(config_.poll_ms);
        else
            ::usleep(static_cast<useconds_t>(config_.poll_ms) * 1000u);
    };
    {
        Lock l(mutex_);
        rate_.reset();
        status_.speed_bps = 0.0;
        status_.eta_s = -1;
    }
    bool asked_cancel = false;
    int misses = 0;
    for (;;)
    {
        if (cancel_ && !asked_cancel)
        {
            config_.helper->cancel(); // ignored by the helper once it renames files: placing always finishes
            asked_cancel = true;
        }
        WingsHelperStatus s;
        if (!config_.helper->status(&s))
        {
            if (++misses >= config_.lost_polls)
            {
                done(WingsInstallPhase::failed,
                     tr("The Wings helper stopped answering before the update finished. Try again."));
                return;
            }
            pause();
            continue;
        }
        misses = 0;
        if (!s.version.empty() && !wings_same_release(s.version, release.tag))
        {
            // Something else asked the helper for another release (or another helper took over the port).
            log_helper("switched release", s);
            done(WingsInstallPhase::failed,
                 tr("The Wings helper started a different update. Wait a minute, then try again."));
            return;
        }
        if (installing && s.phase == WingsHelperPhase::verifying)
            s.phase = WingsHelperPhase::placing; // the install job checks the downloaded file again, then places it
        switch (s.phase)
        {
        case WingsHelperPhase::downloaded:
            if (installing)
            {
                done(WingsInstallPhase::failed,
                     tr("The Wings helper stopped before the update was installed. Try again."));
                return;
            }
            {
                Lock l(mutex_);
                status_.got_bytes = status_.total_bytes;
            }
            done(WingsInstallPhase::verified, tr("Downloaded and checked. Install update puts it in place."));
            return;
        case WingsHelperPhase::downloading:
        case WingsHelperPhase::verifying:
        {
            const double t = config_.clock ? config_.clock() : steady_seconds();
            Lock l(mutex_);
            status_.phase = WingsInstallPhase::downloading;
            status_.total_bytes = s.size > 0 ? s.size : release.asset.size;
            status_.got_bytes = s.phase == WingsHelperPhase::verifying ? status_.total_bytes
                                                                       : std::min(s.received, status_.total_bytes);
            rate_.sample(t, status_.got_bytes);
            status_.speed_bps = rate_.bytes_per_s();
            status_.eta_s = rate_.eta_s(status_.total_bytes);
            status_.message = tr("Downloading the update...");
            break;
        }
        case WingsHelperPhase::placing:
            set_phase(WingsInstallPhase::placing, tr("Installing the update..."));
            break;
        case WingsHelperPhase::placed:
        {
            record_placed(release.tag);
            {
                Lock l(mutex_);
                status_.placed_by = WingsPlacedBy::helper;
                status_.old_kept = s.old_kept;
                status_.target = WingsTarget::wings_copy;
                status_.got_bytes = status_.total_bytes;
            }
            done(WingsInstallPhase::placed, tr("Installed. Close Wings and open it again to use the new version."));
            return;
        }
        case WingsHelperPhase::error:
            if ((asked_cancel && !installing) || s.error == "cancelled")
                done(WingsInstallPhase::idle, tr("Download cancelled. Nothing was changed."));
            else
                done(WingsInstallPhase::failed,
                     tr("The update did not finish: {why}. Nothing was changed.",
                        {{"why", s.error.empty() ? std::string(tr("no reason given")) : s.error}}));
            return;
        case WingsHelperPhase::idle:
        default:
            if (asked_cancel)
                done(WingsInstallPhase::idle, tr("Download cancelled. Nothing was changed."));
            else
                done(WingsInstallPhase::failed,
                     tr("The Wings helper stopped before the update finished. Nothing was changed."));
            return;
        }
        pause();
    }
}

void WingsInstaller::record_placed(const std::string &tag)
{
    // The note that keeps "installed" across a Wings restart until the new version runs.
    WingsPlacedNote note;
    note.tag = tag;
    note.by = WingsPlacedBy::helper;
    hui::save::ensure_directory(config_.note_dir);
    const std::string error = hui::save::write_atomic(join(config_.note_dir, "placed.v1"), encode_wings_placed(note));
    if (!error.empty())
        say("[WINGS-UPDATE] the installed note was not saved: " + error);
}

// ---- words ----

std::vector<WingsInstallStep> wings_install_manual_steps(const WingsInstallStatus &s)
{
    std::vector<WingsInstallStep> steps;
    if (s.phase == WingsInstallPhase::blocked && s.target == WingsTarget::folder)
    {
        steps.push_back({tr("Update the folder yourself"),
                         tr("Replace the files in /data/homebrew/PPSA99178 with the new release")});
        steps.push_back({tr("Or switch to the package file"),
                         tr("Remove that folder, then use Download update and Install update again")});
        steps.push_back({tr("Close Wings and open it again"), tr("So the new version runs")});
        return steps;
    }
    steps.push_back({tr("Download the package"),
                     s.package_url.empty() ? std::string(tr("PPSA99178.ffpkg from the Wings release page on GitHub"))
                                           : s.package_url});
    steps.push_back({tr("Put it in /data/homebrew/"),
                     tr("Over the old one (Files tab: Copy, then Replace)")});
    steps.push_back({tr("Remove the staged copies"), tr("Delete the copies ShadowMountPlus made of the old package")});
    steps.push_back({tr("Close Wings and open it again"), tr("So the new version runs")});
    return steps;
}

std::string wings_install_confirm_text(const WingsInstallStatus &s, const std::string &version_text)
{
    std::string text = tr("Wings will download the {version} package, check it and put it in /data/homebrew, "
                          "over the old one. "
                          "The old file is kept next to it as PPSA99178.ffpkg.old until the new version runs.",
                          {{"version", version_text}});
    if (s.target == WingsTarget::own_copy)
        text += " " + std::string(tr("The current package is a copy you placed yourself; this replaces it."));
    text += " " + std::string(tr("It takes a few seconds. Then close Wings and open it again."));
    return text;
}

WingsInstallText wings_install_text(const WingsInstallStatus &s)
{
    WingsInstallText out;
    switch (s.phase)
    {
    case WingsInstallPhase::idle:
        out.value = tr("Download");
        out.detail = s.message.empty() ? std::string(tr("Checks the release and gets the installer ready. "
                                                        "Nothing is installed yet."))
                                       : s.message;
        break;
    case WingsInstallPhase::checking:
        out.value = tr("Cancel");
        out.detail = tr("Reading the published checksum...");
        break;
    case WingsInstallPhase::downloading:
        out.value = tr("Cancel");
        out.detail = tr("{got} of {size}", {{"got", megabytes(s.got_bytes)}, {"size", megabytes(s.total_bytes)}});
        break;
    case WingsInstallPhase::verified:
        out.value = tr("Install");
        out.detail = s.message;
        break;
    case WingsInstallPhase::placing:
        out.value = tr("Working...");
        out.detail = tr("Placing the update...");
        break;
    case WingsInstallPhase::placed:
        out.value = tr("Quit to finish");
        out.detail = s.message;
        break;
    case WingsInstallPhase::manual:
    case WingsInstallPhase::blocked:
        out.value = tr("Steps");
        out.detail = s.message;
        break;
    case WingsInstallPhase::failed:
        out.value = tr("Download");
        out.detail = s.message;
        break;
    }
    return out;
}

// ---- the update flow as the screen draws it ----

std::string wings_speed_text(double bytes_per_s)
{
    if (!(bytes_per_s > 0.0))
        return std::string();
    char num[32];
    const double mb = bytes_per_s / (1024.0 * 1024.0);
    if (mb >= 1.0)
        std::snprintf(num, sizeof num, "%.1f MB/s", mb);
    else
        std::snprintf(num, sizeof num, "%.0f KB/s", bytes_per_s / 1024.0 < 1.0 ? 1.0 : bytes_per_s / 1024.0);
    return num;
}

std::string wings_eta_text(std::int64_t seconds)
{
    if (seconds < 0)
        return std::string();
    if (seconds < 60)
        return tr("About {n} s left", {{"n", std::to_string(seconds)}});
    const std::int64_t minutes = (seconds + 59) / 60;
    return tr("About {n} min left", {{"n", std::to_string(minutes)}});
}

WingsFlowSnapshot wings_flow_snapshot(const WingsUpdateStatus &up, const WingsInstallStatus &inst)
{
    WingsFlowSnapshot s;
    s.running_text = up.running_text;
    const auto at = [&s](WingsFlowState state, WingsFlowStep step)
    {
        s.state = state;
        s.step = step;
        s.step_index = static_cast<int>(step);
    };
    const bool newer = up.attached && up.advice == WingsAdvice::newer;
    const bool fetchable = newer && up.asset.downloadable;
    // The installer's phase counts while it is about this release (or busy, or placed: those never revert).
    const bool this_release = inst.tag.empty() || inst.tag == up.latest_tag;
    WingsInstallPhase phase = inst.phase;
    if (!this_release && !inst.busy() && phase != WingsInstallPhase::placed)
        phase = WingsInstallPhase::idle;
    s.tag = phase == WingsInstallPhase::idle ? up.latest_tag : (inst.tag.empty() ? up.latest_tag : inst.tag);
    {
        int v[3];
        s.version_text = s.tag == up.latest_tag
                             ? up.latest_text
                             : (hui::parse_release_tag(s.tag, v) ? std::to_string(v[0]) + "." + std::to_string(v[1]) +
                                                                       "." + std::to_string(v[2])
                                                                 : std::string());
    }
    const std::vector<WingsInstallStep> manual = wings_install_manual_steps(inst);
    switch (phase)
    {
    case WingsInstallPhase::placed:
        // Installed stays installed whatever the checker or the switch say, until the new version runs.
        at(WingsFlowState::installed, WingsFlowStep::restart);
        s.step_done = true;
        s.progress = 1.0f;
        s.title = tr("Update installed");
        s.text = inst.message.empty() ? std::string(tr("Close Wings and open it again to use version {v}.",
                                                       {{"v", s.version_text}}))
                                      : inst.message;
        s.buttons.quit = true;
        return s;
    case WingsInstallPhase::checking:
    case WingsInstallPhase::downloading:
        s.bytes = inst.got_bytes;
        s.total = inst.total_bytes;
        if (phase == WingsInstallPhase::downloading && inst.total_bytes > 0 && inst.got_bytes >= inst.total_bytes)
        {
            at(WingsFlowState::checking, WingsFlowStep::download);
            s.progress = -1.0f;
            s.title = tr("Checking the download");
            s.text = tr("Checking the SHA-256 and the package...");
            return s;
        }
        at(WingsFlowState::downloading,
           phase == WingsInstallPhase::checking ? WingsFlowStep::check : WingsFlowStep::download);
        s.progress = inst.total_bytes > 0 ? std::min(1.0f, static_cast<float>(inst.got_bytes) /
                                                               static_cast<float>(inst.total_bytes))
                                          : 0.0f;
        s.speed_bps = phase == WingsInstallPhase::downloading ? inst.speed_bps : 0.0;
        s.eta_s = phase == WingsInstallPhase::downloading ? inst.eta_s : -1;
        s.speed_text = wings_speed_text(s.speed_bps);
        s.eta_text = wings_eta_text(s.eta_s);
        s.title = tr("Downloading the update");
        s.text = phase == WingsInstallPhase::checking
                     ? std::string(tr("Reading the published checksum..."))
                     : tr("{got} of {size}", {{"got", megabytes(inst.got_bytes)}, {"size", megabytes(inst.total_bytes)}});
        s.buttons.cancel = true;
        return s;
    case WingsInstallPhase::verified:
        at(WingsFlowState::ready_to_install, WingsFlowStep::install); // downloaded and checked
        s.progress = 0.0f;
        s.bytes = s.total = inst.total_bytes;
        s.title = tr("Downloaded and checked");
        s.text = inst.message;
        s.needs_replace_confirm = inst.needs_replace_confirm;
        s.buttons.install = true;
        return s;
    case WingsInstallPhase::placing:
        at(WingsFlowState::installing, WingsFlowStep::install);
        s.progress = -1.0f;
        s.title = tr("Installing the update");
        s.text = tr("Putting the update in place. This takes a few seconds...");
        return s;
    case WingsInstallPhase::manual:
    case WingsInstallPhase::blocked:
        at(WingsFlowState::manual, WingsFlowStep::install);
        s.title = tr("Install it yourself");
        s.text = inst.message;
        s.manual_steps = manual;
        s.buttons.steps = true;
        return s;
    case WingsInstallPhase::failed:
        at(WingsFlowState::failed, inst.install_tried ? WingsFlowStep::install : WingsFlowStep::download);
        s.title = tr("The update did not finish");
        s.reason = inst.message;
        s.text = inst.message;
        s.can_retry = fetchable && up.enabled;
        s.manual_steps = manual;
        s.buttons.retry = s.can_retry;
        s.buttons.steps = true;
        return s;
    case WingsInstallPhase::idle:
    default:
        break;
    }
    if (!newer || !up.enabled)
    {
        at(WingsFlowState::idle, WingsFlowStep::check);
        s.title = tr("Wings is up to date");
        s.text = wings_update_text(up).detail;
        return s;
    }
    if (!fetchable)
    {
        // A newer release without a package Wings can verify: the steps by hand.
        at(WingsFlowState::manual, WingsFlowStep::check);
        const WingsUpdateGuide guide = wings_update_guide(up);
        s.title = guide.title;
        s.text = up.asset.issue != WingsAssetIssue::none ? wings_asset_issue_text(up.asset.issue) : guide.subtitle;
        for (const WingsUpdateStep &step : guide.steps)
            s.manual_steps.push_back({step.label, step.detail});
        s.buttons.steps = true;
        return s;
    }
    at(WingsFlowState::available, WingsFlowStep::download);
    s.total = up.asset.size;
    s.title = tr("Wings {latest} is available", {{"latest", up.latest_text}});
    s.text = inst.message.empty() || !this_release
                 ? std::string(tr("Download checks it and gets the installer ready. Nothing is installed until you "
                      "confirm."))
                 : inst.message; // e.g. "Download cancelled. Nothing was changed."
    s.buttons.download = true;
    s.buttons.later = true;
    return s;
}

} // namespace orbit
