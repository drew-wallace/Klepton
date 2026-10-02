#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "klepton.h"
int main(void) {
    int (*advise)(int, int64_t, int64_t, int) = kl_shim_lookup("posix_fadvise");
    assert(advise);
    FILE *file = tmpfile(); assert(file);
    for (int hint = 0; hint <= 5; hint++) {
        errno = EDOM;
        assert(advise(fileno(file), 0, 0, hint) == 95);
        assert(errno == EDOM);
    }
    assert(advise(fileno(file), -1, 0, 0) == EINVAL);
    assert(advise(fileno(file), 0, -1, 0) == EINVAL);
    assert(advise(fileno(file), 0, 0, 6) == EINVAL);
    assert(advise(-1, 0, 0, 0) == EBADF);
    int pipes[2]; assert(pipe(pipes) == 0);
    assert(advise(pipes[0], 0, 0, 0) == ESPIPE);
    close(pipes[0]); close(pipes[1]); fclose(file);
    puts("Steam reached file-advice errors and errno checks passed");
}
