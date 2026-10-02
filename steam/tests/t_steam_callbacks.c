#include <assert.h>
#include "../tools/steam_probe_callbacks.h"
static unsigned remaining, releases;
static int borrowed, malformed;
static unsigned char payload;
static _Bool next(int32_t pipe, probe_callback *msg) {
    assert(pipe == 7 && !borrowed);
    if (!remaining) return 0;
    remaining--; borrowed = 1;
    *msg = (probe_callback){1, 101, &payload, 1};
    if (malformed == 1) msg->size = -1;
    if (malformed == 2) msg->payload = NULL;
    return 1;
}
static void release(int32_t pipe) {
    assert(pipe == 7 && borrowed);
    borrowed = 0; releases++;
}
int main(void) {
    unsigned count = 32;
    remaining = 600;
    assert(drain_callbacks(7, next, release, &count) == 0);
    assert(remaining == 344 && releases == 256 && count == 288 && !borrowed);
    assert(drain_callbacks(7, next, release, &count) == 0);
    assert(remaining == 88 && releases == 512 && !borrowed);
    assert(drain_callbacks(7, next, release, &count) == 0);
    assert(!remaining && releases == 600 && !borrowed);
    for (malformed = 1; malformed <= 2; malformed++) {
        remaining = 2;
        assert(drain_callbacks(7, next, release, &count) == 1);
        assert(remaining == 1 && !borrowed);
    }
    puts("Steam callback batch ownership checks passed");
}
