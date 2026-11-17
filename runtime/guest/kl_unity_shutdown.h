#ifndef KL_UNITY_SHUTDOWN_H
#define KL_UNITY_SHUTDOWN_H

#include <stdint.h>

// Unity registers these JNI signatures on its player class. They run on the
// frame thread, after rendering ends, so managed pause/quit handlers can save.
typedef void (*kl_unity_focus_fn)(void *, void *, uint8_t);
typedef int8_t (*kl_unity_stop_fn)(void *, void *);

static inline void kl_unity_shutdown(void *env, void *player,
                                     kl_unity_focus_fn focus,
                                     kl_unity_stop_fn pause,
                                     kl_unity_stop_fn done) {
    if (focus) focus(env, player, 0);
    // nativePause's return asks Java whether to quit. The host is already
    // explicitly quitting, so nativeDone follows either return value.
    if (pause) (void)pause(env, player);
    if (done) (void)done(env, player);
}

#endif
