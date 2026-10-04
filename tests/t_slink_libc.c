// Steam Link networking and worker-thread libc boundaries, through the resolver.
// curl must see 0 for a DNS name, including its AF_INET6 probe, to send SNI.
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "../runtime/klepton.h"

static int failures;
static void ck(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
int main(void) {
    int (*pton)(int, const char *, void *) = kl_shim_lookup("inet_pton");
    const char *(*ntop)(int, const void *, char *, socklen_t) = kl_shim_lookup("inet_ntop");
    struct in6_addr addr, expected;
    char text[INET6_ADDRSTRLEN];
    if (!pton || !ntop) return 1;
    ck(pton(2, "api.steampowered.com", &addr) == 0,
       "DNS name is not an IPv4 literal");
    ck(pton(10, "api.steampowered.com", &addr) == 0,
       "DNS name is not an IPv6 literal (curl enables SNI)");
    inet_pton(AF_INET6, "2001:db8::1234", &expected);
    ck(pton(10, "2001:db8::1234", &addr) == 1 &&
       memcmp(&addr, &expected, sizeof addr) == 0, "Linux AF_INET6 parses IPv6");
    ck(ntop(10, &addr, text, sizeof text) == text &&
       strcmp(text, "2001:db8::1234") == 0, "Linux AF_INET6 formats IPv6");
    ck(pton(2, "192.0.2.10", &addr) == 1 &&
       ntop(2, &addr, text, sizeof text) == text &&
       strcmp(text, "192.0.2.10") == 0, "IPv4 round trip preserved");
    errno = 0;
    ck(pton(-1, "::1", &addr) == -1 && errno == 97,
       "unsupported family reports Linux EAFNOSUPPORT");
    errno = 0;
    ck(ntop(10, &expected, text, 1) == NULL && errno == 28,
       "small output buffer reports Linux ENOSPC");
    int (*getname)(pthread_t, char *, size_t) = kl_shim_lookup("pthread_getname_np");
    if (!getname) return 1;
    char native[64], guest[64];
    pthread_getname_np(pthread_self(), native, sizeof native);
    errno = 123;
    ck(getname(pthread_self(), guest, sizeof guest) == 0 &&
       strcmp(native, guest) == 0 && errno == 123, "guest reads native thread name and preserves errno");
    ck(getname(pthread_self(), guest, 15) == 34 && errno == 123,
       "bionic thread-name buffer minimum reports Linux ERANGE");
    int (*scheduler)(int, int, const struct sched_param *) = kl_shim_lookup("sched_setscheduler");
    if (!scheduler) return 1;
    struct sched_param priority = {.sched_priority = 1};
    ck(scheduler(0, 1, &priority) == -1 && errno == 1,
       "realtime scheduling request returns recoverable Linux EPERM");
    priority.sched_priority = 100;
    ck(scheduler(0, 1, &priority) == -1 && errno == 22,
       "invalid realtime priority returns Linux EINVAL");
    ck(scheduler(0, 1, NULL) == -1 && errno == 22,
       "null scheduling parameter returns Linux EINVAL");
    return failures ? 1 : 0;
}
