# Third-party notices (Wings TV app)

Wings is free software under the GNU General Public License, version 3 or (at
your option) any later version. See `../LICENSE` and `../NOTICE` (credit to
Orbit Store, https://github.com/saawant12/orbit-store-ps5, GPL-3.0-or-later,
from which this app is forked).

These are the components the Wings app itself builds from or ships. Orbit's
payload, libmicrohttpd and React are NOT part of Wings. OpenSSL and the Mozilla
CA bundle are: Wings fetches https cover art itself (`wings/art`, below). The
Wings update helper (`helper/`, shipped as `assets/helper/wings-helper.elf`) is
a separate payload that installs Wings updates; it ports code from Orbit Store's
backend and statically links curl, OpenSSL and cJSON (below). Component versions
and hashes are those pinned by
`tools/setup-*.sh`, `tools/fetch-opengl-sdk.sh` and `Dockerfile`, unchanged from
Orbit Store 1.1.0 (see its `THIRD-PARTY-NOTICES.md` for the full table and the
source bundle).

| Component | Licence | Where |
|---|---|---|
| ps5-homebrew-ui kit (renderer, widgets, input, sound, PS5 platform layer, native build and FSELF tooling) | GPL-3.0-or-later; native tooling derived from SharpProspero (GPL-3.0) | `src/ui`, `src/gfx`, `src/audio`, `src/core`, `src/platform`, `src/runtime`, `tooling/` |
| ps5-opengl SDK 1.0.0 (OpenGL runtime; Mesa and OpenGNM PSBC components under their own licences) | GPL-3.0-or-later and bundled component licences | fetched into `.deps/` at build time, linked into `eboot.bin` |
| PS5 payload SDK headers and import stubs v0.42 | GPL-3.0-or-later, BSD-labelled files | fetched into `.deps/` at build time |
| libwebp 1.4.0 (PacBrew v0.40.2) | BSD-3-Clause with Google's patent grant | fetched at build time, linked |
| libc++, libc++abi, libunwind (LLVM 18.1.8) | Apache-2.0 with LLVM exceptions | linked |
| compiler-rt builtins (LLVM 18.1.3) | Apache-2.0 with LLVM exceptions | linked |
| stb_image, stb_vorbis, stb_image_write | Public domain or MIT | `src/third_party/stb/` (LICENSE alongside); stb_image_write is used by the PC preview only |
| Inter typeface 4.1 | SIL Open Font License 1.1 | `assets/fonts/*.huifont`, licence in `assets/fonts/Inter-LICENSE.txt` and `third_party/fonts/Inter-LICENSE.txt` |
| OpenSSL 3.5.2 (PacBrew ports v0.40.2; `libssl.a`, `libcrypto.a`) | Apache-2.0 | fetched at build time, statically linked into `eboot.bin` for the cover art TLS client. Licence text: https://github.com/openssl/openssl/blob/openssl-3.5.2/LICENSE.txt |
| Mozilla CA certificate bundle (curl.se `caextract`, 121 roots, as of 2026-08-23; sha256 `11163db4...36fe6c`) | MPL-2.0 (Mozilla data) | `assets/ca-bundle.crt` (copied from the pinned PacBrew sysroot by `wings/art/tools/stage-into-app.sh`); the player may override it with `/download0/wings/ca-bundle.pem` |
| Wings cover art module (`wings/art`: TLS client, DNS-over-HTTPS, disk cache, art service) | GPL-3.0-or-later (this project) | `src/platform/wings_art/`, console build only |
| Wings update helper (download, SHA-256 check and placement of Wings updates; local HTTP API on 127.0.0.1) | GPL-3.0-or-later; ports code from Orbit Store 1.3.5 `backend/tvapp.c` and `backend/api.c` (GPL-3.0-or-later), https://github.com/saawant12/orbit-store-ps5 | `helper/`, built into `assets/helper/wings-helper.elf` |
| cJSON 1.7.19 | MIT (licence text in `third_party/cjson/`) | `third_party/cjson/`, compiled into `wings-helper.elf` |
| curl / libcurl 8.18.0 (PacBrew ports v0.40.2) | curl licence (MIT/X style), https://curl.se/docs/copyright.html | fetched at build time, statically linked into `wings-helper.elf` |
| OpenSSL 3.5.2 (PacBrew ports v0.40.2) | Apache-2.0 | also statically linked into `wings-helper.elf` (HTTPS for update downloads) |
| libpsl, zstd, zlib (PacBrew ports v0.40.2, curl's dependencies) | MIT; BSD-3-Clause (zstd, dual-licensed with GPL-2.0); zlib licence | fetched at build time, statically linked into `wings-helper.elf` |
| Interface sound effects (glass set) | GPL-3.0-or-later (BlackBearReloaded, from the kit) | `assets/audio/sfx/glass/` |

Provenance of the glass sound set still needs confirming in the kit repository
before any public release.

The MPL-2.0 licence text for the CA bundle: https://www.mozilla.org/MPL/2.0/ .
The bundle is data, unmodified, and is not combined with Wings source files.

Pegasus DL is not part of Wings and is not covered by these licences.
