/* Wings update helper - host tests against a fake GitHub on 127.0.0.1.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built with -DWINGS_HELPER_TEST (see `make helper-test`): WINGS_HELPER_TEST_BASE points the
 * pinned release URL at the fake server, as Orbit Store's ORBIT_TEST_UPDATE_BASE does, and
 * every console path lives under a temporary root. */
#include "install.h"
#include "server.h"
#include "version.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IMAGE_SIZE 70000
#define MAGIC_AT (65536 + 1372)

static int failures;
#define CHECK(cond)                                                                            \
    do                                                                                         \
    {                                                                                          \
        if (!(cond))                                                                           \
        {                                                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                                        \
        }                                                                                      \
    } while (0)

static unsigned char image[IMAGE_SIZE], bad_image[IMAGE_SIZE];
static char image_sha[65], bad_sha[65];
static uint16_t fake_port, helper_port;
static char root[256];

/* ---- fake GitHub ---- */

static void sha_hex(const unsigned char *data, size_t n, char *out)
{
    unsigned char d[32];
    unsigned len = 0;
    EVP_Digest(data, n, d, &len, EVP_sha256(), NULL);
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}
static void send_text(int fd, const char *text)
{
    send(fd, text, strlen(text), MSG_NOSIGNAL);
}
static void send_body(int fd, const unsigned char *data, size_t n, int slow)
{
    char head[160];
    snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
             n);
    send_text(fd, head);
    for (size_t p = 0; p < n;)
    {
        size_t chunk = slow ? 1000 : n - p;
        if (send(fd, data + p, chunk, MSG_NOSIGNAL) <= 0)
            return;
        p += chunk;
        if (slow)
            usleep(50000);
    }
}
static void *fake_client(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[2048] = {0}, path[512] = {0};
    size_t got = 0;
    while (got < sizeof req - 1 && !strstr(req, "\r\n\r\n"))
    {
        ssize_t r = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    sscanf(req, "GET %511s", path);
    char head[512];
    if (!strcmp(path, "/asset/good"))
        send_body(fd, image, sizeof image, 0);
    else if (!strcmp(path, "/asset/slow"))
        send_body(fd, image, sizeof image, 1);
    else if (!strcmp(path, "/download/v9.1/PPSA99178.ffpkg"))
        send_body(fd, bad_image, sizeof bad_image, 0);
    else if (!strcmp(path, "/download/v9.3/PPSA99178.ffpkg"))
    {
        snprintf(head, sizeof head,
                 "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%u/asset/slow\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
                 (unsigned)fake_port);
        send_text(fd, head);
    }
    else if (!strcmp(path, "/download/v9.2/PPSA99178.ffpkg"))
    {
        snprintf(head, sizeof head,
                 "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.2:%u/asset/good\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
                 (unsigned)fake_port);
        send_text(fd, head);
    }
    else if (!strncmp(path, "/download/", 10))
    {
        snprintf(head, sizeof head,
                 "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%u/asset/good\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
                 (unsigned)fake_port);
        send_text(fd, head);
    }
    else
        send_text(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    close(fd);
    return NULL;
}
static void *fake_server(void *arg)
{
    int fd = (int)(intptr_t)arg;
    for (;;)
    {
        int c = accept(fd, NULL, NULL);
        if (c < 0)
            continue;
        pthread_t t;
        pthread_create(&t, NULL, fake_client, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    return NULL;
}

/* ---- helper client ---- */

typedef struct
{
    int code;
    char body[4096];
} Reply;

static Reply raw_request(const char *text)
{
    Reply r = {0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(helper_port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a))
    {
        close(fd);
        return r;
    }
    send_text(fd, text);
    char buf[8192] = {0};
    size_t got = 0;
    for (;;)
    {
        ssize_t n = recv(fd, buf + got, sizeof buf - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    sscanf(buf, "HTTP/1.1 %d", &r.code);
    const char *body = strstr(buf, "\r\n\r\n");
    if (body)
        snprintf(r.body, sizeof r.body, "%s", body + 4);
    return r;
}
static Reply call(const char *method, const char *route, const char *body)
{
    char text[4096];
    if (!strcmp(method, "GET"))
        snprintf(text, sizeof text,
                 "GET /wings-helper/v1/%s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nX-Wings-Helper: 1\r\n"
                 "Connection: close\r\n\r\n",
                 route, (unsigned)helper_port);
    else
        snprintf(text, sizeof text,
                 "POST /wings-helper/v1/%s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nX-Wings-Helper: 1\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                 route, (unsigned)helper_port, strlen(body), body);
    return raw_request(text);
}
static cJSON *status(void)
{
    Reply r = call("GET", "status", "");
    return cJSON_Parse(r.body);
}
static const char *text_of(const cJSON *o, const char *key)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(i) ? i->valuestring : "";
}
/* Waits for the worker to finish; returns the final status. */
static cJSON *wait_done(void)
{
    for (int i = 0; i < 400; i++)
    {
        cJSON *s = status();
        if (s && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "busy")))
            return s;
        cJSON_Delete(s);
        usleep(25000);
    }
    return status();
}
static Reply request(const char *route, const char *version, const char *tag, const char *sha,
                     long size, const char *extra)
{
    char body[1024];
    snprintf(body, sizeof body,
             "{\"version\":\"%s\",\"url\":\"http://127.0.0.1:%u/download/%s/PPSA99178.ffpkg\","
             "\"sha256\":\"%s\",\"size\":%ld%s}",
             version, (unsigned)fake_port, tag, sha, size, extra);
    return call("POST", route, body);
}
static Reply download(const char *version, const char *tag, const char *sha, long size,
                      const char *extra)
{
    return request("download", version, tag, sha, size, extra);
}
static Reply install(const char *version, const char *tag, const char *sha, long size,
                     const char *extra)
{
    return request("install", version, tag, sha, size, extra);
}
/* Downloads, then (once "downloaded") installs, and expects the given final phase. */
static void install_expect(const char *version, const char *tag, const char *sha, long size,
                           const char *extra, const char *phase)
{
    Reply r = download(version, tag, sha, size, extra);
    CHECK(r.code == 202);
    cJSON *s = wait_done();
    if (!strcmp(text_of(s, "phase"), "downloaded") && strcmp(phase, "downloaded"))
    {
        CHECK(!strcmp(text_of(s, "version"), version));
        cJSON_Delete(s);
        r = install(version, tag, sha, size, extra);
        CHECK(r.code == 202);
        s = wait_done();
    }
    if (strcmp(text_of(s, "phase"), phase))
        fprintf(stderr, "  %s %s -> %s (%s)\n", version, tag, text_of(s, "phase"),
                text_of(s, "error"));
    CHECK(!strcmp(text_of(s, "phase"), phase));
    cJSON_Delete(s);
}
static bool exists(const char *rel)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s%s", root, rel);
    return !lstat(p, &st);
}
static void write_file(const char *rel, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s%s", root, rel);
    FILE *f = fopen(p, "wb");
    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
}
static void remove_file(const char *rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s%s", root, rel);
    unlink(p);
}
static void *run_server(void *arg)
{
    helper_server_run((int)(intptr_t)arg, 30);
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (size_t i = 0; i < sizeof image; i++)
        image[i] = bad_image[i] = (unsigned char)(i * 31 + 7);
    const unsigned char magic[4] = {0x19, 0x01, 0x54, 0x19};
    memcpy(image + MAGIC_AT, magic, 4);
    sha_hex(image, sizeof image, image_sha);
    sha_hex(bad_image, sizeof bad_image, bad_sha);

    int fake = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t n = sizeof a;
    if (bind(fake, (struct sockaddr *)&a, sizeof a) || listen(fake, 16) ||
        getsockname(fake, (struct sockaddr *)&a, &n))
        return 2;
    fake_port = ntohs(a.sin_port);
    pthread_t fake_thread;
    pthread_create(&fake_thread, NULL, fake_server, (void *)(intptr_t)fake);
    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%u", (unsigned)fake_port);
    setenv("WINGS_HELPER_TEST_BASE", base, 1);

    snprintf(root, sizeof root, "/tmp/wings-helper-test-XXXXXX");
    if (!mkdtemp(root))
        return 2;
    char data[300];
    snprintf(data, sizeof data, "%s/data", root);
    mkdir(data, 0755);
    CHECK(helper_jobs_init(root) == 0);

    int fd = helper_server_open(0);
    CHECK(fd >= 0);
    helper_port = helper_server_port(fd);
    /* Single instance: the port is taken while the first one listens. */
    CHECK(helper_server_open(helper_port) == HELPER_SERVER_IN_USE);
    pthread_t server_thread;
    pthread_create(&server_thread, NULL, run_server, (void *)(intptr_t)fd);

    /* Guards. */
    char text[512];
    snprintf(text, sizeof text,
             "GET /wings-helper/v1/status HTTP/1.1\r\nHost: evil.example:%u\r\n"
             "X-Wings-Helper: 1\r\n\r\n",
             (unsigned)helper_port);
    CHECK(raw_request(text).code == 403);
    snprintf(text, sizeof text,
             "GET /wings-helper/v1/status HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
             (unsigned)helper_port);
    CHECK(raw_request(text).code == 403);
    snprintf(text, sizeof text,
             "GET /wings-helper/v1/status HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nX-Wings-Helper: 1\r\n"
             "Origin: http://127.0.0.1:%u\r\n\r\n",
             (unsigned)helper_port, (unsigned)helper_port);
    CHECK(raw_request(text).code == 403);
    snprintf(text, sizeof text,
             "POST /wings-helper/v1/cancel HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nX-Wings-Helper: 1\r\n"
             "Content-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}",
             (unsigned)helper_port);
    CHECK(raw_request(text).code == 415);
    CHECK(call("GET", "nothing", "").code == 404);
    CHECK(call("POST", "status", "{}").code == 405);

    cJSON *s = status();
    CHECK(!strcmp(text_of(s, "phase"), "idle"));
    CHECK(!strcmp(text_of(s, "target"), "none"));
    CHECK(!strcmp(text_of(s, "helper"), WINGS_HELPER_BUILD));
    cJSON_Delete(s);

    /* Invalid requests. */
    CHECK(download("1.2", "v1.2", "abc", IMAGE_SIZE, "").code == 400);
    CHECK(download("1.2", "v1.2", image_sha, 10, "").code == 400);
    CHECK(download("1.2", "v1.2", image_sha, IMAGE_SIZE, ",\"replace\":\"yes\"").code == 400);
    char body[512];
    snprintf(body, sizeof body,
             "{\"version\":\"1.2\",\"url\":\"https://example.com/download/v1.2/PPSA99178.ffpkg\","
             "\"sha256\":\"%s\",\"size\":%d}",
             image_sha, IMAGE_SIZE);
    CHECK(call("POST", "download", body).code == 400);
    CHECK(install("1.2", "v1.2", "abc", IMAGE_SIZE, "").code == 400);

    /* Failures leave nothing behind. */
    install_expect("1.2", "v1.2", image_sha, IMAGE_SIZE + 1, "", "error");
    install_expect("1.2", "v1.2", bad_sha, IMAGE_SIZE, "", "error");
    install_expect("9.1", "v9.1", bad_sha, IMAGE_SIZE, "", "error");
    install_expect("9.2", "v9.2", image_sha, IMAGE_SIZE, "", "error");
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg"));
    CHECK(!exists("/data/wings-helper/PPSA99178.ffpkg.part"));

    /* A good install. */
    install_expect("1.2", "v1.2", image_sha, IMAGE_SIZE, "", "placed");
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "wings"));
    CHECK(!strcmp(text_of(s, "installed_version"), "1.2"));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(s, "old_kept")));
    CHECK(cJSON_GetObjectItemCaseSensitive(s, "received")->valuedouble == IMAGE_SIZE);
    cJSON_Delete(s);

    /* Never downgrade. */
    CHECK(download("1.1", "v1.1", image_sha, IMAGE_SIZE, "").code == 409);
    CHECK(download("1.3", "v1.3", image_sha, IMAGE_SIZE, ",\"running\":\"1.4\"").code == 409);

    /* An update keeps the previous file until the new version runs. */
    install_expect("1.3", "v1.3", image_sha, IMAGE_SIZE, ",\"running\":\"1.2\"", "placed");
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg.old"));
    s = status();
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "old_kept")));
    CHECK(!strcmp(text_of(s, "installed_version"), "1.3"));
    cJSON_Delete(s);
    CHECK(download("1.4", "v1.4", image_sha, IMAGE_SIZE, "").code == 409); /* old_waiting */
    CHECK(call("POST", "tidy", "{\"running\":\"1.2\"}").code == 409);
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg.old"));
    CHECK(call("POST", "tidy", "{\"running\":\"1.3\"}").code == 200);
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));

    /* A copy the player placed is replaced only when they confirm. */
    remove_file("/data/wings-helper/helper-placed.json");
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "manual"));
    cJSON_Delete(s);
    install_expect("1.4", "v1.4", image_sha, IMAGE_SIZE, "", "downloaded");
    CHECK(install("1.4", "v1.4", image_sha, IMAGE_SIZE, "").code == 409);
    install_expect("1.4", "v1.4", image_sha, IMAGE_SIZE, ",\"replace\":true", "placed");
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg.old"));
    CHECK(call("POST", "tidy", "{\"running\":\"1.4\"}").code == 200);
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));

    /* The version asked for must be the release in the URL ("v" prefix or not). */
    CHECK(download("1.5", "v1.6", image_sha, IMAGE_SIZE, "").code == 400);
    CHECK(download("1.5", "1.6", image_sha, IMAGE_SIZE, "").code == 400);
    CHECK(download("99.0", "v1.5", image_sha, IMAGE_SIZE, "").code == 400);

    /* The last step of placing fails: the installed package and its record stay as they were,
     * and no .old is left behind. */
    setenv("WINGS_HELPER_TEST_FAIL_PLACE", "1", 1);
    install_expect("1.5", "v1.5", image_sha, IMAGE_SIZE, ",\"running\":\"1.4\"", "error");
    unsetenv("WINGS_HELPER_TEST_FAIL_PLACE");
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg"));
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));
    /* The checked download stays, so installing can be tried again. */
    CHECK(exists("/data/wings-helper/PPSA99178.ffpkg.part"));
    CHECK(exists("/data/wings-helper/downloaded.json"));
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "wings"));
    CHECK(!strcmp(text_of(s, "installed_version"), "1.4"));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(s, "old_kept")));
    cJSON_Delete(s);

    /* An older helper stopped between its two renames: only the .old is there. The next
     * request puts it back, still recognised as the helper's install. */
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/data/homebrew/PPSA99178.ffpkg", root);
    snprintf(to, sizeof to, "%s/data/homebrew/PPSA99178.ffpkg.old", root);
    CHECK(!rename(from, to));
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "none"));
    cJSON_Delete(s);
    CHECK(call("POST", "tidy", "{\"running\":\"1.4\"}").code == 200);
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg"));
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "wings"));
    CHECK(!strcmp(text_of(s, "installed_version"), "1.4"));
    cJSON_Delete(s);
    /* The same at helper start. */
    CHECK(!rename(from, to));
    helper_jobs_shutdown();
    CHECK(helper_jobs_init(root) == 0);
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg"));
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));

    /* Never beside a folder copy. */
    char folder[400];
    snprintf(folder, sizeof folder, "%s/data/homebrew/PPSA99178", root);
    mkdir(folder, 0755);
    write_file("/data/homebrew/PPSA99178/eboot.bin", "x");
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "folder"));
    cJSON_Delete(s);
    CHECK(download("1.5", "v1.5", image_sha, IMAGE_SIZE, "").code == 409);
    remove_file("/data/homebrew/PPSA99178/eboot.bin");
    rmdir(folder);

    /* Busy, then cancel. */
    CHECK(download("9.3", "v9.3", image_sha, IMAGE_SIZE, "").code == 202);
    usleep(300000);
    CHECK(download("1.5", "v1.5", image_sha, IMAGE_SIZE, "").code == 409);
    s = status();
    CHECK(!strcmp(text_of(s, "phase"), "downloading"));
    CHECK(cJSON_GetObjectItemCaseSensitive(s, "received")->valuedouble > 0);
    cJSON_Delete(s);
    CHECK(call("POST", "cancel", "{}").code == 200);
    s = wait_done();
    CHECK(!strcmp(text_of(s, "phase"), "idle"));
    CHECK(!strcmp(text_of(s, "installed_version"), "1.4"));
    cJSON_Delete(s);
    CHECK(!exists("/data/wings-helper/PPSA99178.ffpkg.part"));
    CHECK(!exists("/data/wings-helper/downloaded.json"));

    /* Install without a download: refused, nothing placed. */
    Reply r = install("1.5", "v1.5", image_sha, IMAGE_SIZE, "");
    CHECK(r.code == 409 && strstr(r.body, "not_downloaded"));
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));

    /* A download stops at "downloaded" and survives a helper restart. */
    install_expect("1.5", "v1.5", image_sha, IMAGE_SIZE, "", "downloaded");
    CHECK(exists("/data/wings-helper/PPSA99178.ffpkg.part"));
    CHECK(exists("/data/wings-helper/downloaded.json"));
    s = status();
    CHECK(!strcmp(text_of(s, "installed_version"), "1.4"));
    cJSON_Delete(s);
    helper_jobs_shutdown();
    CHECK(helper_jobs_init(root) == 0);
    s = status();
    CHECK(!strcmp(text_of(s, "phase"), "downloaded"));
    CHECK(!strcmp(text_of(s, "version"), "1.5"));
    cJSON_Delete(s);
    /* Install only places what was downloaded: another version or checksum is refused. */
    CHECK(install("1.6", "v1.6", image_sha, IMAGE_SIZE, "").code == 409);
    CHECK(install("1.5", "v1.5", bad_sha, IMAGE_SIZE, "").code == 409);

    /* The .part changed after the download (same size, inode and time): install hashes it
     * again, refuses, and nothing is placed. */
    char part_path[512];
    snprintf(part_path, sizeof part_path, "%s/data/wings-helper/PPSA99178.ffpkg.part", root);
    struct stat before;
    CHECK(!stat(part_path, &before));
    int pf = open(part_path, O_WRONLY);
    CHECK(pf >= 0 && pwrite(pf, "X", 1, 100) == 1);
    close(pf);
    struct timespec times[2] = {before.st_atim, before.st_mtim};
    CHECK(!utimensat(AT_FDCWD, part_path, times, 0));
    s = status();
    CHECK(!strcmp(text_of(s, "phase"), "downloaded"));
    cJSON_Delete(s);
    CHECK(install("1.5", "v1.5", image_sha, IMAGE_SIZE, "").code == 202);
    s = wait_done();
    CHECK(!strcmp(text_of(s, "phase"), "error"));
    CHECK(!strcmp(text_of(s, "installed_version"), "1.4"));
    cJSON_Delete(s);
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));
    CHECK(!exists("/data/wings-helper/PPSA99178.ffpkg.part"));
    CHECK(!exists("/data/wings-helper/downloaded.json"));

    /* An install finished, then the player put another copy in by hand (the console history of
     * 2026-10-14): the kept .old is no longer the way back and is tidied, even though the copy
     * running is older than the release the helper placed. */
    install_expect("1.5", "v1.5", image_sha, IMAGE_SIZE, ",\"running\":\"1.4\"", "placed");
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg.old"));
    CHECK(download("1.6", "v1.6", image_sha, IMAGE_SIZE, "").code == 409); /* old_waiting */
    remove_file("/data/homebrew/PPSA99178.ffpkg");
    write_file("/data/homebrew/PPSA99178.ffpkg", "a copy placed by hand");
    s = status();
    CHECK(!strcmp(text_of(s, "target"), "manual"));
    cJSON_Delete(s);
    CHECK(call("POST", "tidy", "{\"running\":\"1.1\"}").code == 200);
    CHECK(!exists("/data/homebrew/PPSA99178.ffpkg.old"));
    CHECK(exists("/data/homebrew/PPSA99178.ffpkg"));

    /* Stop ends the server. */
    CHECK(call("POST", "stop", "{}").code == 200);
    pthread_join(server_thread, NULL);
    /* The port stays taken until the worker has finished; then main closes it. */
    CHECK(helper_server_open(helper_port) == HELPER_SERVER_IN_USE);
    helper_jobs_shutdown();
    close(fd);
    CHECK(call("GET", "status", "").code == 0);

    /* Idle exit. */
    helper_server_reset();
    fd = helper_server_open(0);
    time_t started = time(NULL);
    helper_server_run(fd, 1);
    CHECK(time(NULL) - started <= 3);
    close(fd);

    char cleanup[400];
    snprintf(cleanup, sizeof cleanup, "rm -rf '%s'", root);
    if (system(cleanup))
        fprintf(stderr, "could not remove %s\n", root);
    if (failures)
    {
        fprintf(stderr, "helper tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("helper tests: all passed\n");
    return 0;
}
