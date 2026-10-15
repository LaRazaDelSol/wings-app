/* Wings update helper - the loopback HTTP server.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The request guards follow Orbit Store 1.3.5 backend/api.c (GPL-3.0-or-later). */
#ifndef WINGS_HELPER_SERVER_H
#define WINGS_HELPER_SERVER_H

#include <stdint.h>

#define HELPER_SERVER_IN_USE (-2)

/* Listens on 127.0.0.1:port (0 picks a free port, for tests). Returns the socket,
 * HELPER_SERVER_IN_USE when another helper already owns the port, or -1. */
int helper_server_open(uint16_t port);
/* The port the socket is bound to. */
uint16_t helper_server_port(int fd);
/* Serves until a stop request, helper_server_request_stop(), or idle_seconds pass with no
 * request while nothing is being installed. Leaves fd open: the caller closes it after
 * helper_jobs_shutdown(), so no second helper starts while file work still runs. */
void helper_server_run(int fd, int idle_seconds);
void helper_server_request_stop(void);
/* Clears the stop flag (tests run the server more than once). */
void helper_server_reset(void);

#endif
