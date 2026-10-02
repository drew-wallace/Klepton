// Exercise real delivery through the guest ABI while a worker is parked.
#include "klepton.h"
#include "kl_x18.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef struct { int32_t flags, pad; void *handler; uint64_t mask; void *restorer; } guest_action;
static int semaphore;
typedef struct {
    _Atomic int received, resumed, returned, observed, post_result, bad_mask;
    int futex;
} worker_state;
static _Thread_local worker_state *current_worker;
static _Atomic int wake_word;
static _Atomic int stop_mask_changer;
extern long klb_syscall(long, long, long, long, long, long, long);
static int (*guest_post)(int *), (*guest_mask)(int, const uint64_t *, uint64_t *);
static int (*guest_suspend)(const uint64_t *);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int ready, stop;
static int (*guest_jump_save)(void *);
static void (*guest_jump_restore)(void *, int);
// A client worker may change its own mask while the game's GC is active.
// Verify that these changes cannot leak into the suspended game workers.
static void *mask_changer(void *unused) {
    (void)unused;
    sigset_t blocked = 0xfbfee027u, empty = 0;
    while (!atomic_load(&stop_mask_changer)) {
        assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
        _Alignas(16) unsigned char jump_buffer[256];
        volatile int resumed_jump = guest_jump_save(jump_buffer);
        if (!resumed_jump) {
            assert(!pthread_sigmask(SIG_SETMASK, &empty, NULL));
            guest_jump_restore(jump_buffer, 1);
        }
        usleep(100);
        assert(!pthread_sigmask(SIG_SETMASK, &empty, NULL));
        usleep(100);
    }
    return NULL;
}
static void handler(int signal, void *info, void *context) {
    (void)info; (void)context;
    if (signal == 30) {
        int epoch = atomic_fetch_add(&current_worker->received, 1) + 1;
        atomic_store(&current_worker->post_result, guest_post(&semaphore));
        uint64_t mask = UINT64_C(0xffffbfd9) & ~(UINT64_C(1) << 23);
        while (atomic_load(&current_worker->resumed) < epoch) guest_suspend(&mask);
        atomic_fetch_add(&current_worker->returned, 1);
    }
}
static void resume_handler(int signal) {
    if (signal == 24) atomic_fetch_add(&current_worker->resumed, 1);
}
static void *worker(void *arg) {
    current_worker = arg;
    uint64_t signals = (UINT64_C(1) << 29) | (UINT64_C(1) << 23);
    assert(!guest_mask(1, &signals, NULL)); // Linux SIG_UNBLOCK
    // A translated guest has a live veneer target in this slot. Exercise the
    // same context-repair branch, even though this fixture is native C.
    assert(!pthread_setspecific(KLX_TSD_SLOT, (void *)handler));
    pthread_mutex_lock(&lock);
    ready++; pthread_cond_broadcast(&condition);
    while (!stop) {
        uint64_t current = 0;
        assert(!guest_mask(2, NULL, &current));
        if (current & signals) atomic_store(&current_worker->bad_mask, 1);
        atomic_store(&current_worker->observed, atomic_load(&current_worker->returned));
        if (current_worker->futex) {
            int expected = atomic_load(&wake_word);
            pthread_mutex_unlock(&lock);
            klb_syscall(98, (long)&wake_word, 128, expected, 0, 0, 0);
            pthread_mutex_lock(&lock);
        } else pthread_cond_wait(&condition, &lock);
    }
    pthread_mutex_unlock(&lock);
    assert(!pthread_setspecific(KLX_TSD_SLOT, NULL));
    return NULL;
}
#define FN(name, type) ((type)kl_shim_lookup(name))
int main(void) {
    kl_thread_init();
    setenv("KL_SIG_X18_REPAIR", "1", 1);
    int (*action)(int,const guest_action *,guest_action *) = FN("sigaction",int (*)(int,const guest_action *,guest_action *));
    int (*create)(pthread_t *,const void *,void *(*)(void *),void *) = FN("pthread_create",int (*)(pthread_t *,const void *,void *(*)(void *),void *));
    int (*kill_thread)(pthread_t,int) = FN("pthread_kill",int (*)(pthread_t,int));
    int (*init)(int *,int,unsigned) = FN("sem_init",int (*)(int *,int,unsigned));
    int (*wait)(int *,const struct timespec *) = FN("sem_timedwait",int (*)(int *,const struct timespec *));
    int (*destroy)(int *) = FN("sem_destroy",int (*)(int *));
    int (*value)(int *,int *) = FN("sem_getvalue",int (*)(int *,int *));
    int (*trywait)(int *) = FN("sem_trywait",int (*)(int *));
    guest_post = FN("sem_post",int (*)(int *));
    guest_mask = FN("pthread_sigmask",int (*)(int,const uint64_t *,uint64_t *));
    guest_suspend = FN("sigsuspend",int (*)(const uint64_t *));
    guest_jump_save = FN("setjmp", int (*)(void *));
    guest_jump_restore = FN("longjmp", void (*)(void *, int));
    assert(guest_jump_save && guest_jump_restore);
    assert(action && create && kill_thread && init && wait && destroy && value && trywait && guest_post && guest_mask && guest_suspend);
    struct sigaction old, old_resume;
    assert(!sigaction(30,NULL,&old));
    assert(!sigaction(24,NULL,&old_resume));
    guest_action in = {.flags=0x10000004,.handler=(void *)handler,.mask=UINT64_C(0xffffbfd9)};
    assert(!action(30,&in,NULL) && !init(&semaphore,0,0));
    guest_action resume = {.flags=0x10000000,.handler=(void *)resume_handler,.mask=UINT64_C(0xffffbfd9)};
    assert(!action(24,&resume,NULL));
    // Measured Steam-host inherited mask, including both GC signals. Preserve
    // the other blocked signals while the worker unblocks suspend/resume.
    sigset_t blocked = 0xfbfee027u, saved = 0;
    sigaddset(&blocked,30);
    sigaddset(&blocked,24);
    assert(!pthread_sigmask(SIG_BLOCK,&blocked,&saved));
    pthread_t changing_thread;
    assert(!pthread_create(&changing_thread,NULL,mask_changer,NULL));
    // Parallel delivery is important: the game's GC suspends several registered
    // workers at once, including workers parked through the translated futex.
    for (int count = 1; count <= 24; count += 23) {
        pthread_t threads[24];
        worker_state states[24] = {0};
        ready = stop = 0;
        atomic_store(&wake_word, 0);
        for (int i = 0; i < count; i++) {
            states[i].futex = count > 1 && (i & 1);
            assert(!create(&threads[i],NULL,worker,&states[i]));
        }
        pthread_mutex_lock(&lock);
        while (ready != count) pthread_cond_wait(&condition,&lock);
        pthread_mutex_unlock(&lock);
        for (int epoch = 1; epoch <= 32; epoch++) {
            for (int i = 0; i < count; i++) assert(!kill_thread(threads[i],30));
            if (epoch & 1) {
                struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline); deadline.tv_sec += 5;
                for (int i = 0; i < count; i++) assert(!wait(&semaphore,&deadline));
            } else {
                int posted = 0;
                for (int i = 0; posted != count && i < 5000; i++) {
                    assert(!value(&semaphore,&posted));
                    if (posted != count) usleep(1000);
                }
                assert(posted == count);
                for (int i = 0; i < count; i++) assert(!trywait(&semaphore));
            }
            for (int i = 0; i < count; i++) {
                assert(!states[i].post_result);
                assert(!kill_thread(threads[i],24));
            }
            for (int i = 0; i < count; i++) {
                for (int j = 0; states[i].returned < epoch && j < 5000; j++) usleep(1000);
                assert(states[i].returned == epoch);
            }
            pthread_mutex_lock(&lock);
            atomic_fetch_add(&wake_word, 1);
            pthread_cond_broadcast(&condition);
            klb_syscall(98, (long)&wake_word, 129, 24, 0, 0, 0);
            pthread_mutex_unlock(&lock);
            for (int i = 0; i < count; i++) {
                for (int j = 0; states[i].observed < epoch && j < 5000; j++) usleep(1000);
                assert(states[i].observed == epoch && !states[i].bad_mask);
            }
        }
        pthread_mutex_lock(&lock);
        stop = 1;
        atomic_fetch_add(&wake_word, 1);
        pthread_cond_broadcast(&condition);
        klb_syscall(98, (long)&wake_word, 129, 24, 0, 0, 0);
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < count; i++) {
            assert(!pthread_join(threads[i],NULL));
            assert(states[i].received == 32 && states[i].resumed == 32);
            assert(!states[i].post_result && !states[i].bad_mask);
        }
    }
    atomic_store(&stop_mask_changer, 1);
    assert(!pthread_join(changing_thread,NULL));
    assert(!pthread_sigmask(SIG_SETMASK,&saved,NULL));
    assert(!sigaction(30,&old,NULL) && !destroy(&semaphore));
    assert(!sigaction(24,&old_resume,NULL));
    puts("Single and 24-worker guest suspend/resume, futex parking and restored masks passed");
}
