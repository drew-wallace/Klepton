// Bionic nonlocal jumps must restore only the calling thread's signal mask.
// Darwin longjmp calls sigprocmask, whose SETMASK changes all process threads.
// Use the native register-only pair and our own mask footer in Bionic's larger
// opaque buffer. No private Darwin jmp_buf fields are interpreted.
#include "kl_jump.h"
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct { uint32_t magic, save_mask; sigset_t mask; } jump_footer;
#define KL_JUMP_MAGIC UINT32_C(0x4b4c4a50)
_Static_assert(sizeof(jmp_buf) + sizeof(jump_footer) <= KL_BIONIC_JUMP_BYTES,
               "native register context exceeds Bionic jump buffer");

// Called by the assembly entry before tail-branching to native _setjmp.
void *kl_jump_prepare(void *buffer, int save_mask) {
    jump_footer footer = {.magic = KL_JUMP_MAGIC, .save_mask = save_mask != 0};
    if (footer.save_mask && pthread_sigmask(SIG_SETMASK, NULL, &footer.mask)) abort();
    memcpy((char *)buffer + sizeof(jmp_buf), &footer, sizeof footer);
    return buffer;
}
_Noreturn void klb_longjmp(void *buffer, int result) {
    jump_footer footer;
    memcpy(&footer, (char *)buffer + sizeof(jmp_buf), sizeof footer);
    if (footer.magic != KL_JUMP_MAGIC) {
        static const char message[] = "[klb] nonlocal jump without a guest jump context\n";
        (void)write(2, message, sizeof message - 1);
        abort();
    }
    if (footer.save_mask && pthread_sigmask(SIG_SETMASK, &footer.mask, NULL)) abort();
    _longjmp(buffer, result);
}
