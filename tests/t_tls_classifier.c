#include "kl_x18.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    // Embedded attribution string, alignment, prologue, TLS canary load.
    uint32_t words[] = {
        0x474f5450, 0x20534d41, 0x3c207962, 0x72707061,
        0x706f406f, 0x73736e65, 0x726f2e6c, 0x00003e67,
        0xd503201f, 0xd503201f,
        0xd10183ff, 0xa9027bfd, 0xa9035ff8, 0xa90457f6,
        0xa9054ff4, 0x910083fd, 0xd53bd058, 0x900028c9,
        0xaa0403f3, 0xf9401708
    };
    assert(kl_x18_is_data(words, sizeof words, 16));
    assert(!kl_x18_tls_is_data(words, sizeof words, 16));
    // The recovery image relocates the ADRP but retains the exact prologue.
    words[17] = 0xf0002989;
    assert(kl_x18_is_data(words, sizeof words, 16));
    assert(!kl_x18_tls_is_data(words, sizeof words, 16));
    words[17] = 0x900028e9; // Unmeasured relocation still refuses.
    assert(kl_x18_tls_is_data(words, sizeof words, 16));
    words[17] = 0xf0002989;
    // A stale/different instruction pattern must retain conservative refusal.
    words[10] ^= 1;
    assert(kl_x18_tls_is_data(words, sizeof words, 16));
    assert(kl_x18_tls_is_data(words, 18 * 4, 16));
    uint32_t data[] = {0x12345678, 0x23456789, 0xd53bd058, 0x01234567};
    assert(kl_x18_tls_is_data(data, sizeof data, 2));
    uint32_t code[] = {0xd53bd040, 0xf9401400, 0xd65f03c0};
    assert(!kl_x18_tls_is_data(code, sizeof code, 0));
    puts("TLS classifier: measured prologue, stale pattern, bounds and data rejection passed");
}
