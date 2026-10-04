// Copy compressed streamed mips through the Android-to-Apple ABI seam and
// verify their sampled GPU colors. Run with a freshly patched Metal ANGLE.
#include <stdio.h>
#include <stdlib.h>
#include "kl_glfb.h"

int main(int argc, char **argv) {
    if (argc > 1) setenv("KL_ANGLE_DIR", argv[1], 1);
    setenv("KL_GLFB", "1", 1);
    setenv("KL_GLFB_SIZE", "64x64", 1);
    return kl_glfb_copy_selftest(stderr) ? 0 : 1;
}
