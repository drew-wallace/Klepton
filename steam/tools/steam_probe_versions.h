// Read-only public Steamworks branch metadata. Valve calls stay on the SDK
// worker; Swift only reads a mutex-protected snapshot. No install/update APIs.
#ifndef KL_STEAM_PROBE_VERSIONS_H
#define KL_STEAM_PROBE_VERSIONS_H
#include <pthread.h>
#include <stdint.h>
#include <string.h>

typedef int (*probe_num_betas_fn)(void *, int *, int *);
typedef _Bool (*probe_beta_info_fn)(void *, int, uint32_t *, uint32_t *,
                                   char *, int, char *, int);
typedef _Bool (*probe_beta_info_dated_fn)(void *, int, uint32_t *, uint32_t *,
                                         char *, int, char *, int, uint32_t *);
typedef void *(*probe_find_user_interface_fn)(int32_t, const char *);
typedef struct {
    void *apps;
    probe_num_betas_fn count;
    probe_beta_info_dated_fn info;
} probe_release_api;
static probe_release_api version_release_api(probe_find_user_interface_fn find, int32_t user) {
    probe_release_api api = {0};
    if (!find || !user) return api;
    api.apps = find(user, "STEAMAPPS_INTERFACE_VERSION009");
    if (!api.apps) return api;
    // Public SDK 1.64 ISteamApps009 ABI (no virtual destructor): slots 30/31.
    // Never pass the new timestamp argument through the SDK 1.63 flat wrapper.
    // https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_164/isteamapps.h
    void **table = *(void ***)api.apps;
    if (table) { api.count = (void *)table[30]; api.info = (void *)table[31]; }
    if (!api.count || !api.info) memset(&api, 0, sizeof api);
    return api;
}
static pthread_mutex_t version_lock = PTHREAD_MUTEX_INITIALIZER;
// 0: waiting for Steam, 1: checking, 2: available, 3: unavailable.
static int version_state;
static uint32_t version_latest_build;
static uint32_t version_latest_updated;
static char version_latest_description[1024];
static double version_next_check, version_deadline;

static void version_publish_release(int state, uint32_t build, uint32_t updated,
                                    const char *description) {
    pthread_mutex_lock(&version_lock);
    version_state = state; version_latest_build = build;
    version_latest_updated = updated;
    memset(version_latest_description, 0, sizeof version_latest_description);
    if (description) strncpy(version_latest_description, description,
                             sizeof version_latest_description - 1);
    pthread_mutex_unlock(&version_lock);
}
static void version_publish(int state, uint32_t build) {
    version_publish_release(state, build, 0, NULL);
}
int32_t kl_steam_game_version_status(uint32_t *latest_build) {
    pthread_mutex_lock(&version_lock);
    int state = version_state;
    if (latest_build) *latest_build = version_latest_build;
    pthread_mutex_unlock(&version_lock);
    return state;
}
void kl_steam_game_release_details(uint32_t *updated, char *description, uint32_t capacity) {
    pthread_mutex_lock(&version_lock);
    if (updated) *updated = version_latest_updated;
    if (description && capacity) {
        strncpy(description, version_latest_description, capacity - 1);
        description[capacity - 1] = 0;
    }
    pthread_mutex_unlock(&version_lock);
}

static void version_check_release(void *apps, probe_num_betas_fn count,
                                  probe_beta_info_fn info,
                                  probe_beta_info_dated_fn dated_info, double now) {
    if (version_state == 2 || version_state == 3 || now < version_next_check) return;
    if (!apps || !count || (!info && !dated_info)) { version_publish(3, 0); return; }
    if (!version_deadline) { version_deadline = now + 30; version_publish(1, 0); }
    version_next_check = now + 1;
    // SDK 1.63's GetBetaInfo uses seven arguments after this; index zero is
    // the default public branch. Never substitute the installed build ID.
    // https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_163/isteamapps.h
    if (count(apps, NULL, NULL) > 0) {
        char name[128] = {0}, description[1024] = {0};
        uint32_t flags = 0, build = 0, updated = 0;
        _Bool success = dated_info ? dated_info(apps, 0, &flags, &build, name, sizeof name,
                                               description, sizeof description, &updated)
                                  : info(apps, 0, &flags, &build, name, sizeof name,
                                         description, sizeof description);
        if (success && build &&
            memchr(name, 0, sizeof name) && !strcmp(name, "public")) {
            // A valid build can still have no publisher description or date.
            if (!memchr(description, 0, sizeof description)) description[0] = 0;
            version_publish_release(2, build, updated, description);
            return;
        }
    }
    if (now >= version_deadline) version_publish(3, 0);
}
static void version_check(void *apps, probe_num_betas_fn count,
                          probe_beta_info_fn info, double now) {
    version_check_release(apps, count, info, NULL, now);
}
static void version_finish(void) {
    // Startup relinquishes its SDK before the game starts. Preserve a measured
    // result for this launch, but do not leave an unfinished check spinning.
    uint32_t build = 0;
    if (kl_steam_game_version_status(&build) != 2) version_publish(3, 0);
}
#endif
