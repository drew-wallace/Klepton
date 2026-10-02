// Disposable, credential-free experiment for the recovery image's helper.
// This is independent of the Steam account backend. It sends no service IPC
// commands and never infers authentication from a returned server pointer.
#ifndef KL_STEAM_SERVICE_PROBE_H
#define KL_STEAM_SERVICE_PROBE_H
#include <pthread.h>
#include <stdatomic.h>

#ifdef KL_STEAM_SERVICE_ABI_PINNED
static int (*service_thread_create)(pthread_t *, const void *, void *(*)(void *), void *);
static _Atomic unsigned service_threads_created;
static int service_thread_observe(pthread_t *out, const void *attr,
                                  void *(*entry)(void *), void *arg) {
    int result = service_thread_create(out, attr, entry, arg);
    const char *owner = kl_addr_image((void *)entry, NULL);
    if (!result && owner && !strcmp(owner, "steamservice.so"))
        atomic_fetch_add(&service_threads_created, 1);
    return result;
}
static int service_address(kl_image *image, void *method, size_t expected) {
    size_t offset = 0;
    const char *owner = kl_addr_image(method, &offset);
    const char *reference = kl_addr_image(kl_sym(image, "CreateInterface"), NULL);
    return owner && reference && !strcmp(owner, reference) && offset == expected;
}
#endif

int kl_steam_service_probe_run(const char *library, int unused) {
    (void)unused;
#ifndef KL_STEAM_SERVICE_ABI_PINNED
    (void)library;
    fprintf(stderr, "[steam-probe] service experiment requires the pinned recovery builder\n");
    return 3;
#else
    const char *run_id = getenv("KL_STEAM_PROBE_RUN_ID");
    if (run_id) printf("[steam-probe] run id: %s\n", run_id);
    setenv("KL_STEAM_OFFLINE", "0", 1);
    setenv("KL_STEAM_SKIP_RESTART_CHECK", "0", 1);
    const char *data = getenv("KL_STEAM_PROBE_DATA");
    if (data && *data) kl_jni_set_files_dir(data);
    kl_fault_install();
    kl_thread_init();
    service_thread_create = (void *)kl_shim_lookup("pthread_create");
    if (!service_thread_create) return 3;
    kl_interpose("pthread_create", (void *)service_thread_observe);
    kl_image *image = kl_load_auto(library);
    if (!image) { fprintf(stderr, "[steam-probe] service load failed\n"); return 3; }
    kl_register_image(library, image);
    const kl_stats *stats = kl_get_stats(image);
    printf("[steam-probe] service mapped; missing=%u tls_refused=%u x18_refused=%u\n",
           stats->imports_missing, stats->tls_refused, stats->x18_refused);
    unsigned missing_count = 0;
    const char *const *missing = kl_missing_imports(image, &missing_count);
    for (unsigned i = 0; i < missing_count; i++)
        printf("[steam-probe] service missing import: %s\n", missing[i]);
    // Audited AArch64 exports: StartThread forwards its sole C string to
    // InitIPC(name,false,false,true) and returns the native server singleton.
    // GetIPCServer takes no args; Stop and Shutdown also take no args.
    void *start = kl_sym(image, "SteamService_StartThread");
    void *get = kl_sym(image, "SteamService_GetIPCServer");
    void *stop = kl_sym(image, "SteamService_Stop");
    void *shutdown = kl_sym(image, "SteamService_Shutdown");
    if (!service_address(image, start, 0x24c7c8) ||
        !service_address(image, get, 0x24c7b8) ||
        !service_address(image, stop, 0x24c770) ||
        !service_address(image, shutdown, 0x24c788)) {
        fprintf(stderr, "[steam-probe] service export ABI mismatch\n"); return 3;
    }
    kl_run_init(image);
    printf("[steam-probe] service constructors returned\n");
    unsigned before = atomic_load(&service_threads_created);
    void *server = ((void *(*)(const char *))start)("KleptonServiceProbe");
    void *queried = ((void *(*)(void))get)();
    printf("[steam-probe] service StartThread returned; server_present=%d getter_matches=%d threads_created=%u\n",
           server != NULL, server && queried == server,
           atomic_load(&service_threads_created) - before);
    fflush(stdout);
    const struct timespec interval = {0, 10000000};
    for (unsigned i = 0; i < 100; i++) nanosleep(&interval, NULL);
    ((void (*)(void))stop)();
    printf("[steam-probe] service Stop returned\n"); fflush(stdout);
    ((void (*)(void))shutdown)();
    printf("[steam-probe] service Shutdown returned\n");
    printf("[steam-probe] runtime gate UNPROVEN; helper IPC requests, account login and tickets not tested\n");
    return server && queried == server ? 4 : 3;
#endif
}
#endif
