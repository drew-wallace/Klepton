// Steam interposition regression tests. These mock forwarding, not credentials.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_steam.h"

static unsigned init_count, frame_count, free_count, next_count, cancel_free;
static uint32_t expected_pipe = 0x7155;
static uint8_t payload[8] = {1, 2, 3, 4};
typedef struct { int user, id; uint8_t *data; int size; } callback_msg;
static bool original_init(void) { init_count++; return false; }
static void original_frame(uint32_t pipe) { assert(pipe == expected_pipe); frame_count++; }
static void original_free(uint32_t pipe) {
    assert(pipe == expected_pipe); free_count++; cancel_free = 1;
}
static bool original_next(uint32_t pipe, void *out) {
    assert(pipe == expected_pipe); next_count++;
    *(callback_msg *)out = (callback_msg){42, 163, payload, sizeof payload};
    return true;
}
static bool original_result(uint32_t pipe, uint64_t call, void *out, int size,
                            int callback_id, bool *failed) {
    assert(pipe == expected_pipe && call == UINT64_C(0x123456789));
    assert(size == 8 && callback_id == 163);
    memcpy(out, payload, size); *failed = true; return false;
}
static uint32_t original_ticket(void *self, void *out, int capacity,
                                uint32_t *size, void *identity) {
    assert(self == (void *)0x1234 && identity == (void *)0x5678);
    if (capacity < 4) { *size = 0; return 0; }
    memcpy(out, payload, 4); *size = 4; return 77;
}

int main(void) {
    // Only the selected client data tree is redirected; sibling paths remain.
    char home_path[2048], mapped[2048];
    const char *home = getenv("HOME");
    assert(home && *home);
    snprintf(home_path, sizeof home_path, "%s/Steam/config/config.vdf", home);
    setenv("KL_STEAM_DATA_ROOT", "/probe/client", 1);
    assert(!strcmp(kl_steam_guest_path(home_path, mapped, sizeof mapped), "/probe/client/config/config.vdf"));
    snprintf(home_path, sizeof home_path, "%s/SteamOther/config", home);
    assert(kl_steam_guest_path(home_path, mapped, sizeof mapped) == home_path);
    unsetenv("KL_STEAM_DATA_ROOT");

    setenv("KL_TRACE_STEAM", "1", 1);
    setenv("KL_STEAM_OFFLINE", "0", 1);
    setenv("KL_VERBOSE_ALL", "0", 1);
    setenv("KL_STEAM_SKIP_RESTART_CHECK", "0", 1);
    bool (*init)(void) = kl_steam_interpose("SteamAPI_Init", original_init);
    assert(init && !init() && init_count == 1);
    assert(kl_steam_interpose("SteamAPI_Init", init) == (void *)init);
    assert(!init() && init_count == 2); // original never becomes the wrapper
    assert(kl_steam_interpose("SteamAPI_Init", NULL) == (void *)init);
    assert(!init() && init_count == 3);
    int (*init_flat)(char *) = kl_steam_interpose("SteamAPI_InitFlat", NULL);
    char error[1024] = {0};
    assert(init_flat(error) == 2 && strstr(error, "no real Steam"));
    assert(init_flat(NULL) == 2); // no runtime is an actual failure
    int (*init_internal)(const char *, void *) = kl_steam_interpose("SteamInternal_SteamAPI_Init", NULL);
    assert(init_internal("SteamClient023", error) == 2 && strstr(error, "no real Steam"));
    void *(*create)(const char *) = kl_steam_interpose("SteamInternal_CreateInterface", NULL);
    assert(create("SteamClient023") == NULL);

    void (*frame)(uint32_t) = kl_steam_interpose("SteamAPI_ManualDispatch_RunFrame", original_frame);
    void (*release)(uint32_t) = kl_steam_interpose("SteamAPI_ManualDispatch_FreeLastCallback", original_free);
    bool (*next)(uint32_t, void *) = kl_steam_interpose("SteamAPI_ManualDispatch_GetNextCallback", original_next);
    bool (*result)(uint32_t, uint64_t, void *, int, int, bool *) =
        kl_steam_interpose("SteamAPI_ManualDispatch_GetAPICallResult", original_result);
    frame(expected_pipe); assert(frame_count == 1);
    callback_msg msg;
    assert(next(expected_pipe, &msg));
    assert(msg.data == payload && msg.size == sizeof payload && !cancel_free);
    uint8_t copy[8]; bool failed = false;
    assert(!result(expected_pipe, UINT64_C(0x123456789), copy, 8, 163, &failed));
    assert(failed && memcmp(copy, payload, 8) == 0);
    release(expected_pipe); assert(free_count == 1 && cancel_free);
    assert(kl_steam_interpose("SteamAPI_ManualDispatch_RunFrame", frame) == (void *)frame);
    frame(expected_pipe); assert(frame_count == 2);

    uint8_t guarded[6]; memset(guarded, 0xaa, sizeof guarded);
    uint32_t size = 100;
    uint32_t (*ticket)(void *, void *, int, uint32_t *, void *) =
        kl_steam_interpose("SteamAPI_ISteamUser_GetAuthSessionTicket", NULL);
    assert(ticket(NULL, guarded, sizeof guarded, &size, NULL) == 0 && size == 0);
    assert(guarded[0] == 0xaa && guarded[5] == 0xaa);
    ticket = kl_steam_interpose("SteamAPI_ISteamUser_GetAuthSessionTicket", original_ticket);
    assert(ticket((void *)0x1234, guarded + 1, 3, &size, (void *)0x5678) == 0);
    assert(size == 0 && guarded[0] == 0xaa && guarded[5] == 0xaa);
    assert(ticket((void *)0x1234, guarded + 1, 4, &size, (void *)0x5678) == 77);
    assert(size == 4 && memcmp(guarded + 1, payload, 4) == 0);
    assert(guarded[0] == 0xaa && guarded[5] == 0xaa);
    setenv("KL_STEAM_OFFLINE", "1", 1);
    assert(ticket(NULL, guarded, sizeof guarded, &size, NULL) == 0 && size == 0);
    frame(expected_pipe); release(expected_pipe);
    assert(frame_count == 2 && free_count == 1);
    assert(!next(expected_pipe, &msg) && next_count == 1);
    puts("steam forwarding regressions passed (mock results; no real credentials)");
    return 0;
}
