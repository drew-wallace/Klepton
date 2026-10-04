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
    // Actual PhysX literal pool and following frame prologue. Its x18 write
    // belongs to the new function, although the preceding two floats decode
    // as unallocated instructions.
    uint32_t physics[] = {0x42c80000, 0x42c80000, 0, 0xd503201f,
        0xd10a03ff, 0x4f000403, 0x5280002c, 0xaa0303f2,
        0xa9b67bfd, 0x910003fd, 0xf940000e};
    assert(!kl_x18_is_data(physics, sizeof physics, 7));
    physics[9] = 0xd503201f; // No complete frame setup: keep refusing.
    assert(kl_x18_is_data(physics, sizeof physics, 7));
    assert(kl_x18_is_data(physics, 9 * 4, 7));
    // Capsule/capsule CCD inserts SIMD setup between saving FP/LR and
    // establishing FP; its first x18 load was skipped on the headset.
    uint32_t capsule[] = {0x42c80000, 0x42c80000, 0, 0xd503201f,
        0xd10883ff, 0x4f000407, 0xaa0203f1, 0xaa0303ec,
        0x52800089, 0xaa0403ea, 0xa9b67bfd, 0x2f000405,
        0x0f07f406, 0x910003fd, 0xf9400012, 0x6d0627e8,
        0x6d072fea, 0xa90153f3, 0x6d0837ec, 0xa9025bf5,
        0xbd400643};
    assert(!kl_x18_is_data(capsule, sizeof capsule, 14));
    capsule[12] = 0x01234567; // Undecodable gap must still refuse.
    assert(kl_x18_is_data(capsule, sizeof capsule, 14));
    capsule[12] = 0x0f07f406;
    capsule[13] = 0xd503201f; // Incomplete frame setup must still refuse.
    assert(kl_x18_is_data(capsule, sizeof capsule, 14));
    puts("TLS classifier: measured prologue, stale pattern, bounds and data rejection passed");
}
