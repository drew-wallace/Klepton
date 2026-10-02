// Exercise the public OpenSL vtables: streaming time, pause/stop, and Clear
// while a feeder has a buffer in flight. Slots are OpenSL ES's ABI order.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../runtime/media/kl_opensl.h"
typedef void **Itf;
#define METHOD(i, slot, type) ((type)(*(void ***)(i))[slot])
typedef int (*GetInterface)(Itf, void *, Itf *);
typedef int (*Realize)(Itf, int);
typedef int (*CreatePlayer)(Itf, Itf *, void *, void *, unsigned, void *, void *);
typedef int (*SetState)(Itf, unsigned);
typedef int (*GetTime)(Itf, unsigned *);
typedef int (*Enqueue)(Itf, const void *, unsigned);
typedef int (*Clear)(Itf);
typedef struct { unsigned count, index; } QueueState;
typedef int (*GetQueue)(Itf, QueueState *);
static atomic_uint callbacks;
static void complete(Itf q, void *ctx) { (void)q; (void)ctx; atomic_fetch_add(&callbacks, 1); }
static unsigned position(Itf p) { unsigned ms; assert(!METHOD(p, 3, GetTime)(p, &ms)); return ms; }
static QueueState queue(Itf q) { QueueState s; assert(!METHOD(q, 2, GetQueue)(q, &s)); return s; }
int main(void) {
    setenv("KL_AUDIO", "0", 1);
    Itf engine, ei, player, play, bq;
    int (*create)(Itf *, unsigned, void *, unsigned, void *, void *) = kl_opensl_sym("slCreateEngine");
    assert(!create(&engine, 0, NULL, 0, NULL, NULL));
    void *engineID = *(void **)kl_opensl_sym("SL_IID_ENGINE");
    assert(!METHOD(engine, 3, GetInterface)(engine, engineID, &ei));
    assert(!METHOD(ei, 2, CreatePlayer)(ei, &player, NULL, NULL, 0, NULL, NULL));
    assert(!METHOD(player, 3, GetInterface)(player, *(void **)kl_opensl_sym("SL_IID_PLAY"), &play));
    assert(!METHOD(player, 3, GetInterface)(player, *(void **)kl_opensl_sym("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), &bq));
    typedef int (*Register)(Itf, void (*)(Itf, void *), void *);
    assert(!METHOD(bq, 3, Register)(bq, complete, NULL));
    assert(!METHOD(player, 0, Realize)(player, 0));
    unsigned duration; assert(!METHOD(play, 2, GetTime)(play, &duration)); assert(duration == UINT32_MAX);
    // 100ms of PCM: pause leaves time stationary; stop resets it.
    int16_t pcm[4800 * 2] = {0};
    assert(!METHOD(bq, 0, Enqueue)(bq, pcm, sizeof pcm));
    assert(!METHOD(play, 0, SetState)(play, 3));
    for (int n = 0; n < 1000 && atomic_load(&callbacks) == 0; n++) usleep(1000);
    assert(atomic_load(&callbacks) == 1); assert(position(play) == 100);
    assert(!METHOD(play, 0, SetState)(play, 2)); usleep(20000); assert(position(play) == 100);
    assert(!METHOD(play, 0, SetState)(play, 1)); assert(position(play) == 0);
    assert(!METHOD(bq, 1, Clear)(bq)); assert(queue(bq).index == 0);
    // Clear must cancel the old completion, and must not cancel the next one.
    assert(!METHOD(bq, 0, Enqueue)(bq, pcm, sizeof pcm));
    assert(!METHOD(play, 0, SetState)(play, 3));
    usleep(20000); assert(queue(bq).count == 1);
    assert(!METHOD(bq, 1, Clear)(bq)); assert(queue(bq).count == 0);
    usleep(120000); assert(atomic_load(&callbacks) == 1); assert(position(play) == 0);
    assert(!METHOD(bq, 0, Enqueue)(bq, pcm, sizeof pcm));
    for (int n = 0; n < 1000 && atomic_load(&callbacks) < 2; n++) usleep(1000);
    assert(atomic_load(&callbacks) == 2); assert(position(play) == 100);
    typedef void (*Destroy)(Itf); METHOD(player, 6, Destroy)(player);
    puts("PASS: OpenSL streaming position, pause/stop, in-flight Clear and completion");
}
