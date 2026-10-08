// Single-owner experimental session controller for the hash-pinned client.
// Only this probe worker calls Valve interfaces. The UI transfers credentials
// through a bounded, mutex-protected mailbox; no credential reaches a log.
#ifndef KL_STEAM_PROBE_SESSION_H
#define KL_STEAM_PROBE_SESSION_H
#include "steam_probe_mailbox.h"
static double session_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
#include "../../games/walkabout/steam/steam_probe_game_api.h"
static int session_run(kl_image *image, void *engine, void *steam_user, void *client,
                       int32_t user, int32_t backend_pipe, int32_t game_pipe,
                       _Bool (*next)(int32_t, probe_callback *), void (*release)(int32_t)) {
#ifndef KL_STEAM_LOGIN_ABI_PINNED
    (void)image; (void)engine; (void)steam_user; (void)client; (void)user;
    (void)backend_pipe; (void)game_pipe; (void)next; (void)release;
    session_publish(4, 0); return 1;
#else
    if (!engine || !steam_user || !next || !release) { session_publish(4, 0); return 1; }
    void *getter = (*(void ***)engine)[8];
    void *logon = kl_sym(image, "Steam_LogOn"), *logoff = kl_sym(image, "Steam_LogOff");
    if (!login_probe_address(image, getter, 0x1286858) ||
        !login_probe_address(image, logon, 0xe54304) ||
        !login_probe_address(image, logoff, 0xe54398)) { session_publish(4, 0); return 1; }
    void *internal = ((void *(*)(void *, int32_t, int32_t))getter)(engine, user, backend_pipe);
    if (!internal || !login_probe_address(image, (*(void ***)internal)[56], 0x10d5704)) {
        session_publish(4, 0); return 1;
    }
    void (*set_token)(void *, const char *, const char *) = (void *)(*(void ***)internal)[56];
    uint64_t (*steam_id)(void *) = (void *)(*(void ***)steam_user)[2];
    _Bool (*logged_on)(void *) = (void *)(*(void ***)steam_user)[1];
    void (*frame)(void *) = (void *)(*(void ***)client)[19];
    int32_t (*login)(int32_t, int32_t, uint64_t) = (void *)logon;
    void (*logout)(int32_t, int32_t) = (void *)logoff;
    unsigned callbacks = 0;
    int failed = 0, attempted = 0, was_logged_on = 0;
    double deadline = session_now() + 600, login_deadline = 0;
    game_prepare(image);
    session_publish(1, 0);
    printf("[steam-probe] interactive native session ready; login_required=1\n");
    while (
#ifdef KL_STEAM_GAME_HOST
           1
#else
           session_now() < deadline
#endif
           ) {
        char token[sizeof session_token] = {0}, account[sizeof session_account] = {0};
        pthread_mutex_lock(&session_lock);
        int stop = session_stop, pending = session_pending;
        if (pending && !stop) {
            memcpy(token, session_token, sizeof token); memcpy(account, session_account, sizeof account);
            session_pending = 0;
            session_clear(session_token, sizeof session_token); session_clear(session_account, sizeof session_account);
        }
        pthread_mutex_unlock(&session_lock);
        if (stop) break;
        if (pending) {
            // The backend parses its own JWT and updates its own SteamID. Query
            // that identity after setting the token; never manufacture a user.
            set_token(internal, token, account);
            session_clear(token, sizeof token); session_clear(account, sizeof account);
            int32_t result = login(user, backend_pipe, steam_id(steam_user));
            attempted = 1; login_deadline = session_now() + 120;
            session_publish(result == 1 ? 2 : 4, result);
            printf("[steam-probe] token Steam_LogOn result=%d\n", result);
        }
        frame(client);
        failed = drain_callbacks(game_pipe, next, release, &callbacks);
        if (!failed && backend_pipe != game_pipe) failed = drain_callbacks(backend_pipe, next, release, &callbacks);
        if (failed) { session_publish(4, 0); break; }
        int now_logged_on = logged_on(steam_user);
        game_advance(now_logged_on);
#ifdef KL_STEAM_GAME_HOST
        game_release_for_host();
#endif
        if (now_logged_on != was_logged_on) {
            printf("[steam-probe] interactive native BLoggedOn=%d\n", now_logged_on);
            session_publish(now_logged_on ? 3 : 4, now_logged_on ? 1 : 0);
            was_logged_on = now_logged_on;
        }
        if (attempted && !now_logged_on && session_now() > login_deadline) { session_publish(4, 0); attempted = 0; }
        const struct timespec interval = {0, 10000000}; nanosleep(&interval, NULL);
    }
    game_shutdown();
    version_finish();
    logout(user, backend_pipe);
    kl_steam_session_cancel();
    printf("[steam-probe] interactive session LogOff returned\n");
    return failed;
#endif
}
#endif
