// Bounded batches keep the host responsive without discarding queued callbacks.
#ifndef KL_STEAM_PROBE_CALLBACKS_H
#define KL_STEAM_PROBE_CALLBACKS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#pragma pack(push, 4)
typedef struct {
    int32_t user, id;
    unsigned char *payload;
    int32_t size;
} probe_callback;
#pragma pack(pop)
_Static_assert(sizeof(probe_callback) == 20, "ARM64 callback ABI");
_Static_assert(offsetof(probe_callback, payload) == 8, "callback pointer ABI");
_Static_assert(offsetof(probe_callback, size) == 16, "callback length ABI");

static int drain_callbacks(int32_t pipe,
                           _Bool (*next)(int32_t, probe_callback *),
                           void (*release)(int32_t), unsigned *count) {
    for (unsigned i = 0; i < 256; i++) {
        probe_callback msg = {0};
        if (!next(pipe, &msg)) return 0;
        int invalid = msg.size < 0 || (msg.size && !msg.payload);
        if (*count < 32 || msg.id == 163 || msg.id == 168)
            printf("[steam-probe] callback pipe=%d id=%d size=%d layout_valid=%d\n",
                   pipe, msg.id, msg.size, !invalid);
        (*count)++;
        // Never retain or dereference the borrowed payload after release.
        release(pipe);
        if (invalid) return 1;
    }
    // A full batch is backpressure, not evidence of a malformed callback.
    // Remaining callbacks stay owned by Steam until the next frame's batch.
    return 0;
}
#endif
