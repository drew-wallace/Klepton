// Disabled diagnostics must not enter the guest or trigger managed constructors.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_mprobe.h"
static int lookups;
kl_image *kl_find_image(const char *name) {
    assert(!strcmp(name, "libil2cpp.so")); lookups++; return NULL;
}
void *kl_sym(kl_image *image, const char *name) { abort(); }
void *kl_base(kl_image *image) { abort(); }
static void check(int mode) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        const char *flags[] = {"KL_PROBE_INPUT", "KL_PROBE_XR", "KL_PROBE_STACKTRACE", "KL_PROBE_RESOLVE"};
        for (unsigned i = 0; i < sizeof flags / sizeof *flags; i++) unsetenv(flags[i]);
        if (mode == 1) setenv("KL_PROBE_INPUT", "0", 1);
        if (mode == 2) setenv("KL_PROBE_INPUT", "1", 1);
        if (mode == 3) setenv("KL_PROBE_XR", "1", 1);
        for (unsigned frame = 0; frame <= 2400; frame++) kl_mprobe_tick(frame);
        assert(lookups == (mode >= 2 ? 1 : 0));
        _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(void) { for (int mode = 0; mode < 4; mode++) check(mode); return 0; }
