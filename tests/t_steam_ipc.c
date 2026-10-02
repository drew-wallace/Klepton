#include "klepton.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "kl_file.h"
// AArch64 Linux epoll_event is naturally aligned, unlike x86's packed layout.
typedef struct { uint32_t events, pad; uint64_t data; } lx_event;
#define FN(name, type) ((type)kl_shim_lookup(name))
static void *(*get_specific)(int);
static int (*set_specific)(int, const void *);
static _Atomic int destructed;
static void tls_destructor(void *value) { assert(value == (void *)7); atomic_fetch_add(&destructed, 1); }
static void *tls_worker(void *arg) {
    int key = *(int *)arg;
    assert(!get_specific(key));
    assert(set_specific(key, (void *)7) == 0 && get_specific(key) == (void *)7);
    return NULL;
}
static _Atomic int probe_tid, probe_release;
static int (*guest_tid)(void);
static void *liveness_worker(void *arg) {
    (void)arg;
    atomic_store(&probe_tid, guest_tid());
    while (!atomic_load(&probe_release)) usleep(1000);
    return NULL;
}
int main(void) {
    kl_thread_init();
    long (*guest_syscall)(long,long,long,long,long,long,long) = FN("syscall", long (*)(long,long,long,long,long,long,long));
    guest_tid = FN("gettid", int (*)(void));
    assert(guest_syscall && guest_tid);
    assert(!guest_syscall(131, getpid(), guest_tid(), 0, 0, 0, 0));
    pthread_t live_thread;
    assert(!pthread_create(&live_thread, NULL, liveness_worker, NULL));
    while (!atomic_load(&probe_tid)) usleep(1000);
    assert(probe_tid != guest_tid());
    assert(!guest_syscall(131, getpid(), probe_tid, 0, 0, 0, 0));
    errno = 0;
    assert(guest_syscall(131, getpid(), probe_tid, 1, 0, 0, 0) == -1 && errno == 38); // Linux ENOSYS, no signal delivered.
    atomic_store(&probe_release, 1);
    assert(!pthread_join(live_thread, NULL));
    // pthread_join observes userspace completion; Mach may retire its thread
    // port just afterward. Wait for that kernel transition, with a bound.
    long retired = 0;
    for (int i = 0; i < 1000; i++) {
        retired = guest_syscall(131, getpid(), probe_tid, 0, 0, 0, 0);
        if (retired == -1) break;
        usleep(1000);
    }
    if (retired != -1 || errno != ESRCH) fprintf(stderr, "retired=%ld errno=%d tid=%d\n", retired, errno, probe_tid);
    assert(retired == -1 && errno == ESRCH);
    assert(guest_syscall(131, getpid(), 0, 0, 0, 0, 0) == -1 && errno == EINVAL);
    assert(guest_syscall(131, getpid()+1, guest_tid(), 0, 0, 0, 0) == -1 && errno == 38);
    char tmpdir[] = "/tmp/klepton-shmem.XXXXXX";
    assert(mkdtemp(tmpdir));
    mode_t old_umask = umask(0077);
    int shared = kl_open_mapped(-100, tmpdir, 0x404002 | 0x80000, 0666);
    umask(old_umask);
    assert(shared >= 0);
    struct stat status;
    assert(!fstat(shared, &status) && S_ISREG(status.st_mode) && !status.st_nlink);
    assert((status.st_mode & 0777) == 0600);
    assert(fcntl(shared, F_GETFD) & FD_CLOEXEC);
    assert(!ftruncate(shared, 4096));
    unsigned char *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shared, 0);
    assert(mapping != MAP_FAILED);
    int alias = dup(shared);
    assert(alias >= 0 && !close(shared));
    assert(pwrite(alias, "steam", 5, 0) == 5 && !memcmp(mapping, "steam", 5));
    assert(!close(alias) && !memcmp(mapping, "steam", 5));
    assert(!munmap(mapping, 4096));
    DIR *dir = opendir(tmpdir); assert(dir);
    struct dirent *item;
    while ((item = readdir(dir))) assert(!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."));
    assert(!closedir(dir));
    errno = 0;
    assert(kl_open_mapped(-100, tmpdir, 0x404000, 0600) == -1 && errno == EINVAL);
    int parent = open(tmpdir, O_RDONLY | O_DIRECTORY); assert(parent >= 0);
    shared = kl_open_mapped(parent, ".", 0x404002, 0600);
    assert(shared >= 0 && !close(shared) && !close(parent));
    assert(!rmdir(tmpdir));
    void *(*cpu_alloc)(size_t) = FN("__sched_cpualloc", void *(*)(size_t));
    void (*cpu_free)(void *) = FN("__sched_cpufree", void (*)(void *));
    int (*cpu_count)(size_t, const void *) = FN("__sched_cpucount", int (*)(size_t,const void *));
    assert(cpu_alloc && cpu_free && cpu_count);
    const size_t cpu_sizes[] = {1, 64, 65, 1025};
    for (unsigned i = 0; i < sizeof cpu_sizes / sizeof cpu_sizes[0]; i++) {
        size_t n = cpu_sizes[i], bytes = ((n + 63) / 64) * 8;
        uint64_t *mask = cpu_alloc(n);
        assert(mask);
        memset(mask, 0, bytes);
        mask[(n - 1) / 64] |= UINT64_C(1) << ((n - 1) % 64);
        assert(cpu_count(bytes, mask) == 1);
        cpu_free(mask);
    }
    errno = 0;
    assert(!cpu_alloc(SIZE_MAX) && errno == ENOMEM);
    cpu_free(NULL);
    int (*eventfd_fn)(unsigned, int) = FN("eventfd", int (*)(unsigned, int));
    ssize_t (*write_fn)(int, const void *, size_t) = FN("write", ssize_t (*)(int,const void *,size_t));
    ssize_t (*read_fn)(int, void *, size_t) = FN("read", ssize_t (*)(int,void *,size_t));
    ssize_t (*checked_write)(int, const void *, size_t, size_t) = FN("__write_chk", ssize_t (*)(int,const void *,size_t,size_t));
    int (*close_fn)(int) = FN("close", int (*)(int));
    int (*dup_fn)(int) = FN("dup", int (*)(int));
    int (*create_epoll)(int) = FN("epoll_create1", int (*)(int));
    int (*ctl_epoll)(int,int,int,void *) = FN("epoll_ctl", int (*)(int,int,int,void *));
    int (*wait_epoll)(int,void *,int,int) = FN("epoll_wait", int (*)(int,void *,int,int));
    assert(eventfd_fn && read_fn && write_fn && checked_write && close_fn && dup_fn);
    assert(create_epoll && ctl_epoll && wait_epoll);
    int fd = eventfd_fn(0, 1 | 0x800);
    assert(fd >= 0);
    int epfd = create_epoll(0);
    lx_event interest = {1,0,UINT64_C(0xabcdef1234567890)}, out = {0};
    assert(epfd >= 0 && ctl_epoll(epfd, 1, fd, &interest) == 0);
    assert(wait_epoll(epfd, &out, 1, 0) == 0);
    uint64_t value = 2;
    assert(write_fn(fd, &value, sizeof value) == 8);
    assert(wait_epoll(epfd, &out, 1, 0) == 1 && out.events == 1 && out.data == interest.data);
    value = 0;
    assert(read_fn(fd, &value, sizeof value) == 8 && value == 1);
    assert(wait_epoll(epfd, &out, 1, 0) == 1); // Semaphore still readable.
    assert(read_fn(fd, &value, sizeof value) == 8 && value == 1);
    assert(wait_epoll(epfd, &out, 1, 0) == 0);
    int copy = dup_fn(fd);
    assert(copy >= 0 && close_fn(fd) == 0);
    value = 1;
    assert(checked_write(copy, &value, 8, 8) == 8);
    assert(read_fn(copy, &value, 8) == 8 && value == 1);
    assert(close_fn(copy) == 0 && close_fn(epfd) == 0);
    int regular[2]; assert(pipe(regular) == 0);
    assert(write_fn(regular[1], "ok", 2) == 2);
    char bytes[2]; assert(read_fn(regular[0], bytes, 2) == 2 && bytes[0] == 'o');
    assert(close_fn(regular[0]) == 0 && close_fn(regular[1]) == 0);
    int (*key_create)(int *, void (*)(void *)) = FN("pthread_key_create", int (*)(int *,void (*)(void *)));
    int (*key_delete)(int) = FN("pthread_key_delete", int (*)(int));
    get_specific = FN("pthread_getspecific", void *(*)(int));
    set_specific = FN("pthread_setspecific", int (*)(int,const void *));
    struct { int key; uint32_t guard; } storage = {0,0xabcdef12};
    assert(key_create && key_delete && get_specific && set_specific);
    assert(key_create(&storage.key, tls_destructor) == 0);
    assert(((uint32_t)storage.key & 0x80000000) && storage.guard == 0xabcdef12);
    assert(!get_specific(0) && set_specific(0, (void *)1) == EINVAL);
    assert(!get_specific(-1) && key_delete(-1) == EINVAL);
    assert(set_specific(storage.key, (void *)3) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, tls_worker, &storage.key) == 0);
    assert(pthread_join(thread, NULL) == 0 && atomic_load(&destructed) == 1);
    assert(get_specific(storage.key) == (void *)3);
    int old = storage.key;
    assert(key_delete(old) == 0 && !get_specific(old));
    assert(key_create(&storage.key, NULL) == 0);
    assert(!get_specific(storage.key)); // Recycled slots cannot recover stale data.
    assert(key_delete(storage.key) == 0);
    puts("Steam IPC hooks: thread liveness, anonymous file backing/aliases/mappings/permissions, CPU masks, eventfd/epoll and Android TLS passed");
}
