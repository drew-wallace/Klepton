#include <stdint.h>
#include <stdio.h>

extern int tlsdesc_probe(const uint64_t *descriptor);
extern intptr_t kl_tlsdesc_entry(const uint64_t *descriptor);

// Deliberately clobber registers outside the TLSDESC caller-clobber set.
intptr_t kl_tlsdesc_resolve(const uint64_t *descriptor) {
    (void)descriptor;
    __asm__ volatile("mov x2, xzr\n\tmovi v4.16b, #0" ::: "x2", "v4");
    return 0x1234;
}

int main(void) {
    uint64_t descriptor[2] = {(uint64_t)(uintptr_t)kl_tlsdesc_entry, 0};
    int ok = tlsdesc_probe(descriptor);
    printf("%s: TLSDESC resolver preserves guest GPR/SIMD state\n",
           ok ? "PASS" : "FAIL");
    return !ok;
}
