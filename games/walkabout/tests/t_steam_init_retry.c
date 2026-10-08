// Exercise the actual probe controller against delayed SDK initialization.
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../steam/tools/steam_probe_callbacks.h"
typedef struct kl_image kl_image;
typedef struct { int imports_missing; } test_stats;
static kl_image *kl_find_image(const char *name) { return NULL; }
static kl_image *kl_load_recursive(const char *name) { return NULL; }
static void *kl_base(kl_image *image) { return NULL; }
static void *kl_sym(kl_image *image, const char *name) { return NULL; }
static const test_stats *kl_get_stats(kl_image *image) { static test_stats s; return &s; }
static double clock_now;
static double session_now(void) { return clock_now; }
static void session_clear(void *p, size_t n) { memset(p, 0, n); }
#define KL_WALKABOUT_API_PINNED 1
#define KL_STEAM_GAME_HOST 1
#include "../steam/steam_probe_game_api.h"
static int calls, response = 1, success_after;
static int sdk_logged_on, session_requests, web_requests;
static int shutdowns, cancellations;
static uint32_t session_handle, web_handle;
static int owns = 1;
static int version_queries;
static int num_betas(void *p, int *available, int *private_count) {
    version_queries++; return 1;
}
static _Bool beta_info(void *p, int index, uint32_t *flags, uint32_t *build,
                       char *name, int name_size, char *description, int description_size) {
    assert(index == 0); *flags = 0; *build = 123456;
    snprintf(name, name_size, "public"); description[0] = 0; return 1;
}
static _Bool beta_info_dated(void *p, int index, uint32_t *flags, uint32_t *build,
                             char *name, int name_size, char *description, int description_size,
                             uint32_t *updated) {
    *updated = 1780000000;
    return beta_info(p, index, flags, build, name, name_size, description, description_size);
}
static void *apps_table[33];
static void **apps_object = apps_table;
static void *find_user_interface(int32_t user, const char *version) {
    assert(user == 1 && !strcmp(version, "STEAMAPPS_INTERFACE_VERSION009"));
    return &apps_object;
}
static int init(char *error) {
    calls++;
    return success_after && calls >= success_after ? 0 : response;
}
static int object;
static void *interface(void) { return &object; }
static int32_t handle(void) { return 1; }
static uint32_t app_id(void *p) { return 1408230; }
static _Bool logged_on(void *p) { return sdk_logged_on; }
static _Bool subscribed(void *p, uint32_t app) { assert(app == 1408230); return owns; }
static uint32_t session_ticket(void *p, void *bytes, int capacity, uint32_t *size, const void *recipient) {
    assert(capacity == 1024 && recipient); session_requests++; *size = session_handle ? 3 : 0; if (*size) memcpy(bytes, "abc", 3); return session_handle;
}
static int dlc_count(void *p) { return 0; }
static _Bool set_recipient(void *p, const char *value) { assert(!strcmp(value, "PlayFab")); return 1; }
static uint32_t web_ticket(void *p, const char *service) {
    assert(!strcmp(service, "AzurePlayFab")); web_requests++; return web_handle;
}
static void no_args(void) {}
static void shutdown(void) { shutdowns++; }
static void cancel(void *p, uint32_t handle) { assert(handle != 0); cancellations++; }
static void frame(int32_t pipe) { assert(pipe == 1); }
static _Bool callback(int32_t pipe, probe_callback *p) { return 0; }
static void reset(void) {
    version_publish(0, 0); version_next_check = version_deadline = 0; version_queries = 0;
    memset(&game_api, 0, sizeof game_api);
    game_retry_requested = game_retry_allowed = game_status = game_result = 0;
    session_handle = web_handle = 0; owns = 1;
    clock_now = 0; calls = 0; response = 1; success_after = 0;
    sdk_logged_on = 0; session_requests = web_requests = 0;
    shutdowns = cancellations = 0;
    game_api.enabled = 1; game_api.init = init;
    game_api.get_user_interface = interface; game_api.get_apps_interface = interface;
    game_api.get_utils_interface = interface; game_api.get_pipe = handle; game_api.get_user = handle;
    game_api.get_app_id = app_id; game_api.logged_on = logged_on;
    game_api.subscribed = subscribed; game_api.session_ticket = session_ticket; game_api.web_ticket = web_ticket;
    game_api.dlc_count = dlc_count; game_api.set_recipient = set_recipient;
    game_api.shutdown = shutdown; game_api.cancel_ticket = cancel;
    game_api.manual_init = no_args; game_api.manual_frame = frame; game_api.manual_next = callback;
}
int main(void) {
    reset(); success_after = 3;
    game_advance(0); assert(!calls);
    game_advance(1); assert(calls == 1 && !game_api.initialized);
    clock_now = 4; game_advance(1); assert(calls == 1);
    clock_now = 5; game_advance(1); assert(calls == 2 && !game_api.initialized);
    clock_now = 10; game_advance(1); assert(calls == 3 && game_api.initialized);
    clock_now = 30; game_advance(1); assert(calls == 3);
    reset(); game_advance(1);
    for (int i = 1; i <= 62; i++) { clock_now = i * 5; game_advance(1); }
    assert(calls == 60 && !game_api.initialized);
    reset(); response = 3; game_advance(1);
    clock_now = 5; game_advance(1); assert(calls == 1 && !game_api.initialized);
    reset(); game_advance(1); clock_now = 5; game_advance(0); assert(calls == 1);
    reset(); success_after = 1; sdk_logged_on = 1;
    game_advance(1);
    assert(session_requests == 1 && web_requests == 1 && !game_api.requested && game_status == 6);
    clock_now = 4; game_advance(1); assert(session_requests == 1);
    clock_now = 5; game_advance(0); assert(session_requests == 1);
    for (int i = 1; i <= 62; i++) { clock_now = i * 5; game_advance(1); }
    assert(session_requests == 60 && web_requests == 60 && game_status == 8 && !game_api.requested);
    // First login returns no handles; the next attempt must restore user ID.
    reset(); success_after = 1; sdk_logged_on = 1; game_advance(1);
    session_handle = 11; web_handle = 12; clock_now = 5; game_advance(1);
    assert(game_api.tickets.user == 1 && game_api.requested);
    probe_session_ticket_response session = {.handle=11,.result=1};
    probe_web_ticket_response web = {.handle=12,.result=1,.size=3,.bytes={'x','y','z'}};
    assert(probe_ticket_callback(&game_api.tickets,1,163,&session,sizeof session)==1);
    assert(probe_ticket_callback(&game_api.tickets,1,168,&web,sizeof web)==1);
    game_advance(1); assert(game_status==7);
    // Partial request and timeout cancel the outstanding ticket, then recover.
    reset(); success_after=1; sdk_logged_on=1; session_handle=11;
    game_advance(1); assert(game_api.requested);
    clock_now=61; game_advance(1); assert(!game_api.requested && cancellations==1);
    session_handle=21; web_handle=22; clock_now=66; game_advance(1);
    assert(game_api.tickets.user==1 && game_api.requested);
    assert(!probe_ticket_callback(&game_api.tickets,1,163,&session,sizeof session)); // stale old handle
    session.handle=21; web.handle=22;
    assert(probe_ticket_callback(&game_api.tickets,1,163,&session,sizeof session)==1);
    assert(probe_ticket_callback(&game_api.tickets,1,168,&web,sizeof web)==1);
    game_advance(1); assert(game_status==7);
    // A transient callback result reissues genuine tickets; permanent denial does not loop.
    reset(); success_after=1; sdk_logged_on=1; session_handle=11; web_handle=12;
    game_advance(1); game_api.tickets.session_done=game_api.tickets.web_done=1;
    game_api.tickets.session_result=16; game_api.tickets.web_result=1;
    game_advance(1); assert(!game_api.requested && cancellations==2 && game_status==6);
    clock_now=5; game_advance(1); assert(session_requests==2 && game_api.tickets.user==1);
    game_api.tickets.session_done=game_api.tickets.web_done=1;
    game_api.tickets.session_result=5; game_api.tickets.web_result=1;
    game_advance(1); assert(!game_api.requested && game_status==8);
    clock_now=10; game_advance(1); assert(session_requests==2);
    assert(kl_steam_ticket_retry()==1 && kl_steam_ticket_retry()==0);
    game_advance(1); assert(session_requests==3 && game_api.tickets.user==1);
    // Exhausted zero-handle window can be restarted from UI without a process restart.
    reset(); success_after=1; sdk_logged_on=1;
    for (int i=0;i<=60;i++) { clock_now=i*5; game_advance(1); }
    assert(game_status==8 && !game_api.requested);
    assert(kl_steam_ticket_retry()==1); session_handle=11; web_handle=12;
    game_advance(1); assert(game_api.requested && game_api.tickets.user==1);
    // License metadata can arrive after native login; never fabricate ownership.
    reset(); success_after=1; sdk_logged_on=1; owns=0; game_advance(1);
    assert(!session_requests && game_status==5);
    owns=1; session_handle=11; web_handle=12; clock_now=5; game_advance(1);
    assert(session_requests==1 && game_api.requested);
    reset(); game_api.initialized = 1; game_api.requested = 1; game_status = 7;
    assert(!game_release_for_host() && !shutdowns); // status alone is insufficient
    game_api.tickets = (probe_ticket_gate){.session_handle=11, .web_handle=12,
        .session_size=4, .web_size=4, .session_done=1, .web_done=1,
        .session_result=1, .web_result=1};
    assert(game_release_for_host());
    assert(shutdowns == 1 && cancellations == 2 && game_status == 9 && !game_api.enabled);
    assert(!game_api.requested && !game_api.initialized && !game_api.tickets.session_size);
    assert(!game_release_for_host() && shutdowns == 1);
    game_advance(1); assert(!calls); // startup code cannot consume game callbacks
    // Version lookup waits for SDK login, runs even while tickets are delayed,
    // and remains available after startup releases the SDK to the game.
    reset(); success_after = 1;
    game_api.num_betas = num_betas; game_api.beta_info = beta_info;
    game_advance(1); assert(version_queries == 0);
    sdk_logged_on = 1; game_advance(1); assert(version_queries == 1);
    uint32_t latest = 0;
    assert(kl_steam_game_version_status(&latest) == 2 && latest == 123456);
    session_handle = 11; web_handle = 12; clock_now = 5; game_advance(1);
    session.handle = 11; web.handle = 12;
    assert(probe_ticket_callback(&game_api.tickets,1,163,&session,sizeof session)==1);
    assert(probe_ticket_callback(&game_api.tickets,1,168,&web,sizeof web)==1);
    game_advance(1); assert(game_release_for_host());
    assert(kl_steam_game_version_status(&latest) == 2 && latest == 123456);
    game_advance(1); assert(version_queries == 1);
    // Resolve Apps009 after initialization and use its dated method instead
    // of the packaged Apps008 wrapper, without changing the game's SDK.
    reset(); success_after = 1;
    apps_table[30] = (void *)num_betas; apps_table[31] = (void *)beta_info_dated;
    game_api.find_user_interface = find_user_interface;
    game_advance(1); assert(version_queries == 0);
    assert(game_api.release_api.apps == &apps_object);
    sdk_logged_on = 1; game_advance(1); assert(version_queries == 1);
    uint32_t updated = 0;
    assert(kl_steam_game_version_status(&latest) == 2 && latest == 123456);
    kl_steam_game_release_details(&updated, NULL, 0);
    assert(updated == 1780000000);
    puts("Steam delayed initialization, retry bounds, version failure and disconnect checks passed");
}
