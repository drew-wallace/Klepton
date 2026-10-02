// Standalone client feasibility probe; never creates a diagnostic Steam identity.
// Inspect by default. --construct deliberately executes unproven client code in
// a disposable host process; run it with a timeout. It is not a login/ticket gate.
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_fault.h"
#include "kl_jni.h"
#include "kl_cacerts.h"
#include "steam_probe_login.h"

static int (*real_eventfd)(unsigned, int);
static int probe_eventfd(unsigned initial, int flags) {
    int r = real_eventfd(initial, flags);
    int saved = errno;
    fprintf(stderr, "[steam-probe] eventfd initial=%u flags=0x%x result=%d errno=%d\n",
            initial, flags, r, r < 0 ? saved : 0);
    errno = saved;
    return r;
}

#include "steam_probe_callbacks.h"

#include "steam_probe_session.h"
#include "steam_service_probe.h"

static int probe_run(const char *library, int interfaces) {
    int construct = interfaces >= 0;
    const char *run_id = getenv("KL_STEAM_PROBE_RUN_ID");
    if (run_id) printf("[steam-probe] run id: %s\n", run_id);
    // Offline success must never be interpreted as a feasibility result.
    setenv("KL_STEAM_OFFLINE", "0", 1);
    setenv("KL_STEAM_SKIP_RESTART_CHECK", "0", 1);
    const char *data = getenv("KL_STEAM_PROBE_DATA");
    if (data && *data) {
        kl_jni_set_files_dir(data);
        char pem_path[2048];
        snprintf(pem_path, sizeof pem_path, "%s/cacert.pem", data);
        FILE *pem = fopen(pem_path, "w");
        int result = pem ? kl_cacerts_write_pem(pem) : -1;
        if (pem && fclose(pem)) result = -1;
        if (result) { fprintf(stderr, "[steam-probe] trust anchor export failed\n"); return 3; }
        printf("[steam-probe] trusted CA anchors exported: %d\n", kl_cacert_count());
    }
    kl_fault_install();
    kl_thread_init();
    real_eventfd = (int (*)(unsigned, int))kl_shim_lookup("eventfd");
    if (real_eventfd) kl_interpose("eventfd", (void *)probe_eventfd);
    kl_image *image = kl_load_auto(library);
    if (!image) {
        fprintf(stderr, "[steam-probe] load failed: %s\n", kl_error());
        return 1;
    }
    kl_register_image(library, image);
    const kl_stats *s = kl_get_stats(image);
    printf("[steam-probe] mapped; imports=%u missing=%u tls_refused=%u x18_refused=%u\n",
           s->imports_bound, s->imports_missing, s->tls_refused, s->x18_refused);
    unsigned count = 0;
    const char *const *missing = kl_missing_imports(image, &count);
    for (unsigned i = 0; i < count; i++)
        printf("[steam-probe] missing import: %s\n", missing[i]);
    printf("[steam-probe] CreateInterface export: %s\n",
           kl_sym(image, "CreateInterface") ? "present" : "absent");
    struct { const char *file; void *base; const char *symbol; void *address; } info = {0};
    int (*guest_dladdr)(const void *, void *) = (void *)kl_shim_lookup("dladdr");
    if (!guest_dladdr || !guest_dladdr(kl_base(image), &info) || !info.file ||
        strcmp(info.file, kl_image_path(image)) || !strchr(info.file, '/')) {
        fprintf(stderr, "[steam-probe] dladdr image path invalid\n");
        return 3;
    }
    printf("[steam-probe] dladdr resolves backing path: %s\n", info.file);
    if (construct) {
        printf("[steam-probe] executing constructors; unresolved paths may trap\n");
        fflush(stdout);
        kl_run_init(image);
        printf("[steam-probe] constructors returned; login and tickets remain unproven\n");
    }
    if (interfaces > 0) {
        // ISteamClient023 order/signatures from Valve Steamworks 1.63 header:
        // https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_163/isteamclient.h
        // Private login state calls are restricted to the pinned login-probe mode.
        void *(*create)(const char *, int *) = kl_sym(image, "CreateInterface");
        if (!create) return 3;
        int result = -1;
        void *engine = NULL;
        if (interfaces >= 2) {
            // Experimental factory invocation only. No private vtable layout
            // is assumed and no login method or credentials are supplied.
            engine = create("CLIENTENGINE_INTERFACE_VERSION005", &result);
            printf("[steam-probe] client engine factory: %s result=%d\n", engine ? "present" : "absent", result);
            fflush(stdout);
        }
        int32_t backend_pipe = 0, backend_user = 0;
        void (*backend_release)(int32_t, int32_t) = NULL;
        _Bool (*backend_pipe_release)(int32_t) = NULL;
        if (interfaces >= 2) {
            // ARM64 exports in both hash-pinned artifacts were disassembled:
            // creation takes no args; pipe release takes HSteamPipe; global-user
            // creation takes a pipe output pointer; user release takes pipe then
            // user. No login call. Recovery offsets: 0xe54198, 0xe541b4,
            // 0xe54214, and 0xe5426c respectively.
            int32_t (*legacy_pipe)(void) = kl_sym(image, "Steam_CreateSteamPipe");
            int32_t (*legacy_global)(int32_t *) = kl_sym(image, "Steam_CreateGlobalUser");
            void (*legacy_release)(int32_t, int32_t) = kl_sym(image, "Steam_ReleaseUser");
            _Bool (*legacy_pipe_release)(int32_t) = kl_sym(image, "Steam_BReleaseSteamPipe");
            if (legacy_pipe && legacy_global && legacy_release && legacy_pipe_release) {
                int32_t pipe = legacy_pipe();
                printf("[steam-probe] legacy CreateSteamPipe: %d\n", pipe); fflush(stdout);
                if (pipe) legacy_pipe_release(pipe);
                pipe = 0;
                int32_t user = legacy_global(&pipe);
                printf("[steam-probe] legacy CreateGlobalUser: user=%d pipe=%d\n", user, pipe); fflush(stdout);
                backend_pipe = pipe; backend_user = user;
                backend_release = legacy_release; backend_pipe_release = legacy_pipe_release;
            } else {
                fprintf(stderr, "[steam-probe] backend startup exports unavailable\n");
                return 3;
            }
        }
        void *client = create("SteamClient023", &result);
        printf("[steam-probe] SteamClient023: %s factory_result=%d\n", client ? "present" : "absent", result);
        fflush(stdout);
        if (!client) return 3;
        void **vtable = *(void ***)client;
        int32_t (*pipe_create)(void *) = (void *)vtable[0];
        _Bool (*pipe_release)(void *, int32_t) = (void *)vtable[1];
        int32_t (*global_user)(void *, int32_t) = (void *)vtable[2];
        void (*user_release)(void *, int32_t, int32_t) = (void *)vtable[4];
        void *(*get_user)(void *, int32_t, int32_t, const char *) = (void *)vtable[5];
        void (*run_frame)(void *) = (void *)vtable[19];
        uint32_t (*ipc_calls)(void *) = (void *)vtable[20];
        _Bool (*shutdown)(void *) = (void *)vtable[22];
        // Verified recovery exports accept pipe + CallbackMsg_t*, and pipe.
        _Bool (*next_callback)(int32_t, probe_callback *) = kl_sym(image, "Steam_BGetCallback");
        void (*free_callback)(int32_t) = kl_sym(image, "Steam_FreeLastCallback");
        int probe_failed = 0;
        printf("[steam-probe] creating client pipe\n"); fflush(stdout);
        int32_t pipe = pipe_create(client);
        printf("[steam-probe] CreateSteamPipe: %d\n", pipe); fflush(stdout);
        if (pipe) {
            int32_t user = global_user(client, pipe);
            printf("[steam-probe] ConnectToGlobalUser: %d\n", user); fflush(stdout);
            if (user) {
                void *steam_user = get_user(client, user, pipe, "SteamUser023");
                printf("[steam-probe] SteamUser023: %s\n", steam_user ? "present" : "absent");
                if (steam_user) {
                    void **methods = *(void ***)steam_user;
                    // Public ISteamUser 1.63 slots 0 (handle), 1 (logged-on).
                    int32_t (*handle)(void *) = (void *)methods[0];
                    _Bool (*logged_on)(void *) = (void *)methods[1];
                    printf("[steam-probe] user handle matches: %d; BLoggedOn: %d\n",
                           handle(steam_user) == user, logged_on(steam_user));
                }
                if (interfaces >= 2) {
                    // The host owns the engine frame pump; SDK game callbacks
                    // alone do not establish that host lifecycle. Keep this
                    // bounded and off SwiftUI's main/rendering thread.
                    uint64_t ipc_total = ipc_calls(client);
                    unsigned callbacks = 0;
                    if (!next_callback || !free_callback) {
                        fprintf(stderr, "[steam-probe] callback exports unavailable\n");
                        probe_failed = 1;
                    } else {
                        printf("[steam-probe] pumping 500 host frames and draining callbacks\n");
                        fflush(stdout);
                        const struct timespec interval = {0, 10000000};
                        for (unsigned i = 0; i < 500 && !probe_failed; i++) {
                            run_frame(client);
                            probe_failed = drain_callbacks(pipe, next_callback, free_callback, &callbacks);
                            if (!probe_failed && backend_pipe != pipe)
                                probe_failed = drain_callbacks(backend_pipe, next_callback, free_callback, &callbacks);
                            ipc_total += ipc_calls(client);
                            nanosleep(&interval, NULL);
                        }
                        printf("[steam-probe] frame pump returned; callbacks=%u IPC calls=%llu\n",
                               callbacks, (unsigned long long)ipc_total);
                    }
                }
#ifndef KL_STEAM_GAME_HOST
                if (interfaces >= 3 && !probe_failed)
                    probe_failed = login_probe_without_credentials(image, engine, steam_user, client, backend_user, backend_pipe);
#endif
                if (interfaces == 4 && !probe_failed)
                    probe_failed = session_run(image, engine, steam_user, client, backend_user, backend_pipe, pipe, next_callback, free_callback);
                fflush(stdout);
                user_release(client, pipe, user);
            }
            printf("[steam-probe] BReleaseSteamPipe: %d\n", pipe_release(client, pipe));
        }
        if (backend_user && backend_pipe) backend_release(backend_pipe, backend_user);
        if (backend_pipe) backend_pipe_release(backend_pipe);
        if (interfaces >= 2) {
            _Bool stopped = shutdown(client);
            printf("[steam-probe] BShutdownIfAllPipesClosed: %d\n", stopped);
            fflush(stdout);
            if (!stopped) probe_failed = 1;
        }
        if (probe_failed) return 3;
    }
    printf("[steam-probe] runtime gate UNPROVEN; physical execution and game PlayFab login unverified\n");
    // Successful mapping/constructors are not success for the runtime milestone.
    return count || s->tls_refused || s->x18_refused ? 3 : 4;
}

int kl_steam_probe_run(const char *library, int interfaces) {
    int result = probe_run(library, interfaces);
    printf("[steam-probe] probe returned result=%d\n", result);
    if (interfaces == 4) session_publish(5, result);
    return result;
}

#ifndef KL_STEAM_PROBE_LIBRARY
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--construct") && strcmp(argv[2], "--interfaces") && strcmp(argv[2], "--backend") && strcmp(argv[2], "--login-probe"))) {
        fprintf(stderr, "usage: %s <client.so> [--construct|--interfaces|--backend|--login-probe]\n", argv[0]);
        return 2;
    }
    return kl_steam_probe_run(argv[1], argc == 2 ? -1 : !strcmp(argv[2], "--login-probe") ? 3 : !strcmp(argv[2], "--backend") ? 2 : !strcmp(argv[2], "--interfaces"));
}
#endif
