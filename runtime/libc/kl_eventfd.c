// Linux eventfd counting semantics on Darwin. Counter state is separate from
// the socketpair: one byte represents readable level, not one queued event.
// Filling the opposite direction suppresses native writable readiness at the
// counter limit. Thus poll/select/kqueue observe the same level as guest I/O.
#include "kl_eventfd.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LX_EFD_SEMAPHORE 1
#define LX_EFD_CLOEXEC 0x80000
#define LX_EFD_NONBLOCK 0x800
#define COUNTER_MAX (UINT64_MAX - UINT64_C(1))

typedef struct event_waiter event_waiter;
typedef struct event_counter {
    pthread_mutex_t mutex;
    event_waiter *waiters;
    uint64_t value;
    unsigned references;
    int semaphore, nonblock, monitor, signal;
} event_counter;
struct event_waiter {
    event_counter *counter;
    int reader, writer;
    event_waiter *next;
};
typedef struct event_alias {
    int fd;
    event_counter *counter;
    struct event_alias *next;
} event_alias;
static pthread_mutex_t aliases_mutex = PTHREAD_MUTEX_INITIALIZER;
static event_alias *aliases;

static event_alias **alias_slot(int fd) {
    event_alias **p = &aliases;
    while (*p && (*p)->fd != fd) p = &(*p)->next;
    return p;
}
// All reference changes hold aliases_mutex, including active I/O operations.
static void drop(event_counter *counter) {
    if (--counter->references) return;
    close(counter->monitor);
    close(counter->signal);
    pthread_mutex_destroy(&counter->mutex);
    free(counter);
}
static event_counter *retain(int fd) {
    pthread_mutex_lock(&aliases_mutex);
    event_alias *alias = *alias_slot(fd);
    event_counter *counter = alias ? alias->counter : NULL;
    if (counter) counter->references++;
    pthread_mutex_unlock(&aliases_mutex);
    return counter;
}
static void release(event_counter *counter) {
    int saved = errno;
    pthread_mutex_lock(&aliases_mutex);
    drop(counter);
    pthread_mutex_unlock(&aliases_mutex);
    errno = saved;
}

// Called with the counter mutex. Hidden descriptors remain alive for active
// I/O even if another thread closes the last guest alias, as on Linux.
static int update_readiness(event_counter *c, uint64_t before) {
    char byte = 1;
    ssize_t n;
    if (!before && c->value) {
        do { n = send(c->signal, &byte, 1, MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
        if (n != 1) return -1;
    } else if (before && !c->value) {
        do { n = recv(c->monitor, &byte, 1, MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
        if (n != 1) return -1;
    }
    char padding[4096] = {0};
    if (before != COUNTER_MAX && c->value == COUNTER_MAX) {
        for (;;) {
            n = send(c->monitor, padding, sizeof padding, MSG_DONTWAIT);
            if (n >= 0 || errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
    } else if (before == COUNTER_MAX && c->value != COUNTER_MAX) {
        for (;;) {
            n = recv(c->signal, padding, sizeof padding, MSG_DONTWAIT);
            if (n > 0 || (n < 0 && errno == EINTR)) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            errno = EIO;
            return -1;
        }
    }
    for (event_waiter *w = c->waiters; w; w = w->next) {
        // A full wakeup socket already promises a wake; it is not an error.
        do { n = send(w->writer, &byte, 1, MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
    }
    return 0;
}

int klb_eventfd(unsigned initial, int flags) {
    if (flags & ~(LX_EFD_SEMAPHORE | LX_EFD_NONBLOCK | LX_EFD_CLOEXEC)) {
        errno = EINVAL; return -1;
    }
    event_counter *c = calloc(1, sizeof *c);
    event_alias *alias = malloc(sizeof *alias);
    if (!c || !alias) { free(c); free(alias); errno = ENOMEM; return -1; }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) { free(c); free(alias); return -1; }
    c->monitor = dup(pair[0]);
    if (c->monitor < 0) {
        int saved = errno; close(pair[0]); close(pair[1]); free(c); free(alias); errno = saved; return -1;
    }
    c->signal = pair[1];
    int error = pthread_mutex_init(&c->mutex, NULL);
    if (error) {
        close(pair[0]); close(pair[1]); close(c->monitor); free(c); free(alias); errno = error; return -1;
    }
    c->references = 1;
    c->semaphore = !!(flags & LX_EFD_SEMAPHORE);
    c->nonblock = !!(flags & LX_EFD_NONBLOCK);
    c->value = initial;
    int failed = fcntl(c->signal, F_SETFD, FD_CLOEXEC) < 0
        || fcntl(c->monitor, F_SETFD, FD_CLOEXEC) < 0
        || ((flags & LX_EFD_CLOEXEC) && fcntl(pair[0], F_SETFD, FD_CLOEXEC) < 0)
        || fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0
        || fcntl(pair[1], F_SETFL, O_NONBLOCK) < 0
        || update_readiness(c, 0) < 0;
    if (failed) {
        int saved = errno; close(pair[0]); drop(c); free(alias); errno = saved; return -1;
    }
    *alias = (event_alias){pair[0], c, NULL};
    pthread_mutex_lock(&aliases_mutex);
    alias->next = aliases; aliases = alias;
    pthread_mutex_unlock(&aliases_mutex);
    return pair[0];
}

// Waiters use their own notification sockets: POLLOUT only promises room for
// one count, so waiting on the eventfd itself would spin for a larger write.
// poll preserves EINTR. Cancellation removes the waiter and releases both the
// mutex and active-I/O reference through the surrounding cleanup handlers.
static void remove_waiter(event_waiter *w) {
    event_waiter **slot = &w->counter->waiters;
    while (*slot && *slot != w) slot = &(*slot)->next;
    if (*slot) *slot = w->next;
    close(w->reader); close(w->writer);
}
static void cancel_wait(void *arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    event_waiter *w = arg;
    pthread_mutex_lock(&w->counter->mutex);
    remove_waiter(w); // leave locked for counter_read/write's cleanup
}
static void unlock_counter(void *arg) {
    pthread_mutex_unlock(&((event_counter *)arg)->mutex);
}
static void release_counter(void *arg) { release(arg); }
static int wait_change(event_counter *c) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) return -1;
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pair[1], F_SETFL, O_NONBLOCK) < 0) {
        int saved = errno; close(pair[0]); close(pair[1]); errno = saved; return -1;
    }
    event_waiter w = {c, pair[0], pair[1], c->waiters};
    c->waiters = &w;
    int ready, saved;
    pthread_cleanup_push(cancel_wait, &w);
    pthread_mutex_unlock(&c->mutex);
    struct pollfd p = {w.reader, POLLIN, 0};
    do {
        // Darwin poll does not wake for deferred pthread cancellation. Bound
        // the wait so guest read/write remain cancellation points as on Linux.
        pthread_testcancel();
        ready = poll(&p, 1, 50);
    } while (ready == 0);
    saved = errno;
    pthread_mutex_lock(&c->mutex);
    pthread_cleanup_pop(0);
    remove_waiter(&w);
    errno = saved;
    return ready;
}

static ssize_t counter_read(event_counter *c, void *buffer, size_t size) {
    ssize_t result = -1;
    if (size < 8) { errno = EINVAL; goto done; }
    if (!buffer) { errno = EFAULT; goto done; }
    pthread_mutex_lock(&c->mutex);
    pthread_cleanup_push(unlock_counter, c);
    while (!c->value) {
        if (c->nonblock) { errno = EAGAIN; goto unlock; }
        if (wait_change(c) < 0) goto unlock;
    }
    uint64_t before = c->value;
    uint64_t value = c->semaphore ? 1 : before;
    c->value -= value;
    if (update_readiness(c, before) < 0) goto unlock;
    memcpy(buffer, &value, 8);
    result = 8;
unlock:
    pthread_cleanup_pop(1);
done:
    return result;
}

static ssize_t counter_write(event_counter *c, const void *buffer, size_t size) {
    ssize_t result = -1;
    if (size < 8) { errno = EINVAL; goto done; }
    if (!buffer) { errno = EFAULT; goto done; }
    uint64_t value;
    memcpy(&value, buffer, 8);
    if (value == UINT64_MAX) { errno = EINVAL; goto done; }
    pthread_mutex_lock(&c->mutex);
    pthread_cleanup_push(unlock_counter, c);
    while (value > COUNTER_MAX - c->value) {
        if (c->nonblock) { errno = EAGAIN; goto unlock; }
        if (wait_change(c) < 0) goto unlock;
    }
    uint64_t before = c->value;
    c->value += value;
    if (update_readiness(c, before) < 0) goto unlock;
    result = 8;
unlock:
    pthread_cleanup_pop(1);
done:
    return result;
}

ssize_t kl_eventfd_read(int fd, void *buffer, size_t size, int *handled) {
    event_counter *c = retain(fd);
    *handled = c != NULL;
    if (!c) return -1;
    ssize_t result;
    pthread_cleanup_push(release_counter, c);
    result = counter_read(c, buffer, size);
    pthread_cleanup_pop(1);
    return result;
}
ssize_t kl_eventfd_write(int fd, const void *buffer, size_t size, int *handled) {
    event_counter *c = retain(fd);
    *handled = c != NULL;
    if (!c) return -1;
    ssize_t result;
    pthread_cleanup_push(release_counter, c);
    result = counter_write(c, buffer, size);
    pthread_cleanup_pop(1);
    return result;
}

// Backing sockets stay nonblocking; Darwin's AF_UNIX send can block despite
// MSG_DONTWAIT. Expose the guest's shared open-file status through fcntl hooks.
int kl_eventfd_getfl(int fd, int *handled) {
    event_counter *c = retain(fd);
    *handled = c != NULL;
    if (!c) return -1;
    pthread_mutex_lock(&c->mutex);
    int flags = fcntl(c->monitor, F_GETFL);
    if (flags >= 0 && !c->nonblock) flags &= ~O_NONBLOCK;
    pthread_mutex_unlock(&c->mutex);
    release(c);
    return flags;
}
int kl_eventfd_setfl(int fd, int flags, int *handled) {
    event_counter *c = retain(fd);
    *handled = c != NULL;
    if (!c) return -1;
    pthread_mutex_lock(&c->mutex);
    int result = fcntl(c->monitor, F_SETFL, flags | O_NONBLOCK);
    if (!result) c->nonblock = !!(flags & O_NONBLOCK);
    pthread_mutex_unlock(&c->mutex);
    release(c);
    return result;
}

int kl_eventfd_close(int fd, int *handled) {
    pthread_mutex_lock(&aliases_mutex);
    event_alias **slot = alias_slot(fd);
    *handled = *slot != NULL;
    int result = -1;
    if (*slot) {
        result = close(fd);
        if (!result) {
            event_alias *old = *slot;
            *slot = old->next;
            drop(old->counter);
            free(old);
        }
    }
    pthread_mutex_unlock(&aliases_mutex);
    return result;
}

int kl_eventfd_dup(int from, int to, int minimum, int cloexec) {
    pthread_mutex_lock(&aliases_mutex);
    event_alias *source = *alias_slot(from);
    event_alias *copy = source ? malloc(sizeof *copy) : NULL;
    if (source && !copy) { pthread_mutex_unlock(&aliases_mutex); errno = ENOMEM; return -1; }
    int fd;
    if (to == -1) fd = dup(from);
    else if (to == -2) fd = fcntl(from, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, minimum);
    else fd = dup2(from, to);
    if (fd >= 0 && from != to) {
        if (cloexec && to != -2 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            int saved = errno; close(fd); fd = -1; errno = saved;
        }
        // dup2 may also replace a managed descriptor with a regular one.
        if (to >= 0) {
            event_alias **slot = alias_slot(to);
            if (*slot) {
                event_alias *old = *slot; *slot = old->next;
                drop(old->counter); free(old);
            }
        }
        if (fd >= 0 && source) {
            source->counter->references++;
            *copy = (event_alias){fd, source->counter, aliases}; aliases = copy; copy = NULL;
        }
    }
    free(copy);
    pthread_mutex_unlock(&aliases_mutex);
    return fd;
}

int klb_dup(int fd) { return kl_eventfd_dup(fd, -1, 0, 0); }
ssize_t klb_write(int fd, const void *buffer, size_t size) {
    int handled;
    ssize_t result = kl_eventfd_write(fd, buffer, size, &handled);
    return handled ? result : write(fd, buffer, size);
}
int klb_eventfd_read(int fd, uint64_t *value) {
    int handled;
    ssize_t result = kl_eventfd_read(fd, value, 8, &handled);
    if (!handled) result = read(fd, value, 8);
    return result == 8 ? 0 : -1;
}
int klb_eventfd_write(int fd, uint64_t value) {
    return klb_write(fd, &value, 8) == 8 ? 0 : -1;
}
static ssize_t vector_io(int fd, const struct iovec *iov, int count, int writing) {
    event_counter *c = retain(fd);
    if (!c) return writing ? writev(fd, iov, count) : readv(fd, iov, count);
    // Retain throughout so descriptor closure cannot destroy the counter.
    if (count < 0 || count > IOV_MAX || (!iov && count)) { release(c); errno = EINVAL; return -1; }
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len > SIZE_MAX - total) { release(c); errno = EINVAL; return -1; }
        total += iov[i].iov_len;
        if (total - iov[i].iov_len < 8 && iov[i].iov_len && !iov[i].iov_base) {
            release(c); errno = EFAULT; return -1;
        }
    }
    if (!count) { release(c); return 0; }
    if (total < 8 || total > SSIZE_MAX) { release(c); errno = EINVAL; return -1; }
    uint8_t bytes[8];
    if (writing) {
        size_t done = 0;
        for (int i = 0; i < count && done < 8; i++) {
            size_t n = iov[i].iov_len < 8 - done ? iov[i].iov_len : 8 - done;
            if (n && !iov[i].iov_base) { release(c); errno = EFAULT; return -1; }
            if (n) memcpy(bytes + done, iov[i].iov_base, n);
            done += n;
        }
    }
    ssize_t result;
    pthread_cleanup_push(release_counter, c);
    result = writing ? counter_write(c, bytes, 8) : counter_read(c, bytes, 8);
    pthread_cleanup_pop(0);
    if (!writing && result == 8) {
        size_t done = 0;
        for (int i = 0; i < count && done < 8; i++) {
            size_t n = iov[i].iov_len < 8 - done ? iov[i].iov_len : 8 - done;
            if (n && !iov[i].iov_base) { result = -1; errno = EFAULT; break; }
            if (n) memcpy(iov[i].iov_base, bytes + done, n);
            done += n;
        }
    }
    release(c);
    return result;
}
ssize_t klb_readv(int fd, const struct iovec *iov, int count) { return vector_io(fd, iov, count, 0); }
ssize_t klb_writev(int fd, const struct iovec *iov, int count) { return vector_io(fd, iov, count, 1); }
