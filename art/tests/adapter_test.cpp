// Wings cover-art tests: the OrbitArtSource adapter against the app's real
// orbit::ArtSource interface (headers from wings/app/src; nothing else linked).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wings/art/orbit_art_source.hpp"

#include <unistd.h>

#include <cstdio>

using namespace wings::art;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Fallback : orbit::ArtSource
{
    int calls = 0;
    orbit::Result<orbit::Art> art(std::string_view, orbit::ArtKind) override
    {
        ++calls;
        orbit::Result<orbit::Art> r;
        r.status = 200;
        r.value.bytes = "fallback";
        return r;
    }
};

int main()
{
    const std::string root = "/tmp/wings-art-adapter-" + std::to_string(::getpid());
    ArtConfig cfg;
    cfg.cache_root = root;
    cfg.ca_bundle_paths = {"/work/wings/art/ca-bundle.crt"};
    ArtService svc(cfg); // dns off by default
    CHECK(svc.start());
    const std::string u = "https://cdn.test/a.png";
    std::string png("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x10\x08\x06\0\0\0\0\0\0\0", 29);
    CHECK(svc.disk().put(u, reinterpret_cast<const std::uint8_t *>(png.data()), png.size()));

    Fallback fb;
    OrbitArtSource src(svc, [](std::string_view key, orbit::ArtKind) -> std::string {
        if (key == "pkg:1") return "https://cdn.test/a.png";
        if (key == "pkg:2") return "https://cdn.test/never.png";
        return std::string();
    }, &fb);
    orbit::ArtSource &iface = src; // usable wherever ArtCache wants an ArtSource

    auto r = iface.art("pkg:1", orbit::ArtKind::cover);
    CHECK(r.ok() && r.status == 200 && r.value.bytes == png && r.value.content_type == "image/png");
    r = iface.art("pkg:2", orbit::ArtKind::cover);
    CHECK(!r.ok() && r.status == 0 && r.unreachable() && r.error.find("DNS-over-HTTPS") != std::string::npos);
    r = iface.art("app:PPSA00001", orbit::ArtKind::cover);
    CHECK(r.ok() && r.value.bytes == "fallback" && fb.calls == 1);
    std::printf(fails ? "adapter test FAILED\n" : "adapter test ok\n");
    return fails ? 1 : 0;
}
