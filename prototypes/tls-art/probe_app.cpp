// Wings prototype: on-console probe, compiled INTO a copy of the Orbit app build
// (PS5 clang wrapper, -fno-exceptions -fno-rtti, PacBrew OpenSSL 3.5.2) by
// link-probe.sh. 3 s after launch a pthread loads /app0/assets/ca-bundle.crt,
// resolves a name over DoH, fetches three real posters over HTTPS, decodes them
// with the app's own orbit::image, and logs everything with the [WINGS-TLS] tag.
// The app already redirects stdout to the title's log:
//   /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev/app.log   (FTP, while running)
// Not part of the host test.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "doh.hpp"
#include "orbit/image.hpp"
#include "tls_client.hpp"

#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace
{

std::string slurp(const char *path)
{
    std::string out;
    if (FILE *f = std::fopen(path, "rb"))
    {
        char buf[8192];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void *probe(void *)
{
    sleep(3); // let the log redirect and the heap come up
    std::printf("[WINGS-TLS] probe start\n");
    const std::string pem = slurp("/app0/assets/ca-bundle.crt");
    std::string info;
    auto shared = wings::tls::Shared::create(pem, &info);
    std::printf("[WINGS-TLS] ca file %zu bytes: %s\n", pem.size(), info.c_str());
    if (!shared)
        return nullptr;
    wings::tls::DohResolver doh(shared, {"https://1.1.1.1/dns-query", "https://8.8.8.8/dns-query"});
    doh.install();
    std::vector<std::uint32_t> ips;
    int ttl = 0;
    std::string why;
    const bool dns_ok = doh.resolve("image.api.playstation.com", &ips, &ttl, &why);
    std::printf("[WINGS-TLS] DoH image.api.playstation.com: %s (%zu addresses, ttl %d) %s\n",
                dns_ok ? "ok" : "FAILED", ips.size(), ttl, why.c_str());

    const char *urls[] = {
        "https://image.api.playstation.com/vulcan/ap/rnd/202211/2218/9UflSkyn6ZUATU2VJNcMQ49b.png?w=260",
        "https://cdn.prosperopatches.com/titles/PPSA24249_16f6978ace1bb1247be8ebd750ee6f0ef9cb2c4f9b76cb0a25d9033cd5700ca2/icon0.webp",
        "https://cdn.jsdelivr.net/gh/nexgen999/evoX-CoreOS@main/assets/icon/FPKGi.png",
    };
    wings::tls::Client client(shared);
    for (const char *url : urls)
    {
        auto r = client.get(url);
        std::printf("[WINGS-TLS] GET %s -> status=%d err=%s (%s) bytes=%zu type=%s dns=%.0fms tcp=%.0fms tls=%.0fms ttfb=%.0fms reused=%d\n",
                    url, r.status, wings::tls::name(r.error), r.detail.c_str(), r.body.size(),
                    r.content_type.c_str(), r.timing.dns_ms, r.timing.connect_ms, r.timing.tls_ms,
                    r.timing.first_byte_ms, r.timing.reused ? 1 : 0);
        if (!r.ok())
            continue;
        orbit::image::Pixels px;
        std::string error;
        const bool ok = orbit::image::decode(std::string_view(reinterpret_cast<const char *>(r.body.data()), r.body.size()),
                                             400, 400, &px, &error);
        std::printf("[WINGS-TLS]   decode: %s %dx%d %s\n", ok ? "ok" : "FAILED", px.width, px.height, error.c_str());
    }
    auto bad = client.get("https://expired.badssl.com/");
    std::printf("[WINGS-TLS] negative: expired.badssl.com -> %s (%s)\n", wings::tls::name(bad.error), bad.detail.c_str());
    std::printf("[WINGS-TLS] probe done\n");
    return nullptr;
}

__attribute__((constructor)) void wings_tls_probe_start()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_t thread;
    pthread_create(&thread, &attr, probe, nullptr);
}

} // namespace
