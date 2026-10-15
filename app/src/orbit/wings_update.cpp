// Wings TV app - "Is there a newer Wings?" (see wings_update.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/wings_update.hpp"

#include "core/save_file.hpp"
#include "core/version.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"
#include "orbit/pegasus_manager.hpp" // parse_sha256_digest
#include "orbit/stream_download.hpp" // stream_size_ok, stream_redirect_allowed

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

// The tag becomes part of an address: "v", digits and dots only.
bool tag_safe(std::string_view tag)
{
    int v[3];
    if (tag.size() > 24 || !hui::parse_release_tag(tag, v))
        return false;
    for (const char c : tag)
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'v'))
            return false;
    return true;
}

constexpr const char *kReleaseDownloadBase = "https://github.com/LaRazaDelSol/wings-app/releases/download/";

bool hex64(std::string_view text)
{
    if (text.size() != 64)
        return false;
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool blank(char c)
{
    return c == ' ' || c == '\t';
}

} // namespace

// ---- pure rules ----

std::string wings_asset_url(std::string_view tag)
{
    return tag_safe(tag) ? std::string(kReleaseDownloadBase) + std::string(tag) + "/" + kWingsAssetName
                         : std::string();
}

std::string wings_digest_url(std::string_view tag)
{
    return tag_safe(tag) ? std::string(kReleaseDownloadBase) + std::string(tag) + "/" + kWingsDigestAssetName
                         : std::string();
}

bool wings_url_allowed(std::string_view url, bool redirect, std::string_view tag)
{
    if (redirect)
        return stream_redirect_allowed(std::string(url));
    return !url.empty() && (url == wings_asset_url(tag) || url == wings_digest_url(tag));
}

bool parse_wings_checksum_text(std::string_view text, std::string *hex)
{
    // First non-empty line: 64 hex, then nothing, or white space and the file name (a leading '*' is the
    // binary-mode mark of sha256sum). Anything else (another file name, extra words) is refused.
    std::size_t pos = 0;
    while (pos < text.size() && (text[pos] == '\n' || text[pos] == '\r' || blank(text[pos])))
        ++pos;
    std::size_t end = text.find_first_of("\r\n", pos);
    if (end == std::string_view::npos)
        end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    while (!line.empty() && blank(line.back()))
        line.remove_suffix(1);
    if (line.size() < 64)
        return false;
    std::string lower;
    for (std::size_t i = 0; i < 64; ++i)
    {
        char c = line[i];
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        lower.push_back(c);
    }
    if (!hex64(lower))
        return false;
    std::string_view rest = line.substr(64);
    if (!rest.empty())
    {
        if (!blank(rest[0]))
            return false;
        while (!rest.empty() && blank(rest[0]))
            rest.remove_prefix(1);
        if (!rest.empty() && rest[0] == '*')
            rest.remove_prefix(1);
        if (rest != kWingsAssetName)
            return false;
    }
    if (hex != nullptr)
        *hex = lower;
    return true;
}

std::string wings_asset_issue_text(WingsAssetIssue issue)
{
    switch (issue)
    {
    case WingsAssetIssue::none:
        return std::string();
    case WingsAssetIssue::missing:
        return tr("This release has no package file");
    case WingsAssetIssue::bad_size:
        return tr("The package size in this release is not one Wings will accept");
    case WingsAssetIssue::no_digest:
        return tr("This release lists no SHA-256 for the package, so Wings will not fetch it");
    case WingsAssetIssue::bad_url:
        return tr("The package address in this release is not the official one");
    case WingsAssetIssue::no_checksum:
        return tr("This release has no published checksum file, so Wings will not fetch it");
    case WingsAssetIssue::digest_differs:
        return tr("The two published checksums for this release disagree, so nothing was fetched");
    }
    return std::string();
}

bool parse_wings_release(std::string_view text, WingsRelease *out, std::string *error)
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
    if (out == nullptr)
        return true;
    WingsRelease r;
    r.tag = name;
    r.asset.url = wings_asset_url(name);
    r.asset.digest_url = wings_digest_url(name);
    bool digest_asset_ok = false;
    for (const json::Value &asset : root["assets"].items())
    {
        if (!asset.is_object())
            continue;
        const std::string asset_name = asset.text("name");
        if (asset_name == kWingsDigestAssetName)
        {
            digest_asset_ok = asset.text("browser_download_url") == r.asset.digest_url;
            continue;
        }
        if (asset_name != kWingsAssetName)
            continue;
        r.asset.issue = WingsAssetIssue::bad_size;
        const double size = asset.number("size");
        if (size < 1.0 || size > static_cast<double>(kStreamMaxImage) ||
            !stream_size_ok(static_cast<std::uint64_t>(size)))
            continue;
        r.asset.size = static_cast<std::uint64_t>(size);
        r.asset.issue = WingsAssetIssue::no_digest;
        std::string hex;
        if (!parse_sha256_digest(asset.text("digest"), &hex))
            continue;
        r.asset.sha256 = hex;
        r.asset.issue = WingsAssetIssue::bad_url;
        if (asset.text("browser_download_url") != r.asset.url)
            continue;
        r.asset.issue = WingsAssetIssue::none;
    }
    if (r.asset.issue == WingsAssetIssue::none && !digest_asset_ok)
        r.asset.issue = WingsAssetIssue::no_checksum;
    r.asset.downloadable = r.asset.issue == WingsAssetIssue::none;
    if (!r.asset.downloadable)
    {
        // Nothing half-trusted is kept.
        r.asset.size = 0;
        r.asset.sha256.clear();
    }
    *out = std::move(r);
    return true;
}

bool parse_wings_latest(std::string_view text, std::string *tag, std::string *error)
{
    WingsRelease r;
    if (!parse_wings_release(text, &r, error))
        return false;
    if (tag != nullptr)
        *tag = r.tag;
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

std::string encode_wings_cache(std::int64_t checked_at, std::string_view tag, const WingsAsset &asset)
{
    std::string out = encode_wings_cache(checked_at, tag);
    if (asset.downloadable)
        out += "size=" + std::to_string(asset.size) + "\nsha256=" + asset.sha256 + "\n";
    return out;
}

bool decode_wings_cache(std::string_view payload, std::int64_t *checked_at, std::string *tag)
{
    return decode_wings_cache(payload, checked_at, tag, nullptr);
}

bool decode_wings_cache(std::string_view payload, std::int64_t *checked_at, std::string *tag, WingsAsset *asset)
{
    *checked_at = 0;
    tag->clear();
    std::uint64_t size = 0;
    std::string digest;
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
        else if (line.substr(0, 5) == "size=")
        {
            const std::string num(line.substr(5));
            if (!num.empty() && num.size() <= 12 && num.find_first_not_of("0123456789") == std::string::npos)
                size = static_cast<std::uint64_t>(std::atoll(num.c_str()));
        }
        else if (line.substr(0, 7) == "sha256=")
        {
            if (hex64(line.substr(7)))
                digest = std::string(line.substr(7));
        }
    }
    if (asset != nullptr)
    {
        *asset = WingsAsset();
        // The addresses are rebuilt from the tag: nothing read from disk is used as an address.
        if (!tag->empty() && stream_size_ok(size) && !digest.empty())
        {
            asset->downloadable = true;
            asset->issue = WingsAssetIssue::none;
            asset->size = size;
            asset->sha256 = digest;
            asset->url = wings_asset_url(*tag);
            asset->digest_url = wings_digest_url(*tag);
        }
    }
    return have_time;
}

// ---- the startup prompt ----

bool wings_should_prompt(std::int64_t now, std::string_view latest_tag, const WingsSnooze &snooze)
{
    if (latest_tag.empty())
        return false;
    if (snooze.tag != latest_tag)
        return true; // never snoozed, or a different (newer) release
    if (now < snooze.until && snooze.until - now <= kWingsSnoozeS)
        return false;
    return true;
}

WingsSnooze wings_snooze_for(std::string_view tag, std::int64_t now)
{
    WingsSnooze s;
    s.tag = std::string(tag);
    s.until = now + kWingsSnoozeS;
    return s;
}

std::string encode_wings_snooze(const WingsSnooze &snooze)
{
    return "snooze_tag=" + snooze.tag + "\nsnooze_until=" + std::to_string(snooze.until) + "\n";
}

bool decode_wings_snooze(std::string_view payload, WingsSnooze *out)
{
    *out = WingsSnooze();
    bool have_tag = false;
    bool have_until = false;
    std::size_t pos = 0;
    while (pos < payload.size())
    {
        std::size_t nl = payload.find('\n', pos);
        if (nl == std::string_view::npos)
            nl = payload.size();
        const std::string_view line = payload.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.substr(0, 11) == "snooze_tag=")
        {
            int v[3];
            if (!hui::parse_release_tag(line.substr(11), v))
                return false;
            out->tag = std::string(line.substr(11));
            have_tag = true;
        }
        else if (line.substr(0, 13) == "snooze_until=")
        {
            const std::string num(line.substr(13));
            if (num.empty() || num.size() > 12 || num.find_first_not_of("0123456789") != std::string::npos)
                return false;
            out->until = std::atoll(num.c_str());
            have_until = true;
        }
    }
    if (!have_tag || !have_until)
    {
        *out = WingsSnooze();
        return false;
    }
    return true;
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
        WingsAsset asset;
        if (d.ok && d.version == 1 && decode_wings_cache(d.payload, &at, &tag, &asset))
        {
            status_.checked_at = at;
            status_.latest_tag = tag;
            status_.asset = asset;
            status_.have_latest = !tag.empty();
            break;
        }
    }
    prompted_tag_.clear();
    for (const std::string &path : snooze_paths())
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 1024))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        WingsSnooze snooze;
        if (d.ok && d.version == 1 && decode_wings_snooze(d.payload, &snooze))
        {
            status_.snooze = snooze;
            break;
        }
    }
    refresh_advice_locked();
}

std::vector<std::string> WingsUpdate::snooze_paths() const
{
    if (!config_.snooze_paths.empty())
        return config_.snooze_paths;
    std::vector<std::string> out;
    for (const std::string &path : config_.cache_paths)
    {
        const std::size_t slash = path.rfind('/');
        out.push_back((slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) +
                      "wings_update_snooze.v1");
    }
    return out;
}

bool WingsUpdate::take_prompt()
{
    Lock l(mutex_);
    if (!status_.attached || !status_.enabled || status_.advice != WingsAdvice::newer ||
        prompted_tag_ == status_.latest_tag || !wings_should_prompt(config_.now(), status_.latest_tag, status_.snooze))
        return false;
    prompted_tag_ = status_.latest_tag;
    return true;
}

void WingsUpdate::snooze_latest()
{
    std::string data;
    std::vector<std::string> paths;
    {
        Lock l(mutex_);
        if (!status_.attached || status_.latest_tag.empty())
            return;
        status_.snooze = wings_snooze_for(status_.latest_tag, config_.now());
        prompted_tag_ = status_.latest_tag;
        data = hui::save::encode(hui::save::Kind::stats, 1, encode_wings_snooze(status_.snooze));
        paths = snooze_paths();
    }
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            break;
    }
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
    WingsAsset asset;
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
        WingsRelease release;
        if (!parse_wings_release(r.body, &release, &why))
            error = tr("The release information could not be read");
        else
        {
            tag = release.tag;
            asset = release.asset;
        }
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
        status_.asset = asset;
        status_.have_latest = true;
        refresh_advice_locked();
        data = hui::save::encode(hui::save::Kind::stats, 1, encode_wings_cache(status_.checked_at, tag, asset));
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
        out.detail = s.asset.downloadable
                         ? tr("You have {mine}. Download update below fetches and installs it; Select here shows the steps by hand.",
                              {{"mine", s.running_text}})
                         : tr("You have {mine}. Select shows how to install it by hand.",
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
    g.subtitle = tr("You have {mine}. These are the steps to do it by hand.", {{"mine", s.running_text}});
    g.steps = {
        {tr("Download PPSA99178.ffpkg and check its SHA-256"),
         tr("{where}, on a PC. The hash is on that page", {{"where", kWingsReleasePage}})},
        {tr("Replace the old file"), tr("Copy it to /data/homebrew/ on this console, over the old one")},
        {tr("Remove the staged copies"), tr("Delete the copies ShadowMountPlus made of the old package")},
        {tr("Close Wings and open it again"), tr("So the new version runs")},
    };
    return g;
}

std::string wings_update_toast(const WingsUpdateStatus &s)
{
    return tr("Wings {latest} is available. Install it from Settings > Wings > Updates.",
              {{"latest", s.latest_text}});
}

} // namespace orbit
