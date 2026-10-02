#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../runtime/kl_env.h"
#include "kl_steam.h"

// Valve's Android backend builds its writable tree from HOME + "/Steam".
// Keep that real data in a selected app/scratch container without changing the
// host process's HOME or reusing a desktop Steam installation.
const char *kl_steam_guest_path(const char *path, char *buffer, size_t capacity) {
    const char *root = getenv("KL_STEAM_DATA_ROOT");
    const char *home = getenv("HOME");
    if (!root || !*root || !home || !*home || !path) return path;
    size_t n = strlen(home);
    while (n > 1 && home[n - 1] == '/') n--;
    if (strncmp(path, home, n) || strncmp(path + n, "/Steam", 6) ||
        (path[n + 6] && path[n + 6] != '/')) return path;
    int length = snprintf(buffer, capacity, "%s%s", root, path + n + 6);
    if (length < 0 || (size_t)length >= capacity) {
        if (capacity) buffer[0] = 0;
        errno = ENAMETOOLONG;
    }
    return buffer;
}

// The wrappers below use the public Steamworks flat-API signatures. They do
// not pretend to implement Steam: offline mode only gets the guest past the
// bootstrap decision so the next interface request can be observed safely.
// A real account, entitlement, callback or multiplayer result is never
// fabricated here.

typedef bool     (*steam_bool0_fn)(void);
typedef int      (*steam_init_internal_fn)(const char *, void *);
typedef int      (*steam_init_flat_fn)(char *);
typedef void     (*steam_void_fn)(void);
typedef void     (*steam_pipe_void_fn)(uint32_t);
typedef bool     (*steam_manual_next_fn)(uint32_t, void *);
typedef bool     (*steam_manual_result_fn)(uint32_t, uint64_t, void *, int, int, bool *);
typedef uint32_t (*steam_u32_fn)(void);
typedef bool     (*steam_restart_fn)(uint32_t);
typedef void    *(*steam_iface_fn)(const char *);
typedef void    *(*steam_user_iface_fn)(uint32_t, const char *);
typedef uint64_t (*steam_user_id_fn)(void *);
typedef const char *(*steam_persona_fn)(void *);
typedef uint64_t (*steam_request_stats_fn)(void *, uint64_t);
typedef uint32_t (*steam_auth_ticket_fn)(void *, void *, int, uint32_t *, void *);

static steam_bool0_fn       g_init;
static steam_init_internal_fn g_init_internal;
static steam_init_flat_fn   g_init_flat;
static steam_bool0_fn       g_init_safe;
static steam_bool0_fn       g_init_anonymous;
static steam_bool0_fn       g_is_running;
static steam_void_fn        g_shutdown;
static steam_void_fn        g_callbacks;
static steam_void_fn        g_release_thread;
static steam_void_fn        g_manual_init;
static steam_pipe_void_fn   g_manual_run_frame;
static steam_pipe_void_fn   g_manual_free;
static steam_manual_next_fn g_manual_next;
static steam_manual_result_fn g_manual_result;
static steam_u32_fn         g_get_user;
static steam_u32_fn         g_get_pipe;
static steam_restart_fn     g_restart;
static steam_iface_fn       g_create_interface;
static steam_user_iface_fn  g_find_user_interface;
static steam_user_iface_fn  g_find_game_server_interface;
static steam_auth_ticket_fn g_auth_ticket;
static int steam_trace(void);
static int steam_offline(void);

// These are the first post-bootstrap flat calls made by Walkabout.  Keep the
// values clearly diagnostic and deterministic: they let the managed layer
// exercise its local profile path without claiming a real Steam account or
// entitlement.  Network, callback and ownership APIs remain unsupported.
static const uint64_t g_offline_steam_id = UINT64_C(76561197960265729);
static const char g_offline_persona[] = "Klepton Offline";

static uint64_t steam_user_steam_id(void *self) {
    (void)self;
    static unsigned calls;
    unsigned n = calls++;
    if (steam_trace() && (n < 4 || n % 600 == 0))
        fprintf(stderr, "  [steam] SteamAPI_ISteamUser_GetSteamID -> %llu (offline diagnostic%s)\n",
                (unsigned long long)g_offline_steam_id,
                n >= 4 ? "; repeated calls sampled" : "");
    return g_offline_steam_id;
}
static const char *steam_friends_persona(void *self) {
    (void)self;
    static unsigned calls;
    unsigned n = calls++;
    if (steam_trace() && (n < 4 || n % 600 == 0))
        fprintf(stderr, "  [steam] SteamAPI_ISteamFriends_GetPersonaName -> %s (offline diagnostic%s)\n",
                g_offline_persona, n >= 4 ? "; repeated calls sampled" : "");
    return g_offline_persona;
}
static uint64_t steam_user_stats_request(void *self, uint64_t id) {
    (void)self; (void)id;
    static unsigned calls;
    unsigned n = calls++;
    if (steam_trace() && (n < 4 || n % 600 == 0))
        fprintf(stderr, "  [steam] SteamAPI_ISteamUserStats_RequestUserStats -> 0 (offline diagnostic; no callback%s)\n",
                n >= 4 ? "; repeated calls sampled" : "");
    return 0; // k_uAPICallInvalid: do not manufacture a callback result
}

static uint32_t steam_user_auth_ticket(void *self, void *ticket, int max_ticket,
                                       uint32_t *ticket_size, void *identity) {
    if (steam_offline()) {
        (void)self; (void)ticket; (void)max_ticket; (void)identity;
        if (ticket_size) *ticket_size = 0;
        if (steam_trace())
            fprintf(stderr, "  [steam] SteamAPI_ISteamUser_GetAuthSessionTicket "
                            "-> 0 (offline diagnostic; no signed ticket)\n");
        return 0;
    }
    uint32_t r = 0;
    if (g_auth_ticket)
        r = g_auth_ticket(self, ticket, max_ticket, ticket_size, identity);
    else if (ticket_size)
        *ticket_size = 0;
    if (steam_trace())
        fprintf(stderr, "  [steam] SteamAPI_ISteamUser_GetAuthSessionTicket "
                        "-> %u (%u bytes)\n", r, ticket_size ? *ticket_size : 0);
    return r;
}

// A deliberately small C++-ABI object used only by KL_STEAM_OFFLINE. The
// Steamworks.NET context initializer asks SteamClient023 for a long list of
// interface pointers and treats any NULL result as a failed bootstrap. Returning
// opaque objects lets initialization complete so the next real Steam call can
// be observed; their method tables return neutral values and perform no IPC.
// This is a diagnostic compatibility layer, not an entitlement implementation.
static uintptr_t steam_generic_zero(void) { return 0; }
static void *g_steam_generic_obj[128];
static void *g_steam_client_obj[1];
static void *g_steam_generic_vtable[128];
static void *g_steam_client_vtable[42];
static int g_steam_fake_ready;

// In offline mode every Steamworks.NET interface currently shares one opaque
// object. A single zero-return stub keeps startup alive, but it hides the
// method calls that matter for multiplayer because C++ interface calls do not
// pass through a named flat symbol. Give the first 64 vtable slots distinct
// varargs stubs so KL_TRACE_STEAM=1 records the actual slot traffic. Integer,
// pointer, and bool returns all use the same zero value; no callback or packet
// is fabricated by this tracer.
static void steam_fake_slot_trace(unsigned slot) {
    static unsigned calls;
    if (steam_trace() && calls++ < 2048)
        fprintf(stderr, "  [steam] offline interface vtable slot %u -> 0\n", slot);
}
#define KL_STEAM_SLOT_LIST(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) \
    X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) \
    X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) \
    X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)
#define KL_STEAM_DECLARE_SLOT(n) \
    static uintptr_t steam_fake_slot_##n(void *self, ...) { \
        (void)self; steam_fake_slot_trace(n); return 0; }
KL_STEAM_SLOT_LIST(KL_STEAM_DECLARE_SLOT)
#define KL_STEAM_SLOT_ADDR(n) (void *)steam_fake_slot_##n,
static void *g_steam_slot_fn[64] = { KL_STEAM_SLOT_LIST(KL_STEAM_SLOT_ADDR) };
#undef KL_STEAM_SLOT_ADDR
#undef KL_STEAM_DECLARE_SLOT

static uintptr_t steam_fake_pipe(void) { return 1; }
static uintptr_t steam_fake_true(void) { return 1; }
static uintptr_t steam_fake_iface(void) { return (uintptr_t)g_steam_generic_obj; }
static uintptr_t steam_fake_iface_common(void *self, uint32_t user,
                                         uint32_t pipe, const char *version) {
    (void)self; (void)user; (void)pipe;
    if (steam_trace()) fprintf(stderr, "  [steam] offline ISteamClient getter %s -> %p\n",
                               version ? version : "(null)", g_steam_generic_obj);
    return (uintptr_t)g_steam_generic_obj;
}
static uintptr_t steam_fake_iface_utils(void *self, uint32_t pipe,
                                        const char *version) {
    (void)self; (void)pipe;
    if (steam_trace()) fprintf(stderr, "  [steam] offline ISteamClient getter %s -> %p\n",
                               version ? version : "(null)", g_steam_generic_obj);
    return (uintptr_t)g_steam_generic_obj;
}

static void steam_fake_init(void) {
    if (g_steam_fake_ready) return;
    for (unsigned i = 0; i < 128; i++)
        g_steam_generic_vtable[i] = i < 64 ? g_steam_slot_fn[i] : (void *)steam_generic_zero;
    for (unsigned i = 0; i < 128; i++) g_steam_generic_obj[i] = NULL;
    g_steam_generic_obj[0] = g_steam_generic_vtable;
    for (unsigned i = 0; i < 42; i++) g_steam_client_vtable[i] = g_steam_slot_fn[i];
    g_steam_client_vtable[0] = (void *)steam_fake_pipe;
    g_steam_client_vtable[1] = (void *)steam_fake_true;
    g_steam_client_vtable[2] = (void *)steam_fake_pipe;
    g_steam_client_vtable[3] = (void *)steam_fake_pipe;
    g_steam_client_vtable[5] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[6] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[8] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[9] = (void *)steam_fake_iface_utils;
    g_steam_client_vtable[10] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[11] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[12] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[13] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[14] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[15] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[16] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[17] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[18] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[19] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[24] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[26] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[27] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[28] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[29] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[30] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[31] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[35] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[36] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[37] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[38] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[39] = (void *)steam_fake_iface_common;
    g_steam_client_vtable[40] = (void *)steam_fake_iface_common;
    g_steam_client_obj[0] = g_steam_client_vtable;
    g_steam_fake_ready = 1;
}

static int steam_trace(void) {
    return kl_env_on("KL_TRACE_STEAM", 0);
}

static int steam_offline(void) {
    return kl_env_on("KL_STEAM_OFFLINE", 0);
}

// SteamErrMsg is a 1024-byte buffer in the SDK. Keep unavailable-runtime
// failures inside ESteamAPIInitResult rather than returning an undefined -1.
static int steam_missing_client(char *error) {
    if (error)
        snprintf(error, 1024, "Klepton: no real Steam initialization entry point is available");
    return 2; // k_ESteamAPIInitResult_NoSteamClient
}

static int steam_name(const char *name) {
    if (!name) return 0;
    return strncmp(name, "SteamAPI_", 9) == 0 ||
           strncmp(name, "SteamInternal_", 14) == 0 ||
           strncmp(name, "SteamGameServer_", 16) == 0;
}

void kl_steam_trace_lookup(const char *name) {
    static unsigned count;
    if (!steam_trace() || !steam_name(name) || count++ >= 1024) return;
    fprintf(stderr, "  [steam] dlsym lookup %s\n", name);
}

void kl_steam_trace_dlopen(const char *path) {
    static unsigned count;
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    if (!steam_trace() || !base || !strstr(base, "steam") || count++ >= 128) return;
    fprintf(stderr, "  [steam] dlopen request %s\n", path);
}

static bool steam_init(void) {
    if (steam_offline()) {
        fprintf(stderr, "  [steam] SteamAPI_Init -> true (offline diagnostic; "
                        "no Steam client or entitlement was established)\n");
        return true;
    }
    bool r = g_init ? g_init() : false;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_Init -> %s\n",
                               r ? "true" : "false");
    return r;
}

// Walkabout resolves the internal bootstrap symbol directly. Keep the offline
// policy at that seam too, otherwise libsteam_api reaches for libsteamclient.so
// before the public SteamAPI_Init wrapper can answer.
// Steamworks.NET binds this symbol to ESteamAPIInitResult, where zero is
// k_ESteamAPIInitResult_OK. It is not a C bool: returning true (1) reports
// k_ESteamAPIInitResult_FailedGeneric, which is why the earlier diagnostic
// hook still made Steamworks.NET print "SteamAPI_Init() failed".
static int steam_init_internal(const char *checks, void *err) {
    if (steam_offline()) {
        if (err) ((char *)err)[0] = '\0';
        fprintf(stderr, "  [steam] SteamInternal_SteamAPI_Init -> OK(0) "
                        "(offline diagnostic; no Steam client or entitlement was established)\n");
        (void)checks;
        return 0; // k_ESteamAPIInitResult_OK
    }
    int r = g_init_internal ? g_init_internal(checks, err) : steam_missing_client(err);
    if (steam_trace()) fprintf(stderr, "  [steam] SteamInternal_SteamAPI_Init -> %d\n", r);
    return r;
}

static bool steam_init_safe(void) {
    if (steam_offline()) {
        fprintf(stderr, "  [steam] SteamAPI_InitSafe -> true (offline diagnostic)\n");
        return true;
    }
    bool r = g_init_safe ? g_init_safe() : false;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_InitSafe -> %s\n",
                               r ? "true" : "false");
    return r;
}

static int steam_init_flat(char *error) {
    if (steam_offline()) {
        if (error) error[0] = '\0';
        fprintf(stderr, "  [steam] SteamAPI_InitFlat -> 0 (offline diagnostic)\n");
        return 0; // k_ESteamAPIInitResult_OK
    }
    int r = g_init_flat ? g_init_flat(error) : steam_missing_client(error);
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_InitFlat -> %d\n", r);
    return r;
}

static bool steam_init_anonymous(void) {
    if (steam_offline()) {
        fprintf(stderr, "  [steam] SteamAPI_InitAnonymousUser -> true "
                        "(offline diagnostic)\n");
        return true;
    }
    bool r = g_init_anonymous ? g_init_anonymous() : false;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_InitAnonymousUser -> %s\n",
                               r ? "true" : "false");
    return r;
}

static bool steam_is_running(void) {
    if (steam_offline()) {
        if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_IsSteamRunning -> true "
                                          "(offline diagnostic)\n");
        return true;
    }
    bool r = g_is_running ? g_is_running() : false;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_IsSteamRunning -> %s\n",
                               r ? "true" : "false");
    return r;
}

static bool steam_restart(uint32_t appid) {
    // A simulator run must never try to relaunch itself through steam://.
    if (steam_offline()) {
        fprintf(stderr, "  [steam] SteamAPI_RestartAppIfNecessary(%u) -> false "
                        "(offline diagnostic)\n", appid);
        return false;
    }
    // A physical-device diagnostic can bypass only Steam's relaunch gate to
    // discover whether SteamAPI_Init can reach an installed client service.
    // This does not initialize Steam or manufacture an identity/ticket.
    if (kl_env_on("KL_STEAM_SKIP_RESTART_CHECK", 0)) {
        fprintf(stderr, "  [steam] SteamAPI_RestartAppIfNecessary(%u) -> false "
                        "(restart check skipped; Steam remains uninitialized)\n",
                appid);
        return false;
    }
    bool r = g_restart ? g_restart(appid) : false;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_RestartAppIfNecessary(%u) -> %s\n",
                               appid, r ? "true" : "false");
    return r;
}

static uint32_t steam_user(void) {
    if (steam_offline()) {
        static unsigned n;
        unsigned k = n++;
        if (steam_trace() && (k < 4 || k % 600 == 0))
            fprintf(stderr, "  [steam] SteamAPI_GetHSteamUser -> 1 (offline diagnostic)\n");
        return 1;
    }
    uint32_t r = g_get_user ? g_get_user() : 0;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_GetHSteamUser -> %u\n", r);
    return r;
}

static uint32_t steam_pipe(void) {
    if (steam_offline()) {
        static unsigned n;
        unsigned k = n++;
        if (steam_trace() && (k < 4 || k % 600 == 0))
            fprintf(stderr, "  [steam] SteamAPI_GetHSteamPipe -> 1 (offline diagnostic)\n");
        return 1;
    }
    uint32_t r = g_get_pipe ? g_get_pipe() : 0;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_GetHSteamPipe -> %u\n", r);
    return r;
}

static void steam_shutdown(void) {
    if (steam_offline()) {
        fprintf(stderr, "  [steam] SteamAPI_Shutdown (offline diagnostic)\n");
        return;
    }
    if (g_shutdown) g_shutdown();
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_Shutdown\n");
}

static void steam_callbacks(void) {
    if (steam_offline()) {
        if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_RunCallbacks (offline no-op)\n");
        return;
    }
    if (g_callbacks) g_callbacks();
    if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_RunCallbacks\n");
}

static void steam_manual_init(void) {
    if (steam_offline()) {
        if (steam_trace()) fprintf(stderr, "  [steam] SteamAPI_ManualDispatch_Init (offline no-op)\n");
        return;
    }
    if (g_manual_init) g_manual_init();
}

static void steam_manual_run_frame(uint32_t pipe) {
    if (steam_offline()) return;
    if (g_manual_run_frame) g_manual_run_frame(pipe);
}

static bool steam_manual_next(uint32_t pipe, void *msg) {
    if (steam_offline()) {
        (void)pipe; (void)msg;
        return false;
    }
    return g_manual_next ? g_manual_next(pipe, msg) : false;
}

static bool steam_manual_result(uint32_t pipe, uint64_t call, void *callback,
                                int callback_size, int callback_id, bool *failed) {
    if (steam_offline()) {
        (void)pipe; (void)call; (void)callback; (void)callback_size;
        (void)callback_id; if (failed) *failed = false;
        return false;
    }
    return g_manual_result ? g_manual_result(pipe, call, callback, callback_size,
                                              callback_id, failed) : false;
}

static void steam_manual_free(uint32_t pipe) {
    if (steam_offline()) return;
    if (g_manual_free) g_manual_free(pipe);
}

static void steam_release_thread(void) {
    if (steam_offline()) return;
    if (g_release_thread) g_release_thread();
}

static void *steam_create_interface(const char *version) {
    if (steam_offline()) {
        if (version && strcmp(version, "SteamClient023") == 0) {
            steam_fake_init();
            fprintf(stderr, "  [steam] SteamInternal_CreateInterface(\"%s\") -> %p "
                            "(offline diagnostic ISteamClient compatibility object)\n",
                    version, g_steam_client_obj);
            return g_steam_client_obj;
        }
        fprintf(stderr, "  [steam] SteamInternal_CreateInterface(\"%s\") -> %p "
                        "(offline diagnostic generic interface)\n",
                version ? version : "(null)", g_steam_generic_obj);
        steam_fake_init();
        return g_steam_generic_obj;
    }
    void *r = g_create_interface ? g_create_interface(version) : NULL;
    if (steam_trace()) fprintf(stderr, "  [steam] SteamInternal_CreateInterface(\"%s\") -> %p\n",
                               version ? version : "(null)", r);
    return r;
}

static void *steam_find_user_interface(uint32_t user, const char *version) {
    if (steam_offline()) {
        steam_fake_init();
        fprintf(stderr, "  [steam] SteamInternal_FindOrCreateUserInterface(%u, \"%s\") -> %p "
                        "(offline diagnostic generic interface)\n",
                user, version ? version : "(null)", g_steam_generic_obj);
        return g_steam_generic_obj;
    }
    void *r = g_find_user_interface ? g_find_user_interface(user, version) : NULL;
    if (steam_trace()) fprintf(stderr,
                               "  [steam] SteamInternal_FindOrCreateUserInterface(%u, \"%s\") -> %p\n",
                               user, version ? version : "(null)", r);
    return r;
}

static void *steam_find_game_server_interface(uint32_t user, const char *version) {
    if (steam_offline()) {
        steam_fake_init();
        fprintf(stderr, "  [steam] SteamInternal_FindOrCreateGameServerInterface(%u, \"%s\") -> %p "
                        "(offline diagnostic generic interface)\n",
                user, version ? version : "(null)", g_steam_generic_obj);
        return g_steam_generic_obj;
    }
    void *r = g_find_game_server_interface ? g_find_game_server_interface(user, version) : NULL;
    if (steam_trace()) fprintf(stderr,
                               "  [steam] SteamInternal_FindOrCreateGameServerInterface(%u, \"%s\") -> %p\n",
                               user, version ? version : "(null)", r);
    return r;
}

void *kl_steam_interpose(const char *name, void *real) {
    if (!steam_name(name) || (!steam_trace() && !steam_offline())) return NULL;

    // A symbol may pass through both the ELF export resolver and dlsym. The
    // export resolver already returns the wrapper, so a later dlsym can hand
    // that wrapper back here as `real`. Never replace the saved Steam entry
    // point with our own wrapper: doing so makes a non-offline call recurse
    // until the guest stack guard is hit.
#define SAVE_STEAM_ORIGINAL(slot, wrapper, type) \
    do { if (real && real != (void *)(wrapper)) slot = (type)real; } while (0)

    // Steamworks.NET's generated flat API wrappers call through ISteamClient's
    // C++ vtable. Newer SteamClient revisions insert private methods, so a
    // hand-sized vtable can drift for one getter even while earlier getters
    // appear correct. In offline mode answer every client interface getter at
    // the flat symbol boundary as well; this keeps the probe deterministic and
    // prints the exact symbol the guest asks for.
    if (steam_offline() && !strncmp(name, "SteamAPI_ISteamClient_GetISteam", 31)) {
        steam_fake_init();
        fprintf(stderr, "  [steam] %s -> %p (offline diagnostic generic interface)\n",
                name, g_steam_generic_obj);
        return (void *)steam_fake_iface;
    }

    if (steam_offline() && strcmp(name, "SteamAPI_ISteamUser_GetSteamID") == 0)
        return (void *)steam_user_steam_id;
    if (steam_offline() && strcmp(name, "SteamAPI_ISteamFriends_GetPersonaName") == 0)
        return (void *)steam_friends_persona;
    if (steam_offline() && strcmp(name, "SteamAPI_ISteamUserStats_RequestUserStats") == 0)
        return (void *)steam_user_stats_request;
    if (strcmp(name, "SteamAPI_ISteamUser_GetAuthSessionTicket") == 0) {
        SAVE_STEAM_ORIGINAL(g_auth_ticket, steam_user_auth_ticket, steam_auth_ticket_fn);
        return (void *)steam_user_auth_ticket;
    }

    if (strcmp(name, "SteamAPI_Init") == 0) {
        SAVE_STEAM_ORIGINAL(g_init, steam_init, steam_bool0_fn); return (void *)steam_init;
    }
    if (strcmp(name, "SteamInternal_SteamAPI_Init") == 0) {
        SAVE_STEAM_ORIGINAL(g_init_internal, steam_init_internal, steam_init_internal_fn);
        return (void *)steam_init_internal;
    }
    if (strcmp(name, "SteamAPI_InitSafe") == 0) {
        SAVE_STEAM_ORIGINAL(g_init_safe, steam_init_safe, steam_bool0_fn);
        return (void *)steam_init_safe;
    }
    if (strcmp(name, "SteamAPI_InitFlat") == 0) {
        SAVE_STEAM_ORIGINAL(g_init_flat, steam_init_flat, steam_init_flat_fn);
        return (void *)steam_init_flat;
    }
    if (strcmp(name, "SteamAPI_InitAnonymousUser") == 0) {
        SAVE_STEAM_ORIGINAL(g_init_anonymous, steam_init_anonymous, steam_bool0_fn);
        return (void *)steam_init_anonymous;
    }
    if (strcmp(name, "SteamAPI_IsSteamRunning") == 0) {
        SAVE_STEAM_ORIGINAL(g_is_running, steam_is_running, steam_bool0_fn);
        return (void *)steam_is_running;
    }
    if (strcmp(name, "SteamAPI_RestartAppIfNecessary") == 0) {
        SAVE_STEAM_ORIGINAL(g_restart, steam_restart, steam_restart_fn);
        return (void *)steam_restart;
    }
    if (strcmp(name, "SteamAPI_GetHSteamUser") == 0) {
        SAVE_STEAM_ORIGINAL(g_get_user, steam_user, steam_u32_fn); return (void *)steam_user;
    }
    if (strcmp(name, "SteamAPI_GetHSteamPipe") == 0) {
        SAVE_STEAM_ORIGINAL(g_get_pipe, steam_pipe, steam_u32_fn); return (void *)steam_pipe;
    }
    if (strcmp(name, "SteamAPI_Shutdown") == 0) {
        SAVE_STEAM_ORIGINAL(g_shutdown, steam_shutdown, steam_void_fn);
        return (void *)steam_shutdown;
    }
    if (strcmp(name, "SteamAPI_RunCallbacks") == 0) {
        SAVE_STEAM_ORIGINAL(g_callbacks, steam_callbacks, steam_void_fn);
        return (void *)steam_callbacks;
    }
    if (strcmp(name, "SteamAPI_ManualDispatch_Init") == 0) {
        SAVE_STEAM_ORIGINAL(g_manual_init, steam_manual_init, steam_void_fn);
        return (void *)steam_manual_init;
    }
    if (strcmp(name, "SteamAPI_ManualDispatch_RunFrame") == 0) {
        SAVE_STEAM_ORIGINAL(g_manual_run_frame, steam_manual_run_frame, steam_pipe_void_fn);
        return (void *)steam_manual_run_frame;
    }
    if (strcmp(name, "SteamAPI_ManualDispatch_GetNextCallback") == 0) {
        SAVE_STEAM_ORIGINAL(g_manual_next, steam_manual_next, steam_manual_next_fn);
        return (void *)steam_manual_next;
    }
    if (strcmp(name, "SteamAPI_ManualDispatch_GetAPICallResult") == 0) {
        SAVE_STEAM_ORIGINAL(g_manual_result, steam_manual_result, steam_manual_result_fn);
        return (void *)steam_manual_result;
    }
    if (strcmp(name, "SteamAPI_ManualDispatch_FreeLastCallback") == 0) {
        SAVE_STEAM_ORIGINAL(g_manual_free, steam_manual_free, steam_pipe_void_fn);
        return (void *)steam_manual_free;
    }
    if (strcmp(name, "SteamAPI_ReleaseCurrentThreadMemory") == 0) {
        SAVE_STEAM_ORIGINAL(g_release_thread, steam_release_thread, steam_void_fn);
        return (void *)steam_release_thread;
    }
    if (strcmp(name, "SteamInternal_CreateInterface") == 0) {
        SAVE_STEAM_ORIGINAL(g_create_interface, steam_create_interface, steam_iface_fn);
        return (void *)steam_create_interface;
    }
    if (strcmp(name, "SteamInternal_FindOrCreateUserInterface") == 0) {
        SAVE_STEAM_ORIGINAL(g_find_user_interface, steam_find_user_interface,
                            steam_user_iface_fn);
        return (void *)steam_find_user_interface;
    }
    if (strcmp(name, "SteamInternal_FindOrCreateGameServerInterface") == 0) {
        SAVE_STEAM_ORIGINAL(g_find_game_server_interface, steam_find_game_server_interface,
                            steam_user_iface_fn);
        return (void *)steam_find_game_server_interface;
    }
#undef SAVE_STEAM_ORIGINAL
    return NULL;
}
