/* Wings update helper - entry point.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Started by the Wings TV app through the console's ELF loader. Serves 127.0.0.1:9178 until
 * the app says stop or nothing has asked for two minutes while no update runs. A second copy
 * finds the port taken and exits at once, leaving the running one in charge. The start-up
 * order follows Orbit Store 1.3.5 backend/main.c (GPL-3.0-or-later). */
#include "install.h"
#include "server.h"
#include "version.h"
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void stop_signal(int signal_number)
{
    (void)signal_number;
    helper_server_request_stop();
}

int main(void)
{
    const char *root = "";
#ifdef WINGS_HELPER_TEST
    if (getenv("WINGS_HELPER_ROOT"))
        root = getenv("WINGS_HELPER_ROOT");
#endif
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    int fd = helper_server_open(WINGS_HELPER_PORT);
    if (fd == HELPER_SERVER_IN_USE)
        return 0; /* the helper already running wins */
    if (fd < 0)
    {
        fprintf(stderr, "Wings helper: cannot listen on port %d.\n", WINGS_HELPER_PORT);
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) || helper_jobs_init(root))
    {
        fprintf(stderr, "Wings helper: cannot start.\n");
        return 1;
    }
    printf("Wings helper %s | port %d\n", WINGS_HELPER_BUILD, WINGS_HELPER_PORT);
    helper_server_run(fd, WINGS_HELPER_IDLE_SECONDS);
    helper_jobs_shutdown(); /* joins the worker: all file work is done */
    close(fd);              /* only now may another helper take the port */
    curl_global_cleanup();
    return 0;
}
