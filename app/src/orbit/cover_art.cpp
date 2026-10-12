// Wings TV app - Cover art glue that needs neither OpenSSL nor the console.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/cover_art.hpp"

#include "core/save_file.hpp"
#include "orbit/art_sources.hpp"
#include "orbit/game_header.hpp"
#include "orbit/popularity.hpp"

namespace orbit
{

bool PosterIndex::sync(std::uint64_t serial, const std::vector<Package> &packages)
{
    {
        Lock lock(mutex_);
        if (serial == serial_)
            return false;
    }
    // Built outside the lock: the art worker is never held up by 3,000 inserts.
    std::unordered_map<std::string, std::string> next;
    std::unordered_map<std::string, std::string> next_titles;
    next.reserve(packages.size());
    for (const Package &package : packages)
    {
        if (package.id.empty())
            continue;
        if (!package.title_id.empty())
            next_titles.emplace(package.id, package.title_id);
        if (package.poster_url.compare(0, 8, "https://") != 0)
            continue;
        next.emplace(package.id, package.poster_url);
    }
    Lock lock(mutex_);
    urls_.swap(next);
    title_ids_.swap(next_titles);
    serial_ = serial;
    return true;
}

std::string PosterIndex::resolve(std::string_view key, ArtKind kind) const
{
    if (key.substr(0, 4) != "pkg:")
        return {};
    // Hero art: only for the game page that is open (game_header.hpp registers its
    // URL from the popularity index). Grid tiles never ask, so nothing is prefetched.
    if (kind == ArtKind::hero)
        return hero_urls().find(key);
    if (kind == ArtKind::hero_low)
        return hero_urls().find_low(key);
    const std::string id(key.substr(4));
    Lock lock(mutex_);
    if (prospero_active_)
    {
        // The icon sits next to the title's 16:9 picture in the popularity data.
        const auto title = title_ids_.find(id);
        if (title != title_ids_.end())
        {
            const PopRecord *record = index_->find(title->second);
            if (record != nullptr)
            {
                const std::string icon = prospero_icon_url(record->hero);
                if (!icon.empty())
                    return icon;
            }
        }
    }
    const auto found = urls_.find(id);
    return found == urls_.end() ? std::string() : found->second;
}

bool PosterIndex::set_icon_source(bool prospero, std::shared_ptr<const PopularityIndex> index)
{
    Lock lock(mutex_);
    const bool active = prospero && index != nullptr;
    const bool changed = active != prospero_active_ || (active && index.get() != index_.get());
    prospero_active_ = active;
    index_ = active ? std::move(index) : nullptr;
    return changed;
}

std::size_t PosterIndex::size() const
{
    Lock lock(mutex_);
    return urls_.size();
}

std::string encode_prefs(const WingsPrefs &prefs)
{
    return std::string("art_doh=") + (prefs.art_doh ? "1" : "0") + "\n";
}

bool decode_prefs(std::string_view payload, WingsPrefs *out)
{
    WingsPrefs prefs;
    std::size_t at = 0;
    while (at < payload.size())
    {
        std::size_t end = payload.find('\n', at);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string_view line = payload.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        if (key == "art_doh")
            prefs.art_doh = value == "1";
    }
    *out = prefs;
    return true;
}

bool load_prefs(const std::vector<std::string> &paths, WingsPrefs *out)
{
    for (const std::string &path : paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 4096))
            continue;
        const hui::save::Decoded decoded = hui::save::decode(hui::save::Kind::prefs, data);
        if (decoded.ok && decoded.version == 1 && decode_prefs(decoded.payload, out))
            return true;
    }
    return false;
}

int save_prefs(const std::vector<std::string> &paths, const WingsPrefs &prefs)
{
    const std::string data = hui::save::encode(hui::save::Kind::prefs, 1, encode_prefs(prefs));
    int written = 0;
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            ++written;
    }
    return written;
}

} // namespace orbit
