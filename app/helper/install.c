/* Wings update helper - download, check and place a Wings release.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from Orbit Store 1.3.5 backend/tvapp.c (GPL-3.0-or-later). The app asks for one
 * release (version, URL, SHA-256, size). The download route fetches PPSA99178.ffpkg only from
 * the Wings release on GitHub and its asset hosts, to a .part file under /data/wings-helper,
 * checks its size, SHA-256 and UFS2 image and records it. The install route hashes that .part
 * again and moves it into /data/homebrew, where
 * ShadowMountPlus registers it. The file it replaces stays as PPSA99178.ffpkg.old until the new
 * version has run (tidy). A copy the player placed is replaced only when they confirm, Wings is
 * never installed beside a folder copy, and an older version never replaces a newer one. Paths
 * are relative to the root given to helper_jobs_init. */
#include "install.h"
#include "ca.h"
#include "version.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define RELEASE_BASE "https://github.com/LaRazaDelSol/wings-app/releases/download/"
#define ASSET_HOST_1 "https://release-assets.githubusercontent.com/"
#define ASSET_HOST_2 "https://objects.githubusercontent.com/"
#define NAME "PPSA99178.ffpkg"
#define DATA "/data"
#define HOMEBREW "/data/homebrew"
#define INSTALLED HOMEBREW "/" NAME
#define OLD INSTALLED ".old"
#define FOLDER_APP HOMEBREW "/PPSA99178/eboot.bin"
/* The helper's own folder. /data/wings is created by the app (settings, diagnostics) and the
 * helper cannot write in it, so the helper keeps its work in a folder it creates itself, like
 * Orbit's /data/orbit-store. */
#define UPDATE_DIR "/data/wings-helper"
#define PART UPDATE_DIR "/" NAME ".part"
#define RECORD UPDATE_DIR "/helper-placed.json"
/* Written once a download is complete and checked: what the .part is (version, SHA-256, size,
 * time, inode). install places only a .part this describes, after hashing it again. */
#define DOWNLOADED UPDATE_DIR "/downloaded.json"
#define MAX_IMAGE (1024LL * 1024 * 1024)
#define SPARE (64LL * 1024 * 1024) /* Left free on /data after the image. */
#define UFS2_MAGIC_OFFSET (65536 + 1372)
typedef char Path[1100];

typedef enum { NOT_INSTALLED, WINGS_COPY, OWN_COPY, FOLDER_COPY, BLOCKED } Installed;
static const char *const installed_names[] = {"none", "wings", "manual", "folder", "blocked"};

static struct
{
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    bool started, stop, cancel, busy, renaming, pending, replace;
    bool placing; /* The queued job: false downloads, true places the downloaded file. */
    char root[512];
    char phase[24], error[256];
    char version[40], url[512], checksum[65];
    int64_t size, received;
    double speed;
} tv = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

typedef struct
{
    int fd;
    int64_t size, limit;
    EVP_MD_CTX *digest;
    const char *failure;
    char failure_text[256];
    double mark_time;
    int64_t mark_bytes;
} Sink;

static void copy_text(char *out, size_t cap, const char *text)
{
    if (!cap)
        return;
    snprintf(out, cap, "%s", text ? text : "");
}
static const char *json_text(const cJSON *o, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void at(char *out, const char *path)
{
    snprintf(out, sizeof(Path), "%s%s", tv.root, path);
}
static bool interrupted(void)
{
    pthread_mutex_lock(&tv.mutex);
    bool stop = tv.stop || tv.cancel;
    pthread_mutex_unlock(&tv.mutex);
    return stop;
}
static const char *test_base(void)
{
#ifdef WINGS_HELPER_TEST
    const char *base = getenv("WINGS_HELPER_TEST_BASE");
    if (base && !strncmp(base, "http://127.0.0.1:", 17))
        return base;
#endif
    return NULL;
}
/* Tests only: the final rename fails (after .old was made), as a full disk or I/O error would. */
static bool test_fail_place(void)
{
#ifdef WINGS_HELPER_TEST
    if (getenv("WINGS_HELPER_TEST_FAIL_PLACE"))
    {
        errno = EIO;
        return true;
    }
#endif
    return false;
}
static bool test_url(const char *url)
{
    const char *base = test_base();
    return base && !strncmp(url, base, strlen(base)) && url[strlen(base)] == '/';
}
/* Where a download (or one of its redirects) may go. */
static bool permitted(const char *url)
{
    return test_url(url) || !strncmp(url, RELEASE_BASE, strlen(RELEASE_BASE)) ||
           !strncmp(url, ASSET_HOST_1, strlen(ASSET_HOST_1)) ||
           !strncmp(url, ASSET_HOST_2, strlen(ASSET_HOST_2));
}
/* The release asset itself: RELEASE_BASE<tag>/PPSA99178.ffpkg and nothing else. The tag, without
 * a leading "v", goes to tag_out (at least 65 bytes). */
static bool release_url(const char *url, char *tag_out)
{
    tag_out[0] = 0;
    const char *base = test_base();
    const char *rest = NULL;
    if (strlen(url) >= sizeof tv.url || strpbrk(url, "?#\\ %") || strstr(url, ".."))
        return false;
    if (test_url(url))
        rest = url + strlen(base) + 1;
    else if (!strncmp(url, RELEASE_BASE, strlen(RELEASE_BASE)))
        rest = url + strlen(RELEASE_BASE);
    if (!rest)
        return false;
    if (test_url(url))
    {
        /* Fixture URLs: <base>/download/<tag>/PPSA99178.ffpkg */
        if (strncmp(rest, "download/", 9))
            return false;
        rest += 9;
    }
    size_t tag = strspn(rest, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-");
    if (!tag || tag > 64 || strcmp(rest + tag, "/" NAME))
        return false;
    if (*rest == 'v')
    {
        rest++;
        tag--;
    }
    memcpy(tag_out, rest, tag);
    tag_out[tag] = 0;
    return true;
}
static bool plain_version(const char *v)
{
    size_t n = strlen(v), digits = 0;
    if (!n || n >= 32)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        if (v[i] == '.')
        {
            if (!digits)
                return false;
            digits = 0;
        }
        else if (v[i] >= '0' && v[i] <= '9')
        {
            if (++digits > 9)
                return false;
        }
        else
            return false;
    }
    return digits > 0;
}
int helper_version_compare(const char *a, const char *b)
{
    while (*a || *b)
    {
        unsigned long x = 0, y = 0;
        while (*a >= '0' && *a <= '9')
            x = x * 10 + (unsigned long)(*a++ - '0');
        while (*b >= '0' && *b <= '9')
            y = y * 10 + (unsigned long)(*b++ - '0');
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9')))
            return strcmp(a, b) < 0 ? -1 : strcmp(a, b) > 0;
    }
    return 0;
}
static int ensure_dir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0755) && errno != EEXIST)
        return -1;
    return !lstat(path, &st) && S_ISDIR(st.st_mode) ? 0 : -1;
}
static int read_small(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct stat st;
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size < (off_t)cap)
        n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    return 0;
}

/* What is in /data/homebrew now. The record ties an image to the helper's install by size,
 * time and inode, so a copy the player swapped in later reads as their own. */
static Installed installed(char *version, size_t cap)
{
    Path p;
    struct stat st;
    if (version)
        *version = 0;
    /* Check the folder independently: an image may also exist. Never install a duplicate. */
    at(p, FOLDER_APP);
    if (!lstat(p, &st))
        return FOLDER_COPY;
    if (errno != ENOENT && errno != ENOTDIR)
        return BLOCKED;
    at(p, INSTALLED);
    if (lstat(p, &st))
        return errno == ENOENT ? NOT_INSTALLED : BLOCKED;
    if (!S_ISREG(st.st_mode))
        return BLOCKED;
    Installed state = OWN_COPY;
    char text[1024];
    at(p, RECORD);
    if (!read_small(p, text, sizeof text))
    {
        cJSON *record = cJSON_Parse(text);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(record, "size");
        const cJSON *modified = cJSON_GetObjectItemCaseSensitive(record, "modified");
        const cJSON *inode = cJSON_GetObjectItemCaseSensitive(record, "inode");
        if (cJSON_IsNumber(size) && size->valuedouble == (double)st.st_size &&
            cJSON_IsNumber(modified) && modified->valuedouble == (double)st.st_mtime &&
            cJSON_IsNumber(inode) && inode->valuedouble == (double)st.st_ino)
        {
            state = WINGS_COPY;
            if (version)
                copy_text(version, cap, json_text(record, "version"));
        }
        cJSON_Delete(record);
    }
    return state;
}
static bool old_kept(void)
{
    Path p;
    struct stat st;
    at(p, OLD);
    return !lstat(p, &st);
}
/* Makes renames in a directory durable. */
static int sync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    if (close(fd))
        rc = -1;
    return rc;
}
/* Never leave the player without a package: an earlier helper (1.0.1 and before renamed the
 * package away first) may have stopped with only PPSA99178.ffpkg.old there. Put it back. */
static void heal(void)
{
    Path target, old, dir;
    struct stat st;
    at(target, INSTALLED);
    at(old, OLD);
    if (!lstat(target, &st) || errno != ENOENT)
        return;
    if (lstat(old, &st) || !S_ISREG(st.st_mode))
        return;
    if (rename(old, target))
    {
        fprintf(stderr, "Wings helper: %s is missing and %s could not be put back (%s).\n",
                target, old, strerror(errno));
        return;
    }
    at(dir, HOMEBREW);
    sync_dir(dir);
    printf("Wings helper: %s was missing; restored it from %s.\n", target, old);
}
/* Copies src to a new file dst (used when a hard link is not possible). */
static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (out < 0)
    {
        close(in);
        return -1;
    }
    static char buffer[1 << 16];
    int rc = 0;
    for (;;)
    {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            rc = n < 0 ? -1 : 0;
            break;
        }
        for (ssize_t done = 0; done < n;)
        {
            ssize_t w = write(out, buffer + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
            {
                rc = -1;
                break;
            }
            done += w;
        }
        if (rc)
            break;
    }
    if (!rc && fsync(out))
        rc = -1;
    if (close(out))
        rc = -1;
    close(in);
    if (rc)
    {
        int saved = errno;
        unlink(dst);
        errno = saved;
    }
    return rc;
}

/* The player-facing storage error, with the step, the path and the system's reason so a
 * console log says what went wrong (errno is read first, before anything can change it). */
static void storage_error(char *error, size_t cap, const char *step, const char *path)
{
    int saved = errno;
    snprintf(error, cap, "%s (%s %s: %s)", "Could not save the update on the console's internal storage.", step, path, strerror(saved));
}

static size_t sink_write(char *data, size_t a, size_t b, void *context)
{
    Sink *sink = context;
    size_t n = a * b;
    if ((int64_t)n > sink->limit - sink->size)
    {
        sink->failure = "The download was larger than the release says. Nothing was installed.";
        return 0;
    }
    if (interrupted())
        return 0;
    for (size_t p = 0; p < n;)
    {
        ssize_t w = write(sink->fd, data + p, n - p);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
        {
            if (w == 0)
                errno = EIO;
            storage_error(sink->failure_text, sizeof sink->failure_text, "write", PART);
            sink->failure = sink->failure_text;
            return 0;
        }
        p += (size_t)w;
    }
    if (!EVP_DigestUpdate(sink->digest, data, n))
        return 0;
    sink->size += (int64_t)n;
    double t = now_seconds();
    pthread_mutex_lock(&tv.mutex);
    tv.received = sink->size;
    if (t - sink->mark_time >= 1.0)
    {
        tv.speed = (double)(sink->size - sink->mark_bytes) / (t - sink->mark_time);
        sink->mark_time = t;
        sink->mark_bytes = sink->size;
    }
    pthread_mutex_unlock(&tv.mutex);
    return n;
}
static int sink_reset(Sink *sink)
{
    sink->size = 0;
    sink->failure = NULL;
    sink->mark_time = now_seconds();
    sink->mark_bytes = 0;
    if (ftruncate(sink->fd, 0) || lseek(sink->fd, 0, SEEK_SET) < 0)
        return -1;
    return EVP_DigestInit_ex(sink->digest, EVP_sha256(), NULL) ? 0 : -1;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)ctx;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return interrupted();
}
/* 0 once a 200 reply is complete. Redirects are followed only within the allowed hosts. */
static int fetch(const char *url, Sink *sink, char *error, size_t cap)
{
    char current[4096];
    long status = 0;
    copy_text(current, sizeof current, url);
    for (unsigned redirects = 0; redirects <= 4; redirects++)
    {
        if (!permitted(current))
        {
            copy_text(error, cap,
                      "The release redirected outside the allowed download hosts. Nothing was "
                      "installed.");
            return -1;
        }
        CURL *curl = sink_reset(sink) ? NULL : curl_easy_init();
        if (!curl)
        {
            copy_text(error, cap, "Could not start the download.");
            return -1;
        }
        struct curl_blob ca = {(void *)wings_helper_ca, sizeof wings_helper_ca - 1,
                               CURL_BLOB_COPY};
        curl_easy_setopt(curl, CURLOPT_URL, current);
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, test_url(current) ? "http" : "https");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Wings-Helper/" WINGS_HELPER_BUILD);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        CURLcode result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        char *location = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
        bool redirect = result == CURLE_OK && status >= 300 && status < 400 && location &&
                        strlen(location) < sizeof current;
        if (redirect)
            copy_text(current, sizeof current, location);
        curl_easy_cleanup(curl);
        if (redirect)
            continue;
        if (result == CURLE_OK && status == 200)
            return 0;
        if (interrupted())
            copy_text(error, cap, "The download was stopped.");
        else if (sink->failure)
            copy_text(error, cap, sink->failure);
        else
            snprintf(error, cap, "The download failed (HTTP %ld, %s). Try again later.", status,
                     curl_easy_strerror(result));
        return -1;
    }
    copy_text(error, cap, "Too many release redirects.");
    return -1;
}

static void set_phase(const char *phase)
{
    pthread_mutex_lock(&tv.mutex);
    copy_text(tv.phase, sizeof tv.phase, phase);
    pthread_mutex_unlock(&tv.mutex);
}
static int save_record(const char *path, const char *text)
{
    Path temp;
    snprintf(temp, sizeof temp, "%s.tmp", path);
    unlink(temp);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    size_t n = strlen(text);
    int rc = write(fd, text, n) == (ssize_t)n && !fsync(fd) ? 0 : -1;
    if (close(fd))
        rc = -1;
    if (!rc)
        rc = rename(temp, path);
    if (rc)
        unlink(temp);
    return rc;
}
/* Checks the finished download: its length, SHA-256 and the UFS2 superblock magic. */
static int verify(Sink *sink, int64_t size, const char *checksum, char *error, size_t cap)
{
    unsigned char digest[EVP_MAX_MD_SIZE], magic[4];
    unsigned length = 0;
    char hex[65];
    if (sink->size != size)
    {
        copy_text(error, cap, "The download has the wrong size. Nothing was installed.");
        return -1;
    }
    if (!EVP_DigestFinal_ex(sink->digest, digest, &length) || length != 32)
        return -1;
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hex, checksum))
    {
        copy_text(error, cap, "The download failed its SHA-256 check. Nothing was installed.");
        return -1;
    }
    if (pread(sink->fd, magic, sizeof magic, UFS2_MAGIC_OFFSET) != (ssize_t)sizeof magic ||
        magic[0] != 0x19 || magic[1] != 0x01 || magic[2] != 0x54 || magic[3] != 0x19)
    {
        copy_text(error, cap, "The download isn't a Wings image. Nothing was installed.");
        return -1;
    }
    return 0;
}
/* Moves the checked .part into place, keeping what was there as .old. At no point is there no
 * package: the current one is first hard-linked (or copied) to .old, then the .part replaces it
 * in one atomic rename. The record is written before that, from the .part's size, time and
 * inode (a rename keeps all three), so tidy never mistakes the new file for one placed by hand. */
static int place(const char *part, bool replace, const char *version, const char *checksum,
                 char *error, size_t cap)
{
    Path target, old, record, dir;
    struct stat st;
    at(target, INSTALLED);
    at(old, OLD);
    at(record, RECORD);
    /* The player may have copied something in while the download ran. */
    char current[40] = {0};
    Installed now = installed(current, sizeof current);
    if (now == FOLDER_COPY || now == BLOCKED || (now == OWN_COPY && !replace))
    {
        copy_text(error, cap,
                  "Another copy of Wings appeared while downloading. Nothing was replaced.");
        return -1;
    }
    if (now == WINGS_COPY && helper_version_compare(version, current) < 0)
    {
        copy_text(error, cap, "A newer Wings is already installed. Nothing was replaced.");
        return -1;
    }
    if (!lstat(old, &st) || errno != ENOENT)
    {
        copy_text(error, cap,
                  "The previous update's PPSA99178.ffpkg.old is still there. Nothing was "
                  "replaced.");
        return -1;
    }
    char previous[1024], text[512];
    bool had_record = !read_small(record, previous, sizeof previous);
    int bad = lstat(part, &st) ? errno : (S_ISREG(st.st_mode) ? 0 : EINVAL);
    if (bad)
    {
        errno = bad;
        storage_error(error, cap, "check", part);
        return -1;
    }
    snprintf(text, sizeof text,
             "{\"version\":\"%s\",\"sha256\":\"%s\",\"size\":%lld,\"modified\":%lld,"
             "\"inode\":%lld}\n",
             version, checksum, (long long)st.st_size, (long long)st.st_mtime,
             (long long)st.st_ino);
    at(dir, UPDATE_DIR);
    if (save_record(record, text) || sync_dir(dir))
    {
        /* A half-written record must not stay: it would not match what is installed. */
        if (had_record ? save_record(record, previous) : unlink(record))
            unlink(record);
        copy_text(error, cap,
                  "Could not record the update's version on the console. Nothing was replaced.");
        return -1;
    }
    pthread_mutex_lock(&tv.mutex);
    tv.renaming = true;
    pthread_mutex_unlock(&tv.mutex);
    int rc = 0;
    bool kept = false;
    if (now != NOT_INSTALLED)
    {
        if (!link(target, old) || !copy_file(target, old))
            kept = true;
        else
        {
            rc = -1;
            snprintf(error, cap, "Could not keep the current Wings as PPSA99178.ffpkg.old (%s).",
                     strerror(errno));
        }
    }
    if (!rc && (test_fail_place() || rename(part, target)))
    {
        rc = -1;
        snprintf(error, cap, "Could not move the update into %s (%s).", HOMEBREW,
                 strerror(errno));
        /* The current package was never touched; only the extra .old goes. */
        if (kept && unlink(old))
            fprintf(stderr, "Wings helper: could not remove %s (%s).\n", old, strerror(errno));
    }
    if (rc)
    {
        /* Nothing was placed: the record goes back to what it was. */
        if (had_record ? save_record(record, previous) : (unlink(record) && errno != ENOENT))
            unlink(record);
    }
    at(dir, HOMEBREW);
    if (sync_dir(dir))
        fprintf(stderr, "Wings helper: could not sync %s (%s).\n", dir, strerror(errno));
    pthread_mutex_lock(&tv.mutex);
    tv.renaming = false;
    pthread_mutex_unlock(&tv.mutex);
    return rc;
}
/* Drops the .part and its downloaded record. */
static void drop_download(void)
{
    Path part, record;
    at(part, PART);
    at(record, DOWNLOADED);
    unlink(record);
    unlink(part);
}
/* What downloaded.json describes, if the .part is still that file (same size, time and inode).
 * A record that no longer matches is dropped with the .part. 0 when a download is ready. Only
 * called while no job runs. */
static int downloaded(char *version, size_t vcap, char *checksum, int64_t *size, double *inode)
{
    Path part, record;
    char text[1024];
    struct stat st;
    at(part, PART);
    at(record, DOWNLOADED);
    if (read_small(record, text, sizeof text))
    {
        if (errno != ENOENT)
            drop_download();
        return -1;
    }
    cJSON *saved = cJSON_Parse(text);
    const cJSON *n_size = cJSON_GetObjectItemCaseSensitive(saved, "size");
    const cJSON *n_time = cJSON_GetObjectItemCaseSensitive(saved, "modified");
    const cJSON *n_inode = cJSON_GetObjectItemCaseSensitive(saved, "inode");
    const char *v = json_text(saved, "version"), *sha = json_text(saved, "sha256");
    bool ok = plain_version(v) && strlen(sha) == 64 && cJSON_IsNumber(n_size) &&
              cJSON_IsNumber(n_time) && cJSON_IsNumber(n_inode) && !lstat(part, &st) &&
              S_ISREG(st.st_mode) && n_size->valuedouble == (double)st.st_size &&
              n_time->valuedouble == (double)st.st_mtime &&
              n_inode->valuedouble == (double)st.st_ino;
    if (ok)
    {
        copy_text(version, vcap, v);
        copy_text(checksum, 65, sha);
        *size = (int64_t)st.st_size;
        *inode = (double)st.st_ino;
    }
    cJSON_Delete(saved);
    if (!ok)
        drop_download();
    return ok ? 0 : -1;
}
/* Hashes the downloaded .part again right before it is placed, so a file swapped or changed
 * after the download is never installed. */
static int recheck(const char *part, int64_t size, double inode, const char *checksum,
                   char *error, size_t cap)
{
    static const char *changed = "The downloaded update changed on the console. Download it "
                                 "again. Nothing was installed.";
    struct stat st;
    int fd = open(part, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != size ||
        (double)st.st_ino != inode)
    {
        if (fd >= 0)
            close(fd);
        copy_text(error, cap, changed);
        return -1;
    }
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    unsigned char *buffer = malloc(1 << 16), digest[EVP_MAX_MD_SIZE], magic[4];
    unsigned length = 0;
    int64_t total = 0;
    int rc = md && buffer && EVP_DigestInit_ex(md, EVP_sha256(), NULL) ? 0 : -1;
    while (!rc)
    {
        ssize_t r = read(fd, buffer, 1 << 16);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            rc = -1;
        if (r <= 0)
            break;
        total += r;
        if (!EVP_DigestUpdate(md, buffer, (size_t)r))
            rc = -1;
    }
    if (!rc && (total != size || !EVP_DigestFinal_ex(md, digest, &length) || length != 32))
        rc = -1;
    char hex[65] = {0};
    for (unsigned i = 0; !rc && i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (!rc && strcmp(hex, checksum))
        rc = -1;
    if (!rc && (pread(fd, magic, sizeof magic, UFS2_MAGIC_OFFSET) != (ssize_t)sizeof magic ||
                magic[0] != 0x19 || magic[1] != 0x01 || magic[2] != 0x54 || magic[3] != 0x19))
        rc = -1;
    close(fd);
    free(buffer);
    EVP_MD_CTX_free(md);
    if (rc)
        copy_text(error, cap, changed);
    return rc;
}
/* The install job: re-checks the downloaded .part and places it. No network. */
static int install(char *error, size_t cap)
{
    char checksum[65], version[40], saved_version[40] = {0}, saved_checksum[65] = {0};
    pthread_mutex_lock(&tv.mutex);
    copy_text(checksum, sizeof checksum, tv.checksum);
    copy_text(version, sizeof version, tv.version);
    bool replace = tv.replace;
    pthread_mutex_unlock(&tv.mutex);
    int64_t size = 0;
    double inode = 0;
    Path part;
    at(part, PART);
    if (downloaded(saved_version, sizeof saved_version, saved_checksum, &size, &inode) ||
        strcmp(saved_version, version) || strcmp(saved_checksum, checksum))
    {
        copy_text(error, cap, "Download the update first. Nothing was installed.");
        return -1;
    }
    set_phase("verifying");
    if (recheck(part, size, inode, checksum, error, cap))
    {
        drop_download();
        return -1;
    }
    set_phase("placing");
    if (place(part, replace, version, checksum, error, cap))
        return -1; /* The checked download stays; installing can be tried again. */
    Path record;
    at(record, DOWNLOADED);
    unlink(record);
    return 0;
}
/* The download job: fetches the release to the .part, checks it and records it. */
static int download(char *error, size_t cap)
{
    char url[512], checksum[65], version[40];
    pthread_mutex_lock(&tv.mutex);
    copy_text(url, sizeof url, tv.url);
    copy_text(checksum, sizeof checksum, tv.checksum);
    copy_text(version, sizeof version, tv.version);
    int64_t size = tv.size;
    pthread_mutex_unlock(&tv.mutex);
    Path dir, part;
    at(dir, UPDATE_DIR);
    int ready = ensure_dir(dir);
    at(dir, HOMEBREW);
    if (ready || ensure_dir(dir))
    {
        snprintf(error, cap, "Could not prepare the folders on the console (%s).",
                 strerror(errno));
        return -1;
    }
    at(part, PART);
    drop_download(); /* An earlier download, of this or another version, is replaced. */
    Sink sink = {.fd = open(part, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0644),
                 .limit = size,
                 .digest = EVP_MD_CTX_new()};
    int rc = -1;
    if (sink.fd < 0)
        storage_error(error, cap, "open", part);
    else if (!sink.digest)
        copy_text(error, cap, "Could not start the checksum. Nothing was changed.");
    else
        rc = fetch(url, &sink, error, cap);
    if (!rc)
    {
        set_phase("verifying");
        rc = verify(&sink, size, checksum, error, cap);
    }
    if (!rc && fsync(sink.fd))
    {
        rc = -1;
        storage_error(error, cap, "fsync", part);
    }
    if (sink.fd >= 0 && close(sink.fd) && !rc)
    {
        rc = -1;
        storage_error(error, cap, "close", part);
    }
    EVP_MD_CTX_free(sink.digest);
    if (!rc && interrupted())
    {
        rc = -1;
        copy_text(error, cap, "The download was stopped.");
    }
    if (!rc)
    {
        struct stat st;
        char text[512];
        Path record;
        at(record, DOWNLOADED);
        at(dir, UPDATE_DIR);
        if (lstat(part, &st))
            rc = -1;
        else
        {
            snprintf(text, sizeof text,
                     "{\"version\":\"%s\",\"sha256\":\"%s\",\"size\":%lld,\"modified\":%lld,"
                     "\"inode\":%lld}\n",
                     version, checksum, (long long)st.st_size, (long long)st.st_mtime,
                     (long long)st.st_ino);
            rc = save_record(record, text) || sync_dir(dir) ? -1 : 0;
        }
        if (rc)
            copy_text(error, cap, "Could not record the download on the console. Try again.");
    }
    if (rc)
        drop_download();
    return rc;
}

static void *worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&tv.mutex);
    while (!tv.stop)
    {
        while (!tv.pending && !tv.stop)
            pthread_cond_wait(&tv.changed, &tv.mutex);
        if (tv.stop)
            break;
        tv.pending = false;
        bool placing = tv.placing;
        pthread_mutex_unlock(&tv.mutex);
        char error[256] = {0};
        int rc = placing ? install(error, sizeof error) : download(error, sizeof error);
        pthread_mutex_lock(&tv.mutex);
        if (rc && tv.cancel && !tv.stop)
        {
            /* The player cancelled: back to idle, nothing to report. */
            tv.error[0] = 0;
            copy_text(tv.phase, sizeof tv.phase, "idle");
        }
        else
        {
            copy_text(tv.error, sizeof tv.error,
                      rc ? (*error ? error : "Wings could not be installed. Try again.") : "");
            copy_text(tv.phase, sizeof tv.phase,
                      rc ? "error" : (placing ? "placed" : "downloaded"));
        }
        tv.speed = 0;
        tv.cancel = false;
        tv.busy = false;
    }
    pthread_mutex_unlock(&tv.mutex);
    return NULL;
}
int helper_jobs_init(const char *root)
{
    copy_text(tv.root, sizeof tv.root, root);
    heal();
    pthread_mutex_lock(&tv.mutex);
    tv.stop = tv.cancel = tv.busy = tv.pending = tv.renaming = false;
    copy_text(tv.phase, sizeof tv.phase, "idle");
    tv.error[0] = 0;
    pthread_mutex_unlock(&tv.mutex);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, WINGS_HELPER_STACK);
    if (!rc)
        rc = pthread_create(&tv.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&tv.mutex);
    tv.started = !rc;
    pthread_mutex_unlock(&tv.mutex);
    return rc ? -1 : 0;
}
void helper_jobs_shutdown(void)
{
    pthread_mutex_lock(&tv.mutex);
    tv.stop = true;
    pthread_cond_signal(&tv.changed);
    bool started = tv.started;
    tv.started = false;
    pthread_mutex_unlock(&tv.mutex);
    if (started)
        pthread_join(tv.thread, NULL);
}
bool helper_busy(void)
{
    pthread_mutex_lock(&tv.mutex);
    bool busy = tv.busy;
    pthread_mutex_unlock(&tv.mutex);
    return busy;
}
bool helper_renaming(void)
{
    pthread_mutex_lock(&tv.mutex);
    bool renaming = tv.renaming;
    pthread_mutex_unlock(&tv.mutex);
    return renaming;
}
cJSON *helper_status(void)
{
    char current[40] = {0};
    Installed state = installed(current, sizeof current);
    bool old = old_kept();
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&tv.mutex);
    /* A checked download waiting to be installed, also one from before the helper restarted.
     * Checked under the lock so no download starts meanwhile. */
    const char *phase = tv.phase, *version = tv.version;
    int64_t received = tv.received, size = tv.size;
    char ready_version[40] = {0}, ready_checksum[65];
    double inode = 0;
    bool settled = !strcmp(tv.phase, "idle") || !strcmp(tv.phase, "downloaded");
    if (!tv.busy && settled)
    {
        if (!downloaded(ready_version, sizeof ready_version, ready_checksum, &size, &inode))
        {
            phase = "downloaded";
            version = ready_version;
            received = size;
        }
        else
        {
            phase = "idle";
            received = size = 0;
        }
    }
    cJSON_AddStringToObject(o, "helper", WINGS_HELPER_BUILD);
    cJSON_AddNumberToObject(o, "api", WINGS_HELPER_API);
    cJSON_AddStringToObject(o, "phase", phase);
    cJSON_AddBoolToObject(o, "busy", tv.busy);
    cJSON_AddStringToObject(o, "version", version);
    cJSON_AddNumberToObject(o, "received", (double)received);
    cJSON_AddNumberToObject(o, "size", (double)size);
    cJSON_AddNumberToObject(o, "speed", tv.speed);
    cJSON_AddStringToObject(o, "error", tv.error);
    cJSON_AddStringToObject(o, "target", installed_names[state]);
    cJSON_AddStringToObject(o, "installed_version", current);
    cJSON_AddBoolToObject(o, "old_kept", old);
    pthread_mutex_unlock(&tv.mutex);
    return o;
}
static int refuse(int code, const char *message, const char *why, char *error, size_t cap,
                  const char **reason)
{
    copy_text(error, cap, message);
    *reason = why;
    return code;
}
/* Both routes: the request's checks and everything that would stop the release being placed,
 * so nothing is downloaded that could not be installed. placing: the install route (no URL or
 * size needed, no space check). */
static int start(const cJSON *input, bool placing, char *error, size_t cap, const char **reason)
{
    char version[40], hash[65], tag[65];
    const char *v = json_text(input, "version"), *url = json_text(input, "url");
    const char *sha = json_text(input, "sha256"), *running = json_text(input, "running");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(input, "size");
    const cJSON *replace = cJSON_GetObjectItemCaseSensitive(input, "replace");
    double bytes = cJSON_IsNumber(size) ? size->valuedouble : 0;
    if (*v == 'v')
        v++;
    copy_text(version, sizeof version, v);
    bool hash_ok = strlen(sha) == 64 && strspn(sha, "0123456789abcdefABCDEF") == 64;
    for (size_t i = 0; hash_ok && i < 65; i++)
        hash[i] = (char)(sha[i] >= 'A' && sha[i] <= 'F' ? sha[i] - 'A' + 'a' : sha[i]);
    /* The version asked for must be the release the URL points at (tag "v1.2" or "1.2"). */
    bool source_ok = placing || (release_url(url, tag) && !strcmp(tag, version) &&
                                 bytes >= UFS2_MAGIC_OFFSET + 4 && bytes <= (double)MAX_IMAGE &&
                                 bytes == (double)(int64_t)bytes);
    if (!cJSON_IsObject(input) || !plain_version(version) || !source_ok || !hash_ok ||
        (replace && !cJSON_IsBool(replace)) || (*running && !plain_version(running)))
        return refuse(400,
                      placing ? "The install request is invalid. Nothing was installed."
                              : "The update request is invalid. Nothing was downloaded.",
                      "invalid", error, cap, reason);
    if (helper_busy())
        return refuse(409, "An update is already being installed. Wait for it.", "busy", error,
                      cap, reason);
    heal();
    if (*running && helper_version_compare(version, running) < 0)
        return refuse(409, "This Wings is newer than that release. Nothing was replaced.",
                      "newer", error, cap, reason);
    char current[40] = {0};
    Installed state = installed(current, sizeof current);
    if (state == FOLDER_COPY)
        return refuse(409,
                      "Wings is installed as the folder /data/homebrew/PPSA99178. Remove that "
                      "folder to install the release, or keep updating it yourself.",
                      "folder", error, cap, reason);
    if (state == BLOCKED)
        return refuse(409,
                      "Something other than Wings is at /data/homebrew/" NAME ". Move it away "
                      "first.",
                      "blocked", error, cap, reason);
    if (state == WINGS_COPY && helper_version_compare(version, current) < 0)
        return refuse(409, "A newer Wings is already installed. Nothing was replaced.", "newer",
                      error, cap, reason);
    /* The player confirms replacing their own copy when they install, not before the download. */
    if (placing && state == OWN_COPY && !cJSON_IsTrue(replace))
        return refuse(409,
                      "A copy you placed yourself is already in /data/homebrew. Confirm "
                      "replacing it with this release.",
                      "manual", error, cap, reason);
    if (old_kept())
        return refuse(409,
                      "The previous update's PPSA99178.ffpkg.old is still there. Open the new "
                      "version once so it can be tidied.",
                      "old_waiting", error, cap, reason);
    struct statvfs vfs;
    Path data;
    at(data, DATA);
    if (!placing && !statvfs(data, &vfs) &&
        (double)vfs.f_bavail * (double)(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize) <
            bytes + (double)SPARE)
    {
        snprintf(error, cap,
                 "Not enough free space on the console's internal storage. The update needs "
                 "about %lld MB.",
                 (long long)(((int64_t)bytes + SPARE) / (1024 * 1024)));
        *reason = "space";
        return 507;
    }
    int code = 202;
    pthread_mutex_lock(&tv.mutex);
    if (tv.busy || tv.stop || !tv.started)
    {
        code = 409;
        copy_text(error, cap, "An update is already being installed. Wait for it.");
        *reason = "busy";
    }
    else
    {
        char ready_version[40] = {0}, ready_checksum[65] = {0};
        int64_t ready_size = 0;
        double inode = 0;
        if (placing &&
            (downloaded(ready_version, sizeof ready_version, ready_checksum, &ready_size,
                        &inode) ||
             strcmp(ready_version, version) || strcmp(ready_checksum, hash)))
        {
            pthread_mutex_unlock(&tv.mutex);
            return refuse(409, "Download the update first. Nothing was installed.",
                          "not_downloaded", error, cap, reason);
        }
        tv.busy = tv.pending = true;
        tv.placing = placing;
        tv.cancel = false;
        tv.error[0] = 0;
        tv.received = placing ? ready_size : 0;
        tv.speed = 0;
        tv.size = placing ? ready_size : (int64_t)bytes;
        tv.replace = cJSON_IsTrue(replace);
        copy_text(tv.version, sizeof tv.version, version);
        copy_text(tv.url, sizeof tv.url, placing ? "" : url);
        copy_text(tv.checksum, sizeof tv.checksum, hash);
        copy_text(tv.phase, sizeof tv.phase, placing ? "verifying" : "downloading");
        pthread_cond_signal(&tv.changed);
    }
    pthread_mutex_unlock(&tv.mutex);
    return code;
}
int helper_download(const cJSON *input, char *error, size_t cap, const char **reason)
{
    return start(input, false, error, cap, reason);
}
int helper_install(const cJSON *input, char *error, size_t cap, const char **reason)
{
    return start(input, true, error, cap, reason);
}
int helper_cancel(void)
{
    pthread_mutex_lock(&tv.mutex);
    if (tv.busy && !tv.renaming)
        tv.cancel = true;
    pthread_mutex_unlock(&tv.mutex);
    return 200;
}
int helper_tidy(const cJSON *input, char *error, size_t cap, const char **reason)
{
    const char *running = json_text(input, "running");
    if (!plain_version(running))
        return refuse(400, "The tidy request is invalid.", "invalid", error, cap, reason);
    if (helper_busy())
        return refuse(409, "An update is being installed. Wait for it.", "busy", error, cap,
                      reason);
    heal();
    char current[40] = {0};
    Installed state = installed(current, sizeof current);
    if (!old_kept())
        return 200;
    /* The .old is the way back from the helper's last install. It goes once the version that
     * install placed (or a newer one) runs, or once that placed file was replaced by hand (the
     * record no longer matches it), since that install is then undone anyway. A record that
     * describes the .old itself means the last placement never finished: not replaced by hand. */
    Path record, old;
    struct stat st;
    char text[1024];
    at(record, RECORD);
    at(old, OLD);
    bool replaced_by_hand = false;
    if (state == OWN_COPY && !read_small(record, text, sizeof text))
    {
        cJSON *saved = cJSON_Parse(text);
        const cJSON *inode = cJSON_GetObjectItemCaseSensitive(saved, "inode");
        replaced_by_hand = !(cJSON_IsNumber(inode) && !lstat(old, &st) &&
                             inode->valuedouble == (double)st.st_ino);
        cJSON_Delete(saved);
    }
    if (!replaced_by_hand && (state != WINGS_COPY || helper_version_compare(running, current) < 0))
        return refuse(409, "The new version isn't running yet. The previous file was kept.",
                      "not_running", error, cap, reason);
    if (unlink(old) && errno != ENOENT)
    {
        snprintf(error, cap, "Could not remove PPSA99178.ffpkg.old (%s).", strerror(errno));
        *reason = "blocked";
        return 500;
    }
    return 200;
}
