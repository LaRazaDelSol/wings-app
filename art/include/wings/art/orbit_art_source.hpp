// Wings cover-art module: adapter to the app's orbit::ArtSource interface.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header-only and the ONLY file of the module that includes app headers
// ("orbit/backend.hpp", from wings/app/src). The rest of the module builds
// without the app.
//
//   art_.reset(new wings::art::ArtService(config));
//   art_->start();
//   source_.reset(new wings::art::OrbitArtSource(*art_, resolver, &backend));
//   ArtCache cache(*source_, sink);
//
// ArtCache already understands the answers: Art::pending (it asks again after
// 0.6 s, up to 50 times), an error (the tile stays a placeholder and is asked
// again after 60 s, which now just hits the negative cache), or bytes.
#pragma once

#include "orbit/backend.hpp"

#include "wings/art/art_service.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wings::art
{

class OrbitArtSource final : public orbit::ArtSource
{
  public:
    // Maps an art key ("pkg:1234", "app:PPSA01234") to an https:// URL, or "" when
    // the key has no remote picture. Called on the art worker thread; typically a
    // lookup in the catalogue snapshot (posterUrl / hero URL).
    using UrlResolver = std::function<std::string(std::string_view key, orbit::ArtKind kind)>;

    // fallback: asked when the resolver returns "" (e.g. installed-game icons
    // that the backend serves itself). May be null.
    OrbitArtSource(ArtService &service, UrlResolver resolver, orbit::ArtSource *fallback = nullptr)
        : service_(service), resolver_(std::move(resolver)), fallback_(fallback)
    {
    }

    // Longest time art() may block waiting for a fetch that is already running.
    // 0 (default): never block; ArtCache polls through Art::pending.
    void set_max_wait_ms(int ms) { max_wait_ms_ = ms; }

    orbit::Result<orbit::Art> art(std::string_view key, orbit::ArtKind kind) override
    {
        orbit::Result<orbit::Art> out;
        const std::string url = resolver_ ? resolver_(key, kind) : std::string();
        if (url.empty())
        {
            if (fallback_)
                return fallback_->art(key, kind);
            out.status = 404;
            out.error = "no artwork URL for " + std::string(key);
            return out;
        }
        const Kind k = kind == orbit::ArtKind::hero ? Kind::hero : Kind::cover;
        ArtResult r = max_wait_ms_ > 0 ? wait(url, k) : service_.request(url, k, Lane::visible);
        switch (r.state)
        {
        case Fetch::ready:
            out.status = 200;
            out.value.bytes = std::move(r.bytes);
            out.value.content_type = std::move(r.content_type);
            break;
        case Fetch::pending:
            out.status = 202;
            out.value.pending = true;
            break;
        case Fetch::disabled:
            // status 0 + error: "the source is not answering". Not 503: ArtCache
            // would poll a 503 fifty times.
            out.status = 0;
            out.error = r.error;
            break;
        case Fetch::failed:
            out.status = r.permanent ? 404 : (r.http_status >= 400 && r.http_status != 503 ? r.http_status : 502);
            out.error = r.error.empty() ? "artwork unavailable" : r.error;
            out.retry_after = r.retry_after_s > 0 ? r.retry_after_s : -1;
            break;
        }
        return out;
    }

    // Warm the disk cache for what is just off screen (call from the screen's
    // snapshot update with the keys of the next rows). Returns URLs queued.
    std::size_t prefetch_keys(const std::vector<std::string> &keys, orbit::ArtKind kind)
    {
        std::vector<std::string> urls;
        for (const std::string &k : keys)
        {
            std::string u = resolver_ ? resolver_(k, kind) : std::string();
            if (!u.empty())
                urls.push_back(std::move(u));
        }
        return service_.prefetch(urls, kind == orbit::ArtKind::hero ? Kind::hero : Kind::cover, Lane::prefetch);
    }

  private:
    ArtResult wait(const std::string &url, Kind k)
    {
        ArtResult r = service_.request(url, k, Lane::visible);
        for (int waited = 0; r.state == Fetch::pending && waited < max_wait_ms_; waited += 5)
        {
            sleep_ms(5);
            r = service_.request(url, k, Lane::visible);
        }
        return r;
    }

    ArtService &service_;
    UrlResolver resolver_;
    orbit::ArtSource *fallback_;
    int max_wait_ms_ = 0;
};

} // namespace wings::art
