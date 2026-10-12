# wings/art - HTTPS cover art for the Wings TV app

SPDX-License-Identifier: GPL-3.0-or-later. Self-contained C++17 library (no exceptions, no RTTI, pthreads
only) that fetches `https://` poster URLs on a PS5 native title, validates them, and caches the original
compressed bytes on disk. Decode/upload/eviction of textures stays in the app's `ArtCache`.

Design and measurements: `docs/tls-art.md`. This module is the production version of
`wings/prototypes/tls-art/`.

## What is here

| Path | Purpose |
|---|---|
| `include/wings/art/art_service.hpp`, `src/art_service.cpp` | `ArtService`: 4-worker pool, lanes (visible, near, prefetch, idle fill), request dedupe, retry schedule, host breaker, offline probing, negative cache, heap pause, state for Settings |
| `include/wings/art/tls_client.hpp`, `src/tls_client.cpp` | OpenSSL HTTPS client: keep-alive pool, <= 3 https-only redirects, 2 MiB cap, type allow-list, address refusal, cancel, no insecure mode |
| `include/wings/art/dns.hpp`, `src/dns.cpp` | `DnsProvider` interface, `CachingDns` (TTL cache), `SystemDns` (console resolver, `-DWINGS_ART_SCENET` only) |
| `include/wings/art/doh.hpp`, `src/doh.cpp` | `DohProvider`: RFC 8484 GET to an IPv4-literal endpoint (opt-in) |
| `include/wings/art/disk_cache.hpp`, `src/disk_cache.cpp` | URL-keyed cache, atomic writes, header+checksum, LRU to 96 MiB, 24 h negative records, persisted index |
| `include/wings/art/image_probe.hpp`, `src/image_probe.cpp` | magic-byte sniff + dimensions from the header (4 Mpx / 4096 px guard) before anything is cached |
| `include/wings/art/orbit_art_source.hpp` | `OrbitArtSource`: adapter to the app's `orbit::ArtSource` (header only, the only file that includes app headers) |
| `include/wings/art/platform.hpp`, `src/platform.cpp` | pthread mutex/cond/thread + clock (mirrors `orbit/thread.hpp`) |
| `shims/ps5_libc_shims.c` | libc symbols OpenSSL needs and the app link lacks (console build only) |
| `ca-bundle.crt` | Mozilla CA bundle from the pinned PacBrew sysroot (see below) |
| `tests/`, `tools/` | host tests (local TLS fixture server, safety suite, adapter test), Docker/WSL scripts, PS5 link probe |
| `probe/art_probe.cpp` | on-console probe compiled into a copy of the app by `tools/link-probe.sh` |
| `docs/IMAGE_GUARD.md` | patch spec for `orbit/image.cpp` (dimension guard) and the `ArtCache` upload bound |

## Behaviour in one page

```
request(url) -> memory handoff -> disk cache -> [network allowed?] -> negative cache -> retry/host/offline
             -> queue (lanes) -> TLS client -> sniff + size guard -> disk cache
```

* **`request()` never blocks on the network.** It returns `ready` (bytes + sniffed MIME), `pending` (a worker
  is on it; ask again, which also re-promotes it in its lane), `failed` (`permanent`, `retry_after_s`),
  or `disabled` (no network allowed: see `state()`).
* **Workers:** 4 (clamped 1..6), 1 MiB stacks, each with its own keep-alive `tls::Client`. Visible and near
  lanes are newest-first (what is on screen now), prefetch and idle fill are FIFO; idle fill never runs while
  foreground work waits and uses at most `idle_fill_workers` (2). Queue bound: 96 foreground (oldest dropped),
  512 background (new ones ignored).
* **Disk cache:** `<root>/<xx>/<fnv1a64>.art` = header (magic, URL, fetch time, length, checksum) + original
  bytes; `.neg` = negative record; `index.v1` = size and last use per entry (LRU does not rely on `utimes`).
  `<file>.tmp` + `rename`, leftovers removed at start. Eviction to 96 MiB at start, on budget overflow and every
  200 writes. Entries older than 30 days are served and refreshed in the idle lane.
* **Failures:** permanent (404/410/4xx, bad type, too large, guard refusal, AVIF/animated, bad certificate,
  blocked address) -> negative cache 24 h. Transient -> retry at 2 s, 8 s, 30 s, 2 min, 10 min, then 1 h
  (`Retry-After` honoured, capped at 1 h). 5 consecutive transient failures on a host pause that host 60 s;
  two resolver failures put the service **offline** and it probes every 30 s. Certificate "expired / not yet
  valid" is treated as transient and sets `ServiceState::clock_suspect` (wrong console clock).
* **Heap awareness:** `ArtConfig::heap_used` is polled by the workers; above 100 MiB no new fetch starts,
  resumes under 92 MiB (`phase == paused_heap`). In-flight fetches finish.
* **Safety (all tested):** https only (also for redirects), no credentials in URLs, no IPv6 or ambiguous numeric
  hosts (`2130706433`, `0x7f000001`, `127.1`), `localhost`/`*.localhost` refused, resolved addresses in
  loopback / RFC 1918 / link-local / CGNAT / TEST-NET / benchmarking / multicast / reserved space refused
  (DNS rebinding, Pegasus on `127.0.0.1:6970`, the LAN), certificate chain + host name (or IP SAN) always
  verified, TLS >= 1.2, **an empty or unparseable CA bundle is refused** (no fallback), `RAND_status() == 1`
  required, `Accept-Encoding: identity` and any other `Content-Encoding` rejected, body cap from
  `Content-Length` and while reading (also chunked / close-delimited), `Content-Type` must be `image/*`
  (not svg) / octet-stream / empty, then **magic bytes** and header dimensions (<= 4 Mpx, <= 4096 px per side).
  No cookies, Authorization or Referer are ever sent; UA `WingsTV/0.1`.

## DNS: the decision

Research (SDK in the pinned build image, `ps5-payload-sdk` target stubs):

* `getaddrinfo`, `gethostbyname`, `getnameinfo` are exported only by `libScePosixForWebKit`, which a native
  title does not load. Never used; `shims/ps5_libc_shims.c` replaces them with clean failures.
* `libSceNet.so` exports `sceNetInit`, `sceNetPoolCreate`, `sceNetResolverCreate`, `sceNetResolverStartNtoa`,
  `sceNetResolverDestroy` ... but ships **no header** and no sample; the prototypes in `dns.cpp` are written
  from the public API. A probe build with `SCENET=1` links and signs, and adds only `libSceNet.prx` to
  `DT_NEEDED`. Whether the title may call `sceNetInit` and the resolver is **not verifiable off-console**.

So the shipped default is: **`DnsMode::off`** - no network at all, covers already on disk still display, and
`ServiceState` says "Cover art is off. Turn on DNS-over-HTTPS in Settings ...". `DnsMode::doh` is opt-in.
`DnsMode::system` exists behind `-DWINGS_ART_SCENET` for the on-console probe; if it works on hardware, flip
the default in the app (`ArtConfig::dns_mode = DnsMode::system`) and keep DoH as the user's alternative. Only
the app decides; nothing in the module silently falls from one resolver to another.

DoH privacy: the host names of the art servers (`image.api.playstation.com`, `cdn.jsdelivr.net`, ...) are sent
to Cloudflare (1.1.1.1) / Google (8.8.8.8); endpoints are configurable (`https://<IPv4 literal>/...` only).
The Settings text must say so.

## Using it

```cpp
#include "wings/art/art_service.hpp"
#include "wings/art/orbit_art_source.hpp"   // app builds only

wings::art::ArtConfig cfg;
cfg.cache_root      = "/download0/wings/art";                       // title-private; see risks
cfg.ca_bundle_paths = {"/download0/wings/ca-bundle.pem",            // user override first
                       "/app0/assets/ca-bundle.crt"};               // shipped
cfg.dns_mode        = settings.art_dns_mode;                        // module default off; the app (orbit::WingsPrefs::art_doh) defaults it on
cfg.heap_used       = [] { size_t live = 0; hui_heap_stats(&live, nullptr, nullptr, nullptr); return (std::uint64_t)live; };
cfg.log             = [](int level, const std::string &line) { hui::sys::log("%s", line.c_str()); };

wings::art::ArtService service(cfg);
service.start();                                                     // always returns; read service.state()
wings::art::OrbitArtSource source(service, resolver, &backend);      // resolver: key -> posterUrl
orbit::ArtCache art(source, textures);
```

Settings integration: `service.set_dns_mode(...)` (takes effect immediately, resets backoff),
`service.state()` (phase + one user-facing sentence + CA/clock/disk flags), `service.stats()`,
`service.clear_cache()`, `service.retry_failed()`.

## Integration checklist for the app (nothing here edits `wings/app`)

Build wiring
1. Stage the module: `bash wings/art/tools/stage-into-app.sh <app dir>` (add the call to
   `tools/build-wings-wsl.sh` / `tools/syncwork.sh` next to the `wings_core` copy; exclude
   `/app/src/platform/wings_art` from rsync `--delete` like `wings_core`). It puts sources + shims in
   `src/platform/wings_art/` (compiled by the console build; **excluded from preview and unit-test builds**
   by their `! -path src/platform/*` filters) and `ca-bundle.crt` in `assets/` (copied to `/app0/assets/`).
2. `app/Makefile`: `PACBREW_STATIC_ARCHIVES ?= lib/libwebp.a lib/libsharpyuv.a lib/libssl.a lib/libcrypto.a`
   (ssl before crypto) and add `src/platform/wings_art/include` to `APP_INCLUDE_PATHS`.
3. Shims (all in `shims/ps5_libc_shims.c`, picked up by step 1): `gmtime_r` (real, X.509 dates), `dladdr`,
   `sendmmsg`, `recvmmsg`, `_setjmp`, `_longjmp` (fail loudly; never reached), and
   `getaddrinfo/freeaddrinfo/gai_strerror/gethostbyname/getnameinfo` (fail cleanly; keeps
   `libScePosixForWebKit` out of `DT_NEEDED`). Verified: the app + module `DT_NEEDED` equals the stock app's.
4. Optional system-resolver experiment: build with `APP_DEFINITIONS=WINGS_ART_SCENET=1` and
   `APP_IMPORT_STUBS+=.deps/native/ps5-payload-sdk/target/lib/libSceNet.so` (adds `libSceNet.prx`).
   Do this only in a probe build first (`SCENET=1 tools/link-probe.sh`).

Exact app edits (after the wiring)
1. `src/main.cpp` (around `orbit::ArtCache art(backend, textures);`): build `ArtConfig` (above), create
   `ArtService` and `OrbitArtSource` before `ArtCache`, pass `source` instead of `backend`, call
   `service.stop()` after `art.stop()`. Resolver: look up `posterUrl` / hero URL for `pkg:<id>` in the current
   library snapshot (copy the map under the Store's lock; the resolver runs on the art worker thread). Keys the
   resolver does not know fall through to `&backend` (installed-game icons).
2. Settings (`orbit/settings.cpp`): a "Cover art" section: Off / DNS-over-HTTPS toggle (with the privacy
   sentence), a read-only line from `service.state().detail`, "Clear art cache" (`clear_cache`), "Retry failed
   covers" (`retry_failed`). Persist the DoH choice in the existing save file (`core/save_file`).
3. `orbit/art.cpp`: (a) bound `uploads_` to 8 with an atomic counter (docs/IMAGE_GUARD.md); (b) raise
   `kMaxQueued` 48 -> 96; (c) `kPendingDelay` 0.6 s -> 0.25 s (the pending poll is now cheap, nothing is
   re-fetched); (d) `limits_.retry_failed` stays 60 s (it now only hits the negative cache); (e) optional:
   `ArtLimits::max_covers` 64 -> 96 for 4K grids.
4. Grid screens: after layout, call `source.prefetch_keys(keys_of_rows_plus_minus_2, ArtKind::cover)`.
5. Optional idle fill (Settings toggle): `service.prefetch(all_urls, Kind::cover, Lane::idle_fill)`
   (about 35 s and 28 MiB for the 1,084-poster sample library).
6. `orbit/image.cpp`: apply `docs/IMAGE_GUARD.md` (4 Mpx / 4096 px before decode).
7. `THIRD-PARTY-NOTICES.md`: add OpenSSL 3.5.2 (Apache-2.0) and the Mozilla CA bundle (MPL-2.0).
8. Preview/fake backend: nothing; `FakeBackend` keeps implementing `ArtSource` directly.

## Tests

```
wsl -d Ubuntu -u root -- bash -lc 'ln -sfn "/mnt/c/path/to/this/repository" /root/wb'
wsl -d Ubuntu -u root -- bash /root/wb/wings/art/tools/run-tests.sh                 # hermetic
wsl -d Ubuntu -u root -- bash /root/wb/wings/art/tools/run-tests.sh --real --filter real   # real CDNs via DoH
wsl -d Ubuntu -u root -- bash /root/wb/wings/art/tools/link-probe.sh                # PS5 cross-link probe
SCENET=1 wsl ... link-probe.sh                                                      # + libSceNet variant
```

`run-tests.sh` builds the module with `-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic`,
generates a private PKI (`tests/gen_certs.sh`: CA, good, expired, wrong host, self-signed, untrusted CA),
starts TLS fixture servers on loopback and runs 261 checks: image probe/guard, URL rules, DNS TTL cache, DoH
wire format, disk cache (atomic, corruption, collision, LRU, negatives with a fake clock, restart), TLS client
(keep-alive, 1-3 redirects ok, 4/loop/http/ftp refused, caps in all three body modes, types, encodings,
stale pooled connection retry, total timeout, cancel), the safety suite (expired, wrong host, self-signed,
untrusted root, IP SAN, real CA store refusing the test CA, empty/garbage bundles, every private address
class, redirect into loopback, ambiguous hosts), service behaviour (dedupe, keep-alive across 41 URLs with
<= 4 connections, persistence, DNS off -> disabled but disk hits served, retry schedule, 60 s host breaker,
offline probing, CA refusal, heap pause, lane order), DoH end to end, and the adapter against the app's real
`orbit::ArtSource`.

## CA bundle

`ca-bundle.crt` is a byte-identical copy of `etc/ca-bundle.crt` from the pinned PacBrew sysroot
(`wings/app/.deps/pacbrew/v0.40.2/sysroot/user/homebrew/etc/ca-bundle.crt`; ports tarball `v0.40.2`,
sha256 `a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d` per
`tools/setup-pacbrew-dependencies.sh`). Contents: Mozilla roots extracted by curl.se `caextract`, "as of
Sun Aug 23 14:46:28 2026 GMT", 121 certificates, 188,905 bytes, sha256
`11163db462ffae3918afdf46c70a8db70cfcbaa8077352290b62a9c03c36fe6c`. License: MPL-2.0 (Mozilla data). To
refresh: bump the PacBrew pin and re-copy (or let the build copy it from the sysroot instead of committing it).
A user override at `/download0/wings/ca-bundle.pem` is tried first.

## Risks and things to check on the console

1. **No run on hardware yet.** Compiles with the PS5 toolchain, links, signs; `DT_NEEDED` identical to stock.
   Run the probe app (`tools/link-probe.sh`, folder `<probe>/app/dist/PPSA99178/`) and read `[WINGS-ART]` lines in
   `/mnt/sandbox/PPSA99178_000/download0/hui/dev/app.log`. Expected: `start=1 ... ca=121`, `DNS off` ->
   `state=3` (disabled), then DoH posters `state=0` with `decode: ok`, `second ask (disk)` ready,
   `expired.badssl.com` failed.
2. **Entropy:** `RAND_status()` must be 1 (otherwise `phase == entropy_error`). If not, seed from
   `libSceRandom`/`/dev/urandom` before `ArtService::start`.
3. **Writable cache dir:** `/download0/wings/art` from a title is assumed (log dir is there); the service
   keeps working memory-only (`disk_ok == false`, 8 MiB handoff) if not. Check `/data/wings` too.
4. **System DNS** (`SCENET=1` build): does `sceNetInit`/`sceNetResolverStartNtoa` work from a title, does it need
   `sceNetPoolCreate` sizes, does adding `libSceNet` to `DT_NEEDED` still load. Unverified.
5. **Clock:** a wrong console date makes every certificate "expired"; surfaced as `clock_suspect`.
6. **Threads:** console threads are FIFO; the workers block in `poll(250 ms)`/`SSL_read`, never spin, and the
   decode gate is the app's single `ArtCache` worker. Confirm Pegasus polling keeps its latency with 4 workers.
7. **Heap:** `heap_used` should be the app's `hui_heap_stats().live`. Art itself is ~5 MiB typical (docs).
8. **Interop:** thin client (no gzip/br/HTTP2). 1,088 real requests passed in the prototype; unusual catalog
   hosts may need libcurl (the `ArtSource` seam makes that a drop-in).
9. AVIF / animated WebP / GIF art stay placeholders (negative-cached with the reason).
