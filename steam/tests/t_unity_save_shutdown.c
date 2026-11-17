#include <assert.h>
#include <stdio.h>
#include "../../runtime/guest/kl_unity_shutdown.h"

static int sequence, pause_quits, saved, uploaded;
static void *expected_env = &sequence, *expected_player = &saved;
static void context(void *env, void *player) {
    assert(env == expected_env && player == expected_player);
}
static void focus(void *env, void *player, uint8_t focused) {
    context(env, player); assert(!focused && sequence++ == 0);
}
static int8_t pause_game(void *env, void *player) {
    context(env, player); assert(sequence++ == 1); saved = 1;
    return pause_quits;
}
static int8_t quit_game(void *env, void *player) {
    context(env, player); assert(sequence++ == 2 && saved); saved = 2;
    return 1;
}
int main(void) {
    for (pause_quits = 0; pause_quits <= 1; pause_quits++) {
        sequence = saved = uploaded = 0;
        kl_unity_shutdown(expected_env, expected_player, focus, pause_game, quit_game);
        // A reader after shutdown observes the quit handler's final save,
        // regardless of whether nativePause requested termination itself.
        uploaded = saved;
        assert(sequence == 3 && uploaded == 2);
    }
    // A guest missing these optional registered natives can still stop.
    sequence = saved = 0;
    kl_unity_shutdown(expected_env, expected_player, focus, pause_game, NULL);
    assert(sequence == 2 && saved == 1);
    kl_unity_shutdown(expected_env, expected_player, NULL, NULL, NULL);
    puts("Unity save lifecycle ordering checks passed");
    return 0;
}
