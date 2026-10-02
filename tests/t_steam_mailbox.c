#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define KL_STEAM_LOGIN_ABI_PINNED 1
#include "../tools/steam_probe_mailbox.h"
static const char token[] = "test-only-opaque-token-never-used-for-authentication";
static void *submit(void *unused) {
    (void)unused;
    return (void *)(intptr_t)kl_steam_session_submit(token, "offline-fixture");
}
int main(void) {
    assert(!kl_steam_session_submit(token, "offline-fixture"));
    session_publish(1, 0);
    assert(!kl_steam_session_submit(NULL, "offline-fixture"));
    assert(!kl_steam_session_submit("short", "offline-fixture"));
    assert(!kl_steam_session_submit(token, ""));
    char too_long[16386]; memset(too_long, 'x', sizeof too_long); too_long[sizeof too_long-1] = 0;
    assert(!kl_steam_session_submit(too_long, "offline-fixture"));
    char long_account[66]; memset(long_account, 'x', sizeof long_account); long_account[65] = 0;
    assert(!kl_steam_session_submit(token, long_account));
    assert(!kl_steam_session_submit("token-with-newline-unsafe\n", "offline-fixture"));
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, submit, NULL));
    unsigned accepted = 0;
    for (unsigned i = 0; i < 8; i++) { void *result; assert(!pthread_join(threads[i], &result)); accepted += (unsigned)(intptr_t)result; }
    assert(accepted == 1);
    assert(session_pending && !strcmp(session_token, token));
    kl_steam_session_cancel();
    assert(!session_pending);
    for (size_t i = 0; i < sizeof session_token; i++) assert(!session_token[i]);
    for (size_t i = 0; i < sizeof session_account; i++) assert(!session_account[i]);
    assert(!kl_steam_session_submit(token, "offline-fixture"));
    int32_t result = -1; session_publish(4, 5);
    assert(kl_steam_session_status(&result) == 4 && result == 5);
    puts("Steam credential mailbox bounds, concurrency and cancellation passed");
}
