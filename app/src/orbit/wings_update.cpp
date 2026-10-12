// Wings TV app - "Is there a newer Wings?" (see wings_update.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/wings_update.hpp"

#include "core/save_file.hpp"
#include "core/version.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"

#include <chrono>
#include <cstdlib>

namespace orbit
{

namespace
{

std::int64_t system_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "v0.1.2" -> "0.1.2"; "" for anything else.
std::string tag_text(std::string_view tag)
{
    int v[3];
    if (!hui::parse_release_tag(tag, v))
        return {};
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

} // namespace

// ---- pure rules ----

bool parse_wings_latest(std::string_view text, std::string *tag, std::string *error)
{
    const auto fail = [&](const char *why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };
    json::Value root;
    std::string why;
    if (!json::parse(text, &root, &why) || !root.is_object())
        return fail("not a release");
    // "releases/latest" never answers with these; an answer that does is not one to act on.
    if (root.flag("draft") || root.flag("prerelease"))
        return fail("not a published release");
    const std::string name = root.text("tag_name");
    int v[3];
    if (!hui::parse_release_tag(name, v))
        return fail("the release tag is not a version");
    if (tag != nullptr)
        *tag = name;
    return true;
}

bool wings_check_due(std::int64_t checked_at, std::int64_t now, bool force)
{
    if (force || checked_at <= 0 || now < checked_at)
        return true;
    return now - checked_at >= kWingsCheckTtlS;
}

WingsAdvice wings_advice(std::string_view running, std::string_view latest_tag)
{
    if (latest_tag.empty())
        return WingsAdvice::not_checked;
    int mine[3];
    int latest[3];
    if (!hui::parse_content_version(running, mine) || !hui::parse_release_tag(latest_tag, latest))
        return WingsAdvice::unreadable;
    const int order = hui::compare_versions(mine, latest);
    return order < 0 ? WingsAdvice::newer : (order == 0 ? WingsAdvice::up_to_date : WingsAdvice::ahead);
}

std::string encode_wings_cache(std::int64_t checked_at, std::string_view tag)
{
    return "checked=" + std::to_string(checked_at) + "\ntag=" + std::string(tag) + "\n";
}

bool decode_wings_cache(std::string_view payload, std::int64_t *checked_at, std::string *tag)
{
    *checked_at = 0;
    tag->clear();
    bool have_time = false;
    std::size_t pos = 0;
    while (pos < payload.size())
    {
        std::size_t nl = payload.find('\n', pos);
        if (nl == std::string_view::npos)
            nl = payload.size();
        const std::string_view line = payload.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.substr(0, 8) == "checked=")
        {
            const std::string num(line.substr(8));
            if (num.empty() || num.size() > 12 || num.find_first_not_of("0123456789") != std::string::npos)
                return false;
            *checked_at = std::atoll(num.c_str());
            have_time = true;
        }
        else if (line.substr(0, 4) == "tag=")
        {
            int v[3];
            if (hui::parse_release_tag(line.substr(4), v))
                *tag = std::string(line.substr(4));
        }
    }
    return have_time;
}

// ---- the service ----

WingsUpdate &wings_update()
{
    static WingsUpdate update;
    return update;
}

WingsUpdate::~WingsUpdate()
{
    thread_.join();
}

bool WingsUpdate::gate(std::string *why) const
{
    if (!config_.ready)
    {
        *why = tr("Not available in this build");
        return false;
    }
    return config_.ready(why);
}

void WingsUpdate::refresh_advice_locked()
{
    status_.running_text = hui::release_version_text(config_.running);
    status_.latest_text = tag_text(status_.latest_tag);
    status_.advice = wings_advice(config_.running, status_.latest_tag);
}

void WingsUpdate::attach(WingsUpdateConfig config)
{
    thread_.join();
    Lock l(mutex_);
    config_ = std::move(config);
    if (!config_.now)
        config_.now = system_now;
    status_ = WingsUpdateStatus();
    status_.attached = true;
    attempt_at_ = 0;
    for (const std::string &path : config_.cache_paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 4096))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        std::int64_t at = 0;
        std::string tag;
        if (d.ok && d.version == 1 && decode_wings_cache(d.payload, &at, &tag))
        {
            status_.checked_at = at;
            status_.latest_tag = tag;
            status_.have_latest = !tag.empty();
            break;
        }
    }
    refresh_advice_locked();
}

void WingsUpdate::set_enabled(bool on)
{
    Lock l(mutex_);
    status_.enabled = on;
}

WingsUpdateStatus WingsUpdate::status() const
{
    Lock l(mutex_);
    return status_;
}

void WingsUpdate::wait_idle()
{
    thread_.join();
}

bool WingsUpdate::start_job()
{
    {
        Lock l(mutex_);
        if (!status_.attached || status_.busy)
            return false;
        status_.busy = true;
        status_.error.clear();
        attempt_at_ = config_.now();
    }
    thread_.join();
    if (!thread_.start([this] { run_check(); }, 256u << 10))
    {
        Lock l(mutex_);
        status_.busy = false;
        status_.error = tr("Could not start the background job");
        return false;
    }
    return true;
}

void WingsUpdate::auto_check()
{
    {
        Lock l(mutex_);
        if (!status_.attached || status_.busy || !status_.enabled)
            return;
        const std::int64_t now = config_.now();
        if (!wings_check_due(status_.checked_at, now, false))
            return;
        // A failed attempt is not repeated every frame.
        if (attempt_at_ != 0 && now >= attempt_at_ && now - attempt_at_ < kWingsRetryS)
            return;
    }
    std::string why;
    if (!gate(&why))
        return; // the network is not up yet (or is off): try again on a later frame, say nothing
    start_job();
}

bool WingsUpdate::check_now()
{
    std::string why;
    if (!gate(&why))
    {
        Lock l(mutex_);
        status_.error = why;
        return false;
    }
    return start_job();
}

void WingsUpdate::run_check()
{
    DocResult r;
    if (config_.docs != nullptr)
        r = config_.docs->get(kWingsLatestReleaseUrl, kWingsReleaseCap);
    else
        r.error = "no network client";
    std::string tag;
    std::string error;
    if (!r.error.empty())
        error = r.error;
    else if (r.status == 403 || r.status == 429)
        error = tr("GitHub is limiting requests from this address. Try again later.");
    else if (r.status == 404)
        error = tr("No release has been published yet");
    else if (r.status != 200)
        error = tr("GitHub answered HTTP {status}", {{"status", std::to_string(r.status)}});
    else
    {
        std::string why;
        if (!parse_wings_latest(r.body, &tag, &why))
            error = tr("The release information could not be read");
    }
    std::string data;
    {
        Lock l(mutex_);
        status_.busy = false;
        status_.error = error;
        if (!error.empty())
            return;
        status_.checked_at = config_.now();
        status_.latest_tag = tag;
        status_.have_latest = true;
        refresh_advice_locked();
        data = hui::save::encode(hui::save::Kind::stats, 1, encode_wings_cache(status_.checked_at, tag));
    }
    // The saved answer is written outside the lock (the frame thread reads the status every frame).
    for (const std::string &path : config_.cache_paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            break;
    }
}

// ---- words ----

WingsUpdateText wings_update_text(const WingsUpdateStatus &s)
{
    WingsUpdateText out;
    const std::string where = kWingsReleasePage;
    if (!s.attached)
    {
        out.value = "-";
        out.detail = tr("Not available in this build");
        return out;
    }
    if (s.busy)
    {
        out.value = tr("Checking...");
        out.detail = tr("Asking {where} for the latest release", {{"where", where}});
        return out;
    }
    if (s.advice == WingsAdvice::newer)
    {
        out.value = tr("{version} available", {{"version", s.latest_text}});
        out.detail = tr("You have {mine}. Select shows how to install it. Wings never installs itself.",
                        {{"mine", s.running_text}});
        return out;
    }
    if (!s.error.empty())
    {
        out.value = tr("Check");
        out.detail = tr("Could not check: {why}", {{"why", s.error}});
        return out;
    }
    switch (s.advice)
    {
    case WingsAdvice::up_to_date:
        out.value = tr("Up to date");
        out.detail = tr("Wings {mine} is the latest release. Select checks again.", {{"mine", s.running_text}});
        break;
    case WingsAdvice::ahead:
        out.value = tr("Newer than the release");
        out.detail = tr("This build ({mine}) is newer than the latest release ({latest}). Select checks again.",
                        {{"mine", s.running_text}, {"latest", s.latest_text}});
        break;
    case WingsAdvice::unreadable:
        out.value = tr("Check");
        out.detail = tr("The latest release is {latest}, but this app's own version could not be read.",
                        {{"latest", s.latest_text}});
        break;
    default:
        out.value = tr("Check");
        out.detail = s.enabled ? tr("Wings looks at {where} at most every 6 hours. Select checks now.",
                                    {{"where", where}})
                               : tr("Checking at start-up is off. Select checks now.");
        break;
    }
    return out;
}

WingsUpdateGuide wings_update_guide(const WingsUpdateStatus &s)
{
    WingsUpdateGuide g;
    g.title = tr("Update to Wings {latest}", {{"latest", s.latest_text}});
    g.subtitle = tr("You have {mine}. Wings never installs itself.", {{"mine", s.running_text}});
    g.steps = {
        {tr("Download PPSA99178.ffpkg and check its SHA-256"),
         tr("{where}, on a PC. The hash is on that page", {{"where", kWingsReleasePage}})},
        {tr("Replace the old file"), tr("Copy it to /data/homebrew/ on this console, over the old one")},
        {tr("Remove the staged copies"), tr("Delete the copies ShadowMountPlus made of the old package")},
        {tr("Restart the jailbreak chain"), tr("So the new package is mounted. Then open Wings again")},
    };
    return g;
}

std::string wings_update_toast(const WingsUpdateStatus &s)
{
    return tr("Wings {latest} is available. Settings > Wings > Updates has the steps.",
              {{"latest", s.latest_text}});
}

} // namespace orbit
