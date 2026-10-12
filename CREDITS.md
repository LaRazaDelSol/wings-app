# Credits

Wings stands on other people's work. Thank you. Credits are checked against each project's own repository; where an individual author is not named there, the project or organisation account is credited instead of guessing a name.

## Orbit Store (the foundation)

Wings is a derivative work of the native TV app of **Orbit Store** by **saawant12**.

- Project: https://github.com/saawant12/orbit-store-ps5
- Forked from: Orbit Store 1.1.0 (TV app 1.4.0), source commit `987022e51eaf925f38093b44f5c24f5e69f57cc9`
- License: GPL-3.0-or-later. Wings keeps the same license.

What Wings kept from Orbit: the app shell, the screen and navigation model, the on-screen keyboard and prompts, the download-queue and library screen structure, the build and packaging pipeline, and the visual language. What Wings replaced: everything that talks to a backend (Wings talks only to Pegasus DL), the Store, Files, Settings and search screens, link grouping and ranking, and all names, art and icons. Orbit's payload, loader launch, pairing, ShadowMount library client, Zero companion and update feeds are not part of Wings. Files that were rewritten say in their first lines which Orbit file they were adapted from, and the original `SPDX-License-Identifier` headers are kept. Parts of the Pegasus HTTP client (`core/src/http.cpp`) and the JSON reader (`core/src/json.cpp`) were adapted from Orbit's `http.cpp` and `json.cpp`.

## The UI kit and platform layer

- **ps5-homebrew-ui** and **ps5-native-app-boilerplate**, Copyright (C) 2026 **BlackBearReloaded**, GPL-3.0-or-later: the renderer, widgets, input, sound and PS5 platform layer. https://github.com/blackbearreloaded/ps5-homebrew-ui
- **ps5-opengl** SDK 1.0.0 (BlackBearReloaded), with Mesa and OpenGNM PSBC components under their own licenses. https://github.com/blackbearreloaded/ps5-opengl
- **SharpProspero** by **SvenGDK** (GPL-3.0), from which the kit's native build and FSELF tooling is derived. https://github.com/SvenGDK/SharpProspero
- **ps5-payload-dev**: the PS5 payload SDK (headers and import stubs v0.42) and the PacBrew ports repository. https://github.com/ps5-payload-dev

## The program Wings is a front end for

**Pegasus DL** is a separate, closed-source program published by the **pegasus-ps5** account (https://github.com/pegasus-ps5/pegasus-dl); no individual author is named there. Wings talks to it only through its local HTTP API (tested against Pegasus 1.11.0) and is not affiliated with it. Pegasus is not copied, bundled or redistributed here, and it is not covered by Wings' license. Only when you ask, Wings downloads the official release file from its github.com release page and checks it against the checksum GitHub publishes for it. Thank you to the Pegasus DL developer(s): it is the most capable download manager available and Wings exists to make it comfortable on a TV.

## Required tools (not part of Wings)

Running Wings needs a PS5 set up with these separate projects, which have their own authors and licenses; Wings does not include, modify or distribute them:

- **ShadowMountPlus** by **drakmor** (GPL-3.0): mounts and registers the package. Wings talks to its local API on 127.0.0.1 only to ask it to scan after a download and to read its game list; it does not bundle that program. https://github.com/drakmor/ShadowMountPlus
- **kstuff** / **kstuff-lite**: kstuff is published under https://github.com/ps5-payload-dev/kstuff and kstuff-lite by **EchoStretch** at https://github.com/EchoStretch/kstuff-lite. Neither repository names the original author of kstuff, so see them for authorship and license.
- **ps5-payload-dev elfldr**: the ELF loader that listens on port 9021, which Wings uses only to send Pegasus-dl.elf when you ask. https://github.com/ps5-payload-dev

## Libraries and assets

| Component | Version | License |
|---|---|---|
| libwebp (PacBrew v0.40.2) | 1.4.0 | BSD-3-Clause with Google's patent grant |
| LLVM libc++, libc++abi, libunwind | 18.1.8 | Apache-2.0 with LLVM exceptions |
| compiler-rt builtins | 18.1.3 | Apache-2.0 with LLVM exceptions |
| OpenSSL (PacBrew ports v0.40.2), statically linked for the cover art TLS client | 3.5.2 | Apache-2.0 |
| Mozilla CA certificate bundle (curl.se `caextract`, 121 roots, as of 2026-08-23) | n/a | MPL-2.0 (data, unmodified) |
| stb_image, stb_vorbis, stb_image_write (stb_image_write: PC preview only) | see `src/third_party/stb/` | Public domain or MIT |
| Inter typeface | 4.1 | SIL Open Font License 1.1 |
| Interface sound effects, "glass" set | n/a | Generated sounds, original to this project (no third-party recordings); GPL-3.0-or-later |

The full table, with where each component lives in the tree and the licence text locations, is in [app/THIRD-PARTY-NOTICES.md](app/THIRD-PARTY-NOTICES.md). Keep `assets/fonts/Inter-LICENSE.txt` and `third_party/fonts/Inter-LICENSE.txt` with the fonts.

## Popularity data

The optional popularity file is built weekly by a separate public repository (`LaRazaDelSol/wings-app`, served through GitHub Pages) from these sources, each credited where the data is shown:

- **IGDB.com**: "Ratings and popularity data from IGDB.com". Used under the Twitch Developer Services Agreement; permission to republish derived scores has been confirmed with IGDB by the project owner.
- **Steam** (Valve): review summaries, only the percent positive and the review count. Wings and this project are not affiliated with Valve.
- **Wikimedia**: Wikipedia pageview data, Wikimedia Foundation analytics datasets (CC0).

Matching is by PlayStation title ID. Low-confidence matches are hidden.

## Thanks

- **saawant12**, for Orbit Store: the TV app Wings began as, and the design language that made a controller-first PS5 app feel right. https://github.com/saawant12/orbit-store-ps5
- The **Pegasus DL** developer(s), for the download manager Wings is a front end for.
- **phantomptr**, for PS5Upload, a great development environment while building and testing Wings. https://github.com/phantomptr/ps5upload
- **BlackBearReloaded**, for the UI kit, boilerplate and tooling.
- **drakmor**, for ShadowMountPlus, and **ps5-payload-dev** and **SvenGDK**, for the SDK and toolchain foundations.

## Name and art

Wings' name, mark, icon and preview artwork are original. Orbit Store's name, logo, icon and artwork are not used. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment; Wings is not affiliated with or endorsed by Sony.

---

If you wrote something Wings builds on and the credit here is missing or wrong, please open an issue and it will be fixed.
