// Wings cover-art module: on-console probe, compiled INTO a copy of the Wings app
// by tools/link-probe.sh (PS5 toolchain, -fno-exceptions -fno-rtti, PacBrew
// OpenSSL). 3 s after launch a pthread:
//   1. starts ArtService with the bundled CA file, disk cache under /download0/wings/art-probe
//   2. (only when built with SCENET=1) tries the console resolver, logs the result
//   3. switches to DNS-over-HTTPS, fetches 3 posters, decodes them with orbit::image
//   4. fetches expired.badssl.com (must fail) and writes the cache a second time (disk hit)
// Everything is logged with the [WINGS-ART] tag to the title's app.log:
//   /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev/app.log   (FTP, while running)
// Not part of the module or of the host tests.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/art_service.hpp"
#include "orbit/image.hpp"

#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <string>

using namespace wings::art;

namespace
{

void show(const char *what, const ArtResult &r)
{
    std::printf("[WINGS-ART] %s -> state=%d bytes=%zu type=%s permanent=%d err=%s\n", what,
                static_cast<int>(r.state), r.bytes.size(), r.content_type.c_str(), r.permanent ? 1 : 0,
                r.error.c_str());
    if (r.state == Fetch::ready)
    {
        orbit::image::Pixels px;
        std::string err;
        const bool ok = orbit::image::decode(r.bytes, 400, 400, &px, &err);
        std::printf("[WINGS-ART]   decode: %s %dx%d %s\n", ok ? "ok" : "FAILED", px.width, px.height, err.c_str());
    }
}

void *probe(void *)
{
    sleep(3); // log redirect and heap are up
    std::printf("[WINGS-ART] probe start (system DNS compiled: %d)\n", system_dns_compiled() ? 1 : 0);
    ArtConfig cfg;
    cfg.cache_root = "/download0/wings/art-probe";
    cfg.ca_bundle_paths = {"/download0/wings/ca-bundle.pem", "/app0/assets/ca-bundle.crt"};
    cfg.dns_mode = DnsMode::off;
    cfg.log = [](int, const std::string &line) { std::printf("%s\n", line.c_str()); };
    ArtService svc(cfg);
    std::string problem;
    const bool started = svc.start(&problem);
    ServiceState st = svc.state();
    std::printf("[WINGS-ART] start=%d phase=%d ca=%d (%s) disk_ok=%d detail=%s %s\n", started ? 1 : 0,
                static_cast<int>(st.phase), st.ca_certs, st.ca_source.c_str(), st.disk_ok ? 1 : 0,
                st.detail.c_str(), problem.c_str());
    if (!started)
        return nullptr;

    const char *urls[] = {
        "https://image.api.playstation.com/vulcan/ap/rnd/202211/2218/9UflSkyn6ZUATU2VJNcMQ49b.png?w=260",
        "https://cdn.prosperopatches.com/titles/PPSA24249_16f6978ace1bb1247be8ebd750ee6f0ef9cb2c4f9b76cb0a25d9033cd5700ca2/icon0.webp",
        "https://cdn.jsdelivr.net/gh/nexgen999/evoX-CoreOS@main/assets/icon/FPKGi.png",
    };
    show("DNS off", svc.request(urls[0]));
    if (system_dns_compiled())
    {
        svc.set_dns_mode(DnsMode::system);
        st = svc.state();
        std::printf("[WINGS-ART] system DNS phase=%d detail=%s\n", static_cast<int>(st.phase), st.detail.c_str());
        if (st.phase == Phase::ready)
            show("system DNS poster", svc.fetch_blocking(urls[0], Kind::cover, 20000));
    }
    svc.set_dns_mode(DnsMode::doh);
    for (const char *u : urls)
        show(u, svc.fetch_blocking(u, Kind::cover, 25000));
    show("second ask (disk)", svc.fetch_blocking(urls[0]));
    show("expired.badssl.com", svc.fetch_blocking("https://expired.badssl.com/", Kind::cover, 25000));
    const ArtStats s = svc.stats();
    std::printf("[WINGS-ART] stats: net_ok=%llu bytes=%llu reused=%llu disk_hits=%llu entries=%llu\n",
                static_cast<unsigned long long>(s.net_ok), static_cast<unsigned long long>(s.net_bytes),
                static_cast<unsigned long long>(s.connections_reused),
                static_cast<unsigned long long>(s.disk_hits), static_cast<unsigned long long>(s.disk.entries));
    std::printf("[WINGS-ART] probe done\n");
    svc.stop();
    return nullptr;
}

__attribute__((constructor)) void wings_art_probe_start()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_t t;
    pthread_create(&t, &attr, probe, nullptr);
}

} // namespace
