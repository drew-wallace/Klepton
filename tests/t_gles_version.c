#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_egl.h"

int main(void) {
    const char *choices[] = {NULL, "3.0", "3.1", "invalid", NULL};
    for (int mode = 0; mode < 5; mode++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            unsetenv("KL_GLFB");
            if (choices[mode]) setenv("KL_GLES_VERSION", choices[mode], 1);
            else unsetenv("KL_GLES_VERSION");
            if (mode == 4) kl_egl_set_gles_version(2, 0);
            int32_t major = -1, minor = -1;
            // ANGLE asks the shared cap surface directly, before symbol lookup.
            assert(kl_gl_cap_integerv(0x821b, &major));
            assert(kl_gl_cap_integerv(0x821c, &minor));
            const char *(*get_string)(uint32_t) = kl_egl_sym("glGetString");
            assert(get_string);
            int expected_major = mode == 4 ? 2 : 3;
            int expected_minor = mode == 1 || mode == 4 ? 0 : mode==2 ? 1 : 2;
            assert(major == expected_major && minor == expected_minor);
            char expected[32];
            snprintf(expected, sizeof expected, "OpenGL ES %d.%d Klepton", expected_major, expected_minor);
            assert(!strcmp(get_string(0x1f02), expected));
            snprintf(expected, sizeof expected, "OpenGL ES GLSL ES %d.%d0", expected_major, expected_minor);
            assert(!strcmp(get_string(0x8b8c), expected));
            _exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("GLES description: default, 3.0/3.1 overrides, invalid input and explicit GLES2 remain consistent");
}
