/* Wings update helper - build constants.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The helper is a small payload the Wings TV app sends to the console's ELF loader. It
 * downloads a Wings release and places it in /data/homebrew, where the TV app itself cannot
 * write. Its design follows Orbit Store 1.3.5's backend (tvapp.c, api.c, main.c),
 * GPL-3.0-or-later. */
#ifndef WINGS_HELPER_VERSION_H
#define WINGS_HELPER_VERSION_H

/* The one source of the helper's build stamp. The app includes this header
 * (kWingsHelperBuild in app/src/orbit/wings_helper_client.hpp), compares it with what a
 * running helper reports, and replaces any other helper. Bump it whenever the helper changes,
 * or a helper left running by an earlier package is taken for this one. Do not override it
 * with -DWINGS_HELPER_BUILD for one build only: the helper and the app must see the same value
 * (tests/unit/wings_helper_client_test.cpp checks the app side). */
#ifndef WINGS_HELPER_BUILD
#define WINGS_HELPER_BUILD "1.0.5"
#endif
/* 2: download and install are separate routes; install only places a downloaded file. */
#define WINGS_HELPER_API 2
#define WINGS_HELPER_PORT 9178
#define WINGS_HELPER_IDLE_SECONDS 120
/* Console threads may default to small stacks; curl and OpenSSL need headroom (Orbit main.c). */
#define WINGS_HELPER_STACK (1024 * 1024)

#endif
