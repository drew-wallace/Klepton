// Tests counter behavior through the same I/O hooks used by guest imports.
#include "libc/kl_eventfd.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#define SEM 1
#define NONBLOCK 0x800
#define CLOEXEC 0x80000
static uint64_t take(int fd) {
    uint64_t value = 0; int handled = 0;
    assert(kl_eventfd_read(fd, &value, 8, &handled) == 8 && handled);
    return value;
}
static void shut(int fd) {
    int handled = 0;
    assert(kl_eventfd_close(fd, &handled) == 0 && handled);
}
static short readiness(int fd) {
    struct pollfd p = {fd, POLLIN | POLLOUT, 0};
    assert(poll(&p, 1, 0) >= 0);
    return p.revents;
}
static void *reader(void *arg) {
    int fd = *(int *)arg;
    assert(take(fd) == 7);
    return NULL;
}
static void *writer(void *arg) {
    int fd = *(int *)arg;
    assert(klb_eventfd_write(fd, 4) == 0);
    return NULL;
}
static void *semaphore_reader(void *arg) {
    int fd = *(int *)arg;
    for (int i = 0; i < 250; i++) assert(take(fd) == 1);
    return NULL;
}
static void *cancelled_reader(void *arg) {
    uint64_t value; int handled;
    kl_eventfd_read(*(int *)arg, &value, 8, &handled);
    return NULL;
}
static void *cancelled_writer(void *arg) {
    klb_eventfd_write(*(int *)arg, 2);
    return NULL;
}
static _Atomic int interrupted;
static void on_signal(int sig) { (void)sig; }
static void *interrupted_writer(void *arg) {
    assert(klb_eventfd_write(*(int *)arg, 2) == -1 && errno == EINTR);
    atomic_store(&interrupted, 1);
    return NULL;
}
static int fd_count(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}
int main(void) {
    alarm(15);
    int baseline = fd_count();
    assert(klb_eventfd(0, 2) == -1 && errno == EINVAL);
    int fd = klb_eventfd(0, NONBLOCK | CLOEXEC);
    assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    assert(readiness(fd) == POLLOUT);
    uint64_t value = 0; int handled;
    assert(kl_eventfd_read(fd, &value, 8, &handled) == -1 && handled && errno == EAGAIN);
    assert(kl_eventfd_write(fd, &value, 7, &handled) == -1 && errno == EINVAL);
    assert(kl_eventfd_read(fd, NULL, 8, &handled) == -1 && errno == EFAULT);
    assert(klb_eventfd_write(fd, UINT64_MAX) == -1 && errno == EINVAL);
    assert(klb_eventfd_write(fd, 0) == 0 && readiness(fd) == POLLOUT);
    assert(klb_eventfd_write(fd, 3) == 0 && klb_eventfd_write(fd, 5) == 0);
    assert(readiness(fd) == (POLLIN | POLLOUT));
    assert(take(fd) == 8 && readiness(fd) == POLLOUT);
    assert(klb_eventfd_write(fd, UINT64_MAX - 1) == 0);
    assert((readiness(fd) & (POLLIN | POLLOUT)) == POLLIN);
    assert(klb_eventfd_write(fd, 1) == -1 && errno == EAGAIN);
    assert(take(fd) == UINT64_MAX - 1 && readiness(fd) == POLLOUT);
    assert(kl_eventfd_getfl(fd, &handled) & O_NONBLOCK);
    int copy = klb_dup(fd);
    assert(copy >= 0 && !(fcntl(copy, F_GETFD) & FD_CLOEXEC));
    assert(klb_eventfd_write(copy, 9) == 0 && take(fd) == 9);
    int copy2 = kl_eventfd_dup(fd, -2, 100, 1);
    assert(copy2 >= 100 && (fcntl(copy2, F_GETFD) & FD_CLOEXEC));
    assert(kl_eventfd_setfl(copy, 0, &handled) == 0 && handled);
    assert(!(kl_eventfd_getfl(fd, &handled) & O_NONBLOCK));
    assert(kl_eventfd_setfl(fd, O_NONBLOCK, &handled) == 0 && handled);
    assert(kl_eventfd_getfl(copy, &handled) & O_NONBLOCK);
    shut(fd);
    assert(klb_eventfd_write(copy, 6) == 0 && take(copy2) == 6);
    shut(copy); shut(copy2);

    fd = klb_eventfd(3, SEM | NONBLOCK);
    assert(take(fd) == 1 && take(fd) == 1 && (readiness(fd) & POLLIN));
    assert(take(fd) == 1 && !(readiness(fd) & POLLIN));
    uint8_t bytes[16] = {0}; value = 12;
    struct iovec out[] = {{&value, 3}, {(char *)&value + 3, 5}};
    struct iovec in[] = {{bytes, 2}, {bytes + 2, 14}};
    assert(klb_writev(fd, out, 2) == 8);
    struct iovec invalid[] = {{NULL, 8}};
    assert(klb_readv(fd, invalid, 1) == -1 && errno == EFAULT);
    assert(klb_readv(fd, in, 2) == 8 && bytes[0] == 1 && bytes[8] == 0);
    assert(take(fd) == 1); // The invalid read did not consume a count.
    int regular = open("/dev/null", O_RDWR);
    assert(regular >= 0 && kl_eventfd_dup(regular, fd, 0, 0) == fd);
    assert(kl_eventfd_read(fd, bytes, 8, &handled) == -1 && !handled);
    close(fd); close(regular);

    fd = klb_eventfd(0, 0);
    pthread_t t;
    assert(pthread_create(&t, NULL, reader, &fd) == 0);
    assert(klb_eventfd_write(fd, 7) == 0);
    assert(pthread_join(t, NULL) == 0);
    assert(klb_eventfd_write(fd, UINT64_MAX - 2) == 0);
    assert(pthread_create(&t, NULL, writer, &fd) == 0);
    usleep(10000);
    assert(take(fd) == UINT64_MAX - 2);
    assert(pthread_join(t, NULL) == 0 && take(fd) == 4);
    shut(fd);
    fd = klb_eventfd(0, SEM);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, semaphore_reader, &fd) == 0);
    for (int i = 0; i < 1000; i++) assert(klb_eventfd_write(fd, 1) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    shut(fd);
    fd = klb_eventfd(0, 0);
    assert(pthread_create(&t, NULL, cancelled_reader, &fd) == 0);
    usleep(10000);
    assert(pthread_cancel(t) == 0);
    void *cancel_result;
    assert(pthread_join(t, &cancel_result) == 0 && cancel_result == PTHREAD_CANCELED);
    // Cancellation must leave the counter and lock usable.
    assert(klb_eventfd_write(fd, UINT64_MAX - 1) == 0);
    assert(pthread_create(&t, NULL, cancelled_writer, &fd) == 0);
    usleep(10000);
    assert(pthread_cancel(t) == 0);
    assert(pthread_join(t, &cancel_result) == 0 && cancel_result == PTHREAD_CANCELED);
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(pthread_create(&t, NULL, interrupted_writer, &fd) == 0);
    for (int i = 0; i < 100 && !atomic_load(&interrupted); i++) {
        usleep(1000);
        pthread_kill(t, SIGUSR1);
    }
    assert(atomic_load(&interrupted) && pthread_join(t, NULL) == 0);
    assert(take(fd) == UINT64_MAX - 1);
    shut(fd);
    for (int i = 0; i < 200; i++) {
        fd = klb_eventfd(1, SEM | NONBLOCK);
        assert(take(fd) == 1);
        shut(fd);
    }
    assert(fd_count() == baseline);
    puts("eventfd: counter, semaphore, readiness, bounds, aliases, vector I/O, blocking, interruption, cancellation and concurrent I/O passed");
    return 0;
}
