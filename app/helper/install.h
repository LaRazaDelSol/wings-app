/* Wings update helper - download, check and place a Wings release.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from Orbit Store 1.3.5 backend/tvapp.c (GPL-3.0-or-later). */
#ifndef WINGS_HELPER_INSTALL_H
#define WINGS_HELPER_INSTALL_H

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

/* Starts the worker thread. root prefixes every console path ("" on the console; a temporary
 * directory in the host tests). 0 on success. */
int helper_jobs_init(const char *root);
/* Interrupts a download and joins the worker. */
void helper_jobs_shutdown(void);

/* The status object of GET /wings-helper/v1/status (caller frees). */
cJSON *helper_status(void);

/* The routes' work. Each returns the HTTP status; on a refusal error holds the message and
 * *reason a short machine word (busy, invalid, folder, blocked, newer, manual, old_waiting,
 * space, not_running, not_downloaded).
 * download fetches the release to the .part, checks it and records it (phase "downloaded").
 * install only places a downloaded, re-checked .part; it never uses the network. */
int helper_download(const cJSON *input, char *error, size_t cap, const char **reason);
int helper_install(const cJSON *input, char *error, size_t cap, const char **reason);
int helper_cancel(void);
int helper_tidy(const cJSON *input, char *error, size_t cap, const char **reason);

bool helper_busy(void);
/* True only between the two renames; stopping then could leave no PPSA99178.ffpkg. */
bool helper_renaming(void);

/* -1, 0 or 1 for dotted numeric versions ("1.2" < "1.10"). */
int helper_version_compare(const char *a, const char *b);

#endif
