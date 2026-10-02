#include "klepton.h"
#include "kl_jump.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void exercise(int mode, int requested_return, int jump_api) {
    _Alignas(16) struct {
        uint64_t before[2];
        unsigned char bytes[KL_BIONIC_JUMP_BYTES];
        uint64_t after[2];
    } buffer;
    memset(&buffer, 0xa5, sizeof buffer);
    sigset_t empty = 0, changed = 0, current = 0;
    sigaddset(&changed, SIGUSR1);
    assert(!pthread_sigmask(SIG_SETMASK, &empty, NULL));
    volatile int returned;
    switch (mode) {
    case 0: returned = klb_setjmp(buffer.bytes); break;
    case 1: returned = klb__setjmp(buffer.bytes); break;
    case 2: returned = klb_sigsetjmp(buffer.bytes, 0); break;
    default: returned = klb_sigsetjmp(buffer.bytes, -1); break;
    }
    if (!returned) {
        assert(!pthread_sigmask(SIG_SETMASK, &changed, NULL));
        const char *name = jump_api == 0 ? "longjmp" : jump_api == 1 ? "_longjmp" : "siglongjmp";
        ((void (*)(void *, int))kl_shim_lookup(name))(buffer.bytes, requested_return);
        assert(!"jump returned instead of restoring the saved context");
    }
    assert(returned == (requested_return ? requested_return : 1));
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &current));
    assert(current == (mode == 0 || mode == 3 ? empty : changed));
    for (int i = 0; i < 2; i++) {
        assert(buffer.before[i] == UINT64_C(0xa5a5a5a5a5a5a5a5));
        assert(buffer.after[i] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    }
}
int main(void) {
    assert(kl_shim_lookup("setjmp") == (void *)klb_setjmp);
    assert(kl_shim_lookup("_setjmp") == (void *)klb__setjmp);
    assert(kl_shim_lookup("sigsetjmp") == (void *)klb_sigsetjmp);
    sigset_t original = 0;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &original));
    for (int mode = 0; mode < 4; mode++)
        for (int api = 0; api < 3; api++) {
            exercise(mode, 0, api);
            exercise(mode, 7, api);
        }
    assert(!pthread_sigmask(SIG_SETMASK, &original, NULL));
    puts("Guest jump masks, return values, caller contexts and buffer guards passed");
}
