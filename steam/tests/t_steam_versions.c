#include <assert.h>
#include <stdio.h>
#include "../tools/steam_probe_versions.h"

static int queries, branches, result, unterminated;
static uint32_t build;
static const char *branch;
static int count(void *apps, int *available, int *private_count) {
    assert(apps && !available && !private_count); queries++; return branches;
}
static _Bool info(void *apps, int index, uint32_t *flags, uint32_t *id,
                  char *name, int name_size, char *description, int description_size) {
    assert(apps && index == 0 && name_size == 128 && description_size == 1024);
    *flags = 0; *id = build;
    if (unterminated) memset(name, 'x', name_size);
    else snprintf(name, name_size, "%s", branch);
    snprintf(description, description_size, "fixture");
    return result;
}
static _Bool dated_info(void *apps, int index, uint32_t *flags, uint32_t *id,
                        char *name, int name_size, char *description, int description_size,
                        uint32_t *updated) {
    *updated = 1780000000;
    return info(apps, index, flags, id, name, name_size, description, description_size);
}
static void *table[33];
static void **object = table;
static void *find_interface(int32_t user, const char *version) {
    assert(user == 1 && !strcmp(version, "STEAMAPPS_INTERFACE_VERSION009"));
    return &object;
}
static void reset(void) {
    version_publish(0, 0); version_next_check = version_deadline = 0;
    queries = 0; branches = 1; result = 1; unterminated = 0;
    build = 123456; branch = "public";
}
int main(void) {
    int apps;
    uint32_t latest = 99;
    uint32_t updated = 99;
    char description[1024];
    reset(); assert(kl_steam_game_version_status(&latest) == 0 && latest == 0);
    version_check(&apps, count, info, 1);
    assert(kl_steam_game_version_status(&latest) == 2 && latest == build);
    version_check(&apps, count, info, 2); assert(queries == 1);
    version_finish(); assert(kl_steam_game_version_status(&latest) == 2 && latest == build);
    kl_steam_game_release_details(&updated, description, sizeof description);
    assert(updated == 0 && !strcmp(description, "fixture")); // SDK 1.63 has no date.
    // Only Apps009 is resolved for the dated method; older SDK wrappers are
    // never called with the new argument. Unsupported interfaces fall back.
    assert(!version_release_api(NULL, 1).apps);
    assert(!version_release_api(find_interface, 0).apps);
    assert(!version_release_api(find_interface, 1).apps);
    table[30] = (void *)count; table[31] = (void *)dated_info;
    probe_release_api api = version_release_api(find_interface, 1);
    assert(api.apps == &object && api.count == count && api.info == dated_info);
    reset(); version_check_release(api.apps, api.count, NULL, api.info, 1);
    assert(kl_steam_game_version_status(&latest) == 2 && latest == build);
    version_finish();
    kl_steam_game_release_details(&updated, description, sizeof description);
    assert(updated == 1780000000 && !strcmp(description, "fixture"));
    char tiny[2] = {'x', 'x'};
    kl_steam_game_release_details(NULL, tiny, sizeof tiny);
    assert(tiny[0] == 'f' && tiny[1] == 0);
    kl_steam_game_release_details(NULL, NULL, 0);
    // Delayed app metadata retries without publishing zero as a real version.
    reset(); branches = 0; version_check(&apps, count, info, 1);
    assert(kl_steam_game_version_status(&latest) == 1 && latest == 0);
    version_check(&apps, count, info, 1.5); assert(queries == 1);
    branches = 1; version_check(&apps, count, info, 2);
    assert(kl_steam_game_version_status(&latest) == 2 && latest == build);
    // Failure, wrong branch and malformed strings never look up to date.
    for (int scenario = 0; scenario < 5; scenario++) {
        reset();
        if (scenario == 0) result = 0;
        if (scenario == 1) build = 0;
        if (scenario == 2) branch = "beta";
        if (scenario == 3) unterminated = 1;
        if (scenario == 4) branches = -1;
        version_check(&apps, count, info, 1);
        version_check(&apps, count, info, 31);
        assert(kl_steam_game_version_status(&latest) == 3 && latest == 0);
    }
    reset(); version_check(&apps, NULL, info, 1);
    assert(kl_steam_game_version_status(NULL) == 3);
    reset(); version_check(&apps, count, NULL, 1);
    assert(kl_steam_game_version_status(NULL) == 3);
    reset(); version_finish(); assert(kl_steam_game_version_status(NULL) == 3);
    puts("Steam public build metadata, delayed availability and failure checks passed");
}
