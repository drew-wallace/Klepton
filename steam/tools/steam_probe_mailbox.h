// Bounded credential mailbox shared with the probe UI; no Valve calls here.
#ifndef KL_STEAM_PROBE_MAILBOX_H
#define KL_STEAM_PROBE_MAILBOX_H
#include <pthread.h>
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static int session_state, session_result, session_stop, session_pending;
static char session_token[16385], session_account[65];
static void session_clear(void *p, size_t n) {
    volatile unsigned char *bytes = p;
    while (n--) *bytes++ = 0;
}
static void session_publish(int state, int result) {
    pthread_mutex_lock(&session_lock);
    session_state = state; session_result = result;
    pthread_mutex_unlock(&session_lock);
}
// States: 0 starting, 1 login required, 2 connecting, 3 logged on,
// 4 failure, 5 stopped. Result is an actual Valve EResult when available.
int32_t kl_steam_session_status(int32_t *result) {
    pthread_mutex_lock(&session_lock);
    int state = session_state;
    if (result) *result = session_result;
    pthread_mutex_unlock(&session_lock);
    return state;
}
int32_t kl_steam_session_submit(const char *token, const char *account) {
#ifndef KL_STEAM_LOGIN_ABI_PINNED
    (void)token; (void)account; return 0;
#else
    if (!token || !account) return 0;
    size_t tn = strnlen(token, sizeof session_token), an = strnlen(account, sizeof session_account);
    if (tn < 20 || tn >= sizeof session_token || !an || an >= sizeof session_account) return 0;
    for (size_t i = 0; i < tn; i++) if ((unsigned char)token[i] < 33 || (unsigned char)token[i] > 126) return 0;
    for (size_t i = 0; i < an; i++) if ((unsigned char)account[i] < 32 || (unsigned char)account[i] > 126) return 0;
    pthread_mutex_lock(&session_lock);
    int accepted = session_state == 1 && !session_pending && !session_stop;
    if (accepted) {
        memcpy(session_token, token, tn + 1); memcpy(session_account, account, an + 1);
        session_pending = 1;
    }
    pthread_mutex_unlock(&session_lock);
    return accepted;
#endif
}
void kl_steam_session_cancel(void) {
    pthread_mutex_lock(&session_lock);
    session_stop = 1; session_pending = 0;
    session_clear(session_token, sizeof session_token);
    session_clear(session_account, sizeof session_account);
    pthread_mutex_unlock(&session_lock);
}
#endif
