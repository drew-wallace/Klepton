// Compile the real app bridge; dead stripping drops unrelated guest loaders.
// Run with: python3 tests/test_visionos_session.py
#include <assert.h>
#include <stdatomic.h>
#include "../visionos/Sources/kl_app.c"

static atomic_int completed;
static atomic_int frames;
void kl_driver_note_frame(void) { atomic_fetch_add(&frames, 1); }

static void *wait_for_display(void *unused) {
    guest_pace_wait();
    atomic_store(&completed, 1);
    return NULL;
}

int main(void) {
    // An offered frame from the old display must not run after suspension.
    kl_app_guest_publish();
    kl_app_guest_publish();
    kl_app_guest_suspend();
    assert(g_guest.consumed == g_guest.published);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, wait_for_display, NULL) == 0);
    // Exceed the normal one-second pacer timeout. Home must keep waiting.
    usleep(1200000);
    assert(!atomic_load(&completed));
    kl_app_guest_publish();
    pthread_join(thread, NULL);
    assert(atomic_load(&completed));
    assert(!g_guest.suspended);
    assert(atomic_load(&frames) == 1);

    // Repeated Home/reopen cycles use the same clock and never owe a backlog.
    for (int i = 0; i < 3; ++i) {
        atomic_store(&completed, 0);
        kl_app_guest_suspend();
        assert(pthread_create(&thread, NULL, wait_for_display, NULL) == 0);
        usleep(20000);
        assert(!atomic_load(&completed));
        kl_app_guest_publish();
        pthread_join(thread, NULL);
        assert(g_guest.consumed == g_guest.published);
    }

    // Explicit teardown must also wake a suspended waiter.
    atomic_store(&completed, 0);
    kl_app_guest_suspend();
    assert(pthread_create(&thread, NULL, wait_for_display, NULL) == 0);
    usleep(20000);
    pthread_mutex_lock(&g_guest.mu);
    g_guest.stop = 1;
    pthread_cond_broadcast(&g_guest.cv);
    pthread_mutex_unlock(&g_guest.mu);
    pthread_join(thread, NULL);
    assert(atomic_load(&completed));
    puts("PASS: suspended frame clock, repeated resume, discarded ticks, stop wakeup");
    return 0;
}
