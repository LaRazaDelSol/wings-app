/* Wings cover-art module: libc symbols libcrypto.a (PacBrew OpenSSL 3.5.2)
 * references that the app's link (SDK import stubs + clean-room libc shim) does
 * not provide. Found by the link probe (prototypes/tls-art/link-probe.sh); all
 * six sit on code paths a TCP TLS client never executes (datagram BIOs, async
 * fibres, DSO path lookup) except gmtime_r, which X.509 validity checking DOES
 * call, so that one is a real implementation. Same approach as
 * src/runtime/runtime_shims.c.
 *
 * Add this file to the app's src/ (or compile it into the art archive) ONLY in
 * the console build. Do NOT add it to the host/preview or unit-test builds:
 * glibc already provides all of these and the duplicates would break
 * getaddrinfo for everything else.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

struct tm *gmtime_r(const time_t *timep, struct tm *out)
{
    long long t = (long long)*timep;
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0)
    {
        rem += 86400;
        --days;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0)
        out->tm_wday += 7;
    // Howard Hinnant's civil_from_days.
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        ++y;
    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)(m - 1);
    out->tm_mday = (int)d;
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    out->tm_yday = cum[m - 1] + (int)d - 1 + (leap && m > 2 ? 1 : 0);
    out->tm_isdst = 0;
    return out;
}

// dladdr: only used to find the path of a loaded OpenSSL DSO.
int dladdr(const void *addr, void *info)
{
    (void)addr;
    (void)info;
    return 0;
}

// Datagram BIOs (DTLS/QUIC): never reached by a TCP client.
long sendmmsg(int fd, void *msgs, unsigned vlen, int flags)
{
    (void)fd; (void)msgs; (void)vlen; (void)flags;
    errno = ENOSYS;
    return -1;
}
long recvmmsg(int fd, void *msgs, unsigned vlen, int flags, void *timeout)
{
    (void)fd; (void)msgs; (void)vlen; (void)flags; (void)timeout;
    errno = ENOSYS;
    return -1;
}

// OpenSSL ASYNC fibres (SSL_MODE_ASYNC / ASYNC_start_job): unused; fail loudly.
int _setjmp(void *env)
{
    (void)env;
    abort();
}
void _longjmp(void *env, int value)
{
    (void)env;
    (void)value;
    abort();
}

// ---- Name resolution -------------------------------------------------------
// In the SDK stubs getaddrinfo/freeaddrinfo/gai_strerror/gethostbyname/
// getnameinfo are exported ONLY by libScePosixForWebKit, which a native title
// does not load (runtime_shims.c documents the same trap for mkstemp/isatty:
// the call jumps to address 0). Defining them here keeps the import out of the
// eboot (the link probe verifies NEEDED has no libScePosixForWebKit) and makes
// any accidental use fail cleanly. Hostnames are resolved by DohResolver.
#include <netdb.h>

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res)
{
    (void)node; (void)service; (void)hints;
    *res = NULL;
    return EAI_FAIL;
}
void freeaddrinfo(struct addrinfo *ai)
{
    (void)ai;
}
const char *gai_strerror(int code)
{
    (void)code;
    return "name resolution via getaddrinfo is disabled in the app";
}
struct hostent *gethostbyname(const char *name)
{
    (void)name;
    return NULL;
}
int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, size_t hostlen,
                char *serv, size_t servlen, int flags)
{
    (void)sa; (void)salen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags;
    return EAI_FAIL;
}
