/* Wings update helper - the loopback HTTP server.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One request per connection, served in turn: the app polls status a few times a second and
 * the work runs on the install worker. The guards follow Orbit Store 1.3.5 backend/api.c
 * (GPL-3.0-or-later): a browser on the same console can reach 127.0.0.1, so a request must
 * name this helper's own Host (no DNS rebinding), carry no Origin, carry X-Wings-Helper: 1,
 * and send JSON on POST (a page cannot add those headers without a CORS preflight, which the
 * helper never answers). The download URL is pinned, so the most a hostile caller could do is
 * install a genuine Wings release. */
#include "server.h"
#include "install.h"
#include "version.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HEAD_LIMIT 8192
#define BODY_LIMIT 4096
#define ROUTE "/wings-helper/v1/"

static atomic_bool stopping;

void helper_server_request_stop(void)
{
    atomic_store(&stopping, true);
}
void helper_server_reset(void)
{
    atomic_store(&stopping, false);
}

int helper_server_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a))
    {
        int e = errno;
        close(fd);
        return e == EADDRINUSE ? HELPER_SERVER_IN_USE : -1;
    }
    if (listen(fd, 8))
    {
        close(fd);
        return -1;
    }
    return fd;
}
uint16_t helper_server_port(int fd)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &n))
        return 0;
    return ntohs(a.sin_port);
}

static void send_all(int fd, const char *data, size_t n)
{
    while (n)
    {
        ssize_t w = send(fd, data, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        data += w;
        n -= (size_t)w;
    }
}
static const char *reason_phrase(int code)
{
    switch (code)
    {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 415:
        return "Unsupported Media Type";
    case 431:
        return "Request Header Fields Too Large";
    case 507:
        return "Insufficient Storage";
    default:
        return "Internal Server Error";
    }
}
static void respond(int fd, int code, cJSON *body)
{
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    const char *payload = text ? text : "{}";
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                     code, reason_phrase(code), strlen(payload));
    send_all(fd, head, (size_t)n);
    send_all(fd, payload, strlen(payload));
    free(text);
}
static void respond_error(int fd, int code, const char *message, const char *reason)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    cJSON_AddStringToObject(o, "reason", reason);
    respond(fd, code, o);
}
/* Only this helper's own address: 127.0.0.1:<port> or localhost:<port>. */
static bool own_host(const char *host, uint16_t port)
{
    char expect[40];
    snprintf(expect, sizeof expect, "127.0.0.1:%u", (unsigned)port);
    if (!strcmp(host, expect))
        return true;
    snprintf(expect, sizeof expect, "localhost:%u", (unsigned)port);
    return !strcasecmp(host, expect);
}

typedef struct
{
    char method[8], path[64];
    const char *host, *origin, *marker, *type, *length, *encoding;
} Head;

/* Splits the request head in place. False: not a request this server reads. */
static bool parse_head(char *text, Head *h)
{
    memset(h, 0, sizeof *h);
    char *line_end = strstr(text, "\r\n");
    if (!line_end)
        return false;
    *line_end = 0;
    char version[16];
    if (sscanf(text, "%7s %63s %15s", h->method, h->path, version) != 3 ||
        strncmp(version, "HTTP/1.", 7))
        return false;
    for (char *line = line_end + 2; *line;)
    {
        char *end = strstr(line, "\r\n");
        if (!end)
            return false;
        *end = 0;
        char *colon = strchr(line, ':');
        if (!colon)
            return false;
        *colon = 0;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        for (char *t = value + strlen(value); t > value && (t[-1] == ' ' || t[-1] == '\t');)
            *--t = 0;
        const char **slot = !strcasecmp(line, "Host")              ? &h->host
                            : !strcasecmp(line, "Origin")          ? &h->origin
                            : !strcasecmp(line, "X-Wings-Helper")  ? &h->marker
                            : !strcasecmp(line, "Content-Type")    ? &h->type
                            : !strcasecmp(line, "Content-Length")  ? &h->length
                            : !strcasecmp(line, "Transfer-Encoding") ? &h->encoding
                                                                     : NULL;
        if (slot)
        {
            if (*slot)
                return false; /* a repeated header is never ours */
            *slot = value;
        }
        line = end + 2;
    }
    return true;
}

/* Serves one request. *trusted is set once the request passes the guards (only those keep the
 * helper from going idle). The whole request must arrive within REQUEST_SECONDS. */
#define REQUEST_SECONDS 5
static void handle(int fd, uint16_t port, bool *trusted)
{
    time_t deadline = time(NULL) + REQUEST_SECONDS;
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char buffer[HEAD_LIMIT + BODY_LIMIT + 1];
    size_t got = 0;
    char *head_end = NULL;
    while (!head_end)
    {
        if (got >= HEAD_LIMIT)
        {
            respond_error(fd, 431, "The request is too large.", "invalid");
            return;
        }
        if (time(NULL) >= deadline)
            return;
        ssize_t r = recv(fd, buffer + got, HEAD_LIMIT - got, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
        got += (size_t)r;
        buffer[got] = 0;
        head_end = strstr(buffer, "\r\n\r\n");
    }
    size_t head_length = (size_t)(head_end - buffer) + 4;
    head_end[2] = 0; /* keep the last header's CRLF for the parser */
    Head h;
    if (!parse_head(buffer, &h))
    {
        respond_error(fd, 400, "The request could not be read.", "invalid");
        return;
    }
    if (!h.host || !own_host(h.host, port) || h.origin || !h.marker || strcmp(h.marker, "1"))
    {
        respond_error(fd, 403, "This helper only answers the Wings app.", "forbidden");
        return;
    }
    *trusted = true;
    bool get = !strcmp(h.method, "GET"), post = !strcmp(h.method, "POST");
    if (strncmp(h.path, ROUTE, strlen(ROUTE)))
    {
        respond_error(fd, 404, "Not found.", "not_found");
        return;
    }
    const char *route = h.path + strlen(ROUTE);
    bool status = !strcmp(route, "status");
    bool known = status || !strcmp(route, "download") || !strcmp(route, "install") ||
                 !strcmp(route, "cancel") ||
                 !strcmp(route, "stop") || !strcmp(route, "tidy");
    if (!known)
    {
        respond_error(fd, 404, "Not found.", "not_found");
        return;
    }
    if ((status && !get) || (!status && !post))
    {
        respond_error(fd, 405, "Method not allowed.", "invalid");
        return;
    }
    cJSON *input = NULL;
    if (post)
    {
        if (h.encoding)
        {
            respond_error(fd, 411, "Send a Content-Length.", "invalid");
            return;
        }
        if (!h.type || strncasecmp(h.type, "application/json", 16) ||
            (h.type[16] && h.type[16] != ';'))
        {
            respond_error(fd, 415, "Send JSON.", "invalid");
            return;
        }
        char *end = NULL;
        unsigned long length = h.length ? strtoul(h.length, &end, 10) : 0;
        if (!h.length || !*h.length || *end || h.length[0] == '-')
        {
            respond_error(fd, 411, "Send a Content-Length.", "invalid");
            return;
        }
        if (length > BODY_LIMIT)
        {
            respond_error(fd, 413, "The request body is too large.", "invalid");
            return;
        }
        size_t want = head_length + length;
        while (got < want)
        {
            if (time(NULL) >= deadline)
                return;
            ssize_t r = recv(fd, buffer + got, want - got, 0);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                return;
            got += (size_t)r;
        }
        buffer[want] = 0;
        input = length ? cJSON_Parse(buffer + head_length) : cJSON_CreateObject();
        if (!cJSON_IsObject(input))
        {
            cJSON_Delete(input);
            respond_error(fd, 400, "The request body isn't a JSON object.", "invalid");
            return;
        }
    }
    char error[256] = {0};
    const char *reason = "invalid";
    int code = 200;
    if (status)
        respond(fd, 200, helper_status());
    else if (!strcmp(route, "download") || !strcmp(route, "install"))
    {
        code = !strcmp(route, "download") ? helper_download(input, error, sizeof error, &reason)
                                          : helper_install(input, error, sizeof error, &reason);
        if (code == 202)
            respond(fd, 202, helper_status());
        else
            respond_error(fd, code, error, reason);
    }
    else if (!strcmp(route, "cancel"))
        respond(fd, helper_cancel(), helper_status());
    else if (!strcmp(route, "tidy"))
    {
        code = helper_tidy(input, error, sizeof error, &reason);
        if (code == 200)
            respond(fd, 200, helper_status());
        else
            respond_error(fd, code, error, reason);
    }
    else if (helper_renaming())
        respond_error(fd, 409, "Wings is being moved into place. Try again in a moment.",
                      "busy");
    else
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "stopping", true);
        respond(fd, 200, o);
        helper_server_request_stop();
    }
    cJSON_Delete(input);
}

static bool peer_is_loopback(const struct sockaddr_in *peer)
{
    return peer->sin_family == AF_INET && peer->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
}

void helper_server_run(int fd, int idle_seconds)
{
    uint16_t port = helper_server_port(fd);
    time_t last = time(NULL);
    while (!atomic_load(&stopping))
    {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int ready = poll(&p, 1, 500);
        time_t now = time(NULL);
        if (helper_busy())
            last = now;
        if (ready < 0 && errno != EINTR)
            break;
        if (ready <= 0)
        {
            if (now - last >= idle_seconds)
                break;
            continue;
        }
        struct sockaddr_in peer;
        socklen_t n = sizeof peer;
        int client = accept(fd, (struct sockaddr *)&peer, &n);
        if (client < 0)
            continue;
        bool trusted = false;
        if (peer_is_loopback(&peer))
            handle(client, port, &trusted);
        close(client);
        if (trusted)
            last = time(NULL);
    }
    /* fd stays open: the port is the single-instance lock until the caller has finished all
     * file work (helper_jobs_shutdown) and closes it. */
}
