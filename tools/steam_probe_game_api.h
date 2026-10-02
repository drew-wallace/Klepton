// Independent probe through Walkabout's genuine packaged Steamworks SDK.
#ifndef KL_STEAM_PROBE_GAME_API_H
#define KL_STEAM_PROBE_GAME_API_H
#include "steam_probe_ticket_gate.h"
static pthread_mutex_t game_status_lock = PTHREAD_MUTEX_INITIALIZER;
static int game_status, game_result, game_retry_requested, game_retry_allowed;
static void game_publish(int stage, int result) {
    pthread_mutex_lock(&game_status_lock); game_status = stage; game_result = result; pthread_mutex_unlock(&game_status_lock);
}
int32_t kl_steam_ticket_status(int32_t *result) {
    pthread_mutex_lock(&game_status_lock); int stage = game_status;
    if (result) *result = game_result; pthread_mutex_unlock(&game_status_lock); return stage;
}
// UI requests are consumed on the backend worker, which owns SDK callbacks.
int32_t kl_steam_ticket_retry(void) {
    pthread_mutex_lock(&game_status_lock);
    int accepted = game_retry_allowed && !game_retry_requested;
    if (accepted) game_retry_requested = 1;
    pthread_mutex_unlock(&game_status_lock);
    return accepted;
}
static void game_allow_retry(int allowed) {
    pthread_mutex_lock(&game_status_lock); game_retry_allowed = allowed; pthread_mutex_unlock(&game_status_lock);
}
static int game_take_retry(void) {
    pthread_mutex_lock(&game_status_lock); int requested = game_retry_requested;
    game_retry_requested = 0; pthread_mutex_unlock(&game_status_lock); return requested;
}
#ifdef KL_WALKABOUT_API_PINNED
static struct {
    kl_image *image, *backend_image;
    int enabled, initialized, authenticated_attempt, requested, reported, failed;
    int init_result;
    double init_retry_at, init_retry_deadline;
    int32_t pipe, user;
    void *steam_user, *apps, *utils;
    int (*init)(char *);
    void (*shutdown)(void), (*manual_init)(void), (*manual_frame)(int32_t), (*manual_free)(int32_t);
    _Bool (*manual_next)(int32_t, probe_callback *);
    int32_t (*get_pipe)(void), (*get_user)(void);
    void *(*get_user_interface)(void), *(*get_apps_interface)(void), *(*get_utils_interface)(void);
    _Bool (*logged_on)(void *), (*subscribed)(void *, uint32_t);
    int (*dlc_count)(void *);
    _Bool (*dlc_data)(void *, int, uint32_t *, _Bool *, char *, int);
    _Bool (*dlc_installed)(void *, uint32_t);
    _Bool (*set_recipient)(void *, const char *);
    uint32_t (*get_app_id)(void *);
    uint32_t (*session_ticket)(void *, void *, int, uint32_t *, const void *);
    uint32_t (*web_ticket)(void *, const char *);
    void (*cancel_ticket)(void *, uint32_t);
    probe_ticket_gate tickets;
    double ticket_deadline;
    double ticket_request_retry_at, ticket_request_deadline;
} game_api;
static int game_initialize(void) {
    char error[1024] = {0};
    int result = game_api.init(error);
    game_api.init_result = result;
    kl_image *client_image = kl_find_image("libsteamclient.so");
    if (client_image && game_api.backend_image)
        printf("[steam-probe] Walkabout client instance distinct=%d\n",
               kl_base(client_image) != kl_base(game_api.backend_image));
    // Error strings may name account/system details. Emit classifications only.
    printf("[steam-probe] Walkabout SteamAPI_InitFlat result=%d reason_no_client=%d reason_no_user=%d reason_user_interface=%d reason_connect_global=%d reason_app_context=%d\n",
           result, strstr(error, "running instance") != NULL, strstr(error, "Steam user") != NULL, strstr(error, "SteamUser023") != NULL,
           strstr(error, "ConnectToGlobalUser failed") != NULL, strstr(error, "Can't determine AppID") != NULL);
    session_clear(error, sizeof error);
    if (result) { game_publish(2, result); return 0; }
    game_api.initialized = 1;
    game_api.steam_user = game_api.get_user_interface();
    game_api.apps = game_api.get_apps_interface(); game_api.utils = game_api.get_utils_interface();
    game_api.pipe = game_api.get_pipe(); game_api.user = game_api.get_user();
    if (!game_api.steam_user || !game_api.apps || !game_api.utils || !game_api.pipe || !game_api.user) {
        game_publish(3, 0); game_api.failed = 1; return 0;
    }
    uint32_t app_id = game_api.get_app_id(game_api.utils);
    printf("[steam-probe] Walkabout SDK interfaces present; app_id=%u pipe=%d user=%d\n", app_id, game_api.pipe, game_api.user);
    if (app_id != 1408230) { game_publish(3, 0); game_api.failed = 1; return 0; }
    game_api.tickets.user = game_api.user;
    game_api.manual_init();
    game_publish(4, 0); // SDK initialized; login is independently required.
    return 1;
}
static void game_prepare(kl_image *backend_image) {
    game_api.backend_image = backend_image;
    game_api.enabled = getenv("KL_STEAM_WALKABOUT_API") && !strcmp(getenv("KL_STEAM_WALKABOUT_API"), "1");
    if (!game_api.enabled) return;
    // Steam's documented environment app context; ownership is still queried
    // from the genuine client. This is not an entitlement override.
    setenv("SteamAppId", "1408230", 1); setenv("SteamGameId", "1408230", 1);
    game_api.image = kl_load_recursive("libsteam_api.so");
    if (!game_api.image || kl_get_stats(game_api.image)->imports_missing) {
        printf("[steam-probe] Walkabout SDK load failed\n"); game_publish(3, 0); game_api.failed = 1; return;
    }
#define GAME_SYM(field, symbol) do { game_api.field = (void *)kl_sym(game_api.image, symbol); if (!game_api.field) { game_publish(3, 0); game_api.failed = 1; return; } } while (0)
    GAME_SYM(init, "SteamAPI_InitFlat"); GAME_SYM(shutdown, "SteamAPI_Shutdown");
    GAME_SYM(get_user_interface, "SteamAPI_SteamUser_v023"); GAME_SYM(get_apps_interface, "SteamAPI_SteamApps_v008");
    GAME_SYM(get_utils_interface, "SteamAPI_SteamUtils_v010"); GAME_SYM(get_pipe, "SteamAPI_GetHSteamPipe");
    GAME_SYM(get_user, "SteamAPI_GetHSteamUser"); GAME_SYM(logged_on, "SteamAPI_ISteamUser_BLoggedOn");
    GAME_SYM(get_app_id, "SteamAPI_ISteamUtils_GetAppID"); GAME_SYM(subscribed, "SteamAPI_ISteamApps_BIsSubscribedApp");
    GAME_SYM(dlc_count, "SteamAPI_ISteamApps_GetDLCCount"); GAME_SYM(dlc_data, "SteamAPI_ISteamApps_BGetDLCDataByIndex");
    GAME_SYM(dlc_installed, "SteamAPI_ISteamApps_BIsDlcInstalled");
    GAME_SYM(set_recipient, "SteamAPI_SteamNetworkingIdentity_SetGenericString");
    GAME_SYM(session_ticket, "SteamAPI_ISteamUser_GetAuthSessionTicket"); GAME_SYM(web_ticket, "SteamAPI_ISteamUser_GetAuthTicketForWebApi");
    GAME_SYM(cancel_ticket, "SteamAPI_ISteamUser_CancelAuthTicket"); GAME_SYM(manual_init, "SteamAPI_ManualDispatch_Init");
    GAME_SYM(manual_frame, "SteamAPI_ManualDispatch_RunFrame"); GAME_SYM(manual_next, "SteamAPI_ManualDispatch_GetNextCallback");
    GAME_SYM(manual_free, "SteamAPI_ManualDispatch_FreeLastCallback");
#undef GAME_SYM
    printf("[steam-probe] Walkabout genuine SDK loaded; requesting real initialization\n");
    game_publish(1, 0); game_initialize();
}
static void game_cancel_tickets(void) {
    if (!game_api.requested) return;
    if (game_api.tickets.session_handle) game_api.cancel_ticket(game_api.steam_user, game_api.tickets.session_handle);
    if (game_api.tickets.web_handle && game_api.tickets.web_handle != game_api.tickets.session_handle)
        game_api.cancel_ticket(game_api.steam_user, game_api.tickets.web_handle);
    game_api.requested = 0; session_clear(&game_api.tickets, sizeof game_api.tickets);
    printf("[steam-probe] Walkabout diagnostic tickets cancelled\n");
}
static void game_inspect_dlc(void) {
    int count = game_api.dlc_count(game_api.apps);
    printf("[steam-probe] Walkabout DLC enumeration count=%d bounded=%d\n", count, count >= 0 && count <= 64);
    if (count < 0 || count > 64) return;
    for (int i = 0; i < count; i++) {
        uint32_t app = 0; _Bool available = 0;
        struct { unsigned char before[16]; char name[256]; unsigned char after[16]; } guarded;
        memset(&guarded, 0xa5, sizeof guarded);
        int result = game_api.dlc_data(game_api.apps, i, &app, &available, guarded.name, sizeof guarded.name);
        int valid = 1;
        for (int j = 0; j < 16; j++) if (guarded.before[j] != 0xa5 || guarded.after[j] != 0xa5) valid = 0;
        if (result && (!app || !memchr(guarded.name, 0, sizeof guarded.name))) valid = 0;
        session_clear(&guarded, sizeof guarded);
        printf("[steam-probe] Walkabout DLC entry index=%d valid=%d returned=%d app_id=%u\n", i, valid, result, app);
        if (!valid) return;
        if (result) printf("[steam-probe] Walkabout DLC app_id=%u store_available=%d subscribed=%d installed=%d\n",
            app, available, game_api.subscribed(game_api.apps, app), game_api.dlc_installed(game_api.apps, app));
    }
    // Steam's enumerator may omit unowned DLC. This is an observed list, not
    // proof that every published course or purchased bundle was exercised.
}
static int game_ticket_transient(int result) {
    // Valve EResult: absent request, Fail, NoConnection, Busy, Timeout,
    // ServiceUnavailable, ConnectFailed, RemoteDisconnect, TryAnotherCM.
    return result == 0 || result == 2 || result == 3 || result == 10 || result == 16 ||
           result == 20 || result == 35 || result == 38 || result == 48;
}
static void game_retry_tickets(void) {
    game_cancel_tickets();
    if (session_now() + 5 < game_api.ticket_request_deadline) {
        game_api.reported = 0; game_api.ticket_request_retry_at = session_now() + 5;
        game_publish(6, 0);
        printf("[steam-probe] Walkabout ticket retry scheduled delay_seconds=5\n");
    } else { game_allow_retry(1); game_publish(8, 0); }
}
static void game_advance(int native_logged_on) {
    if (!game_api.enabled || game_api.failed) return;
    if (game_take_retry() && native_logged_on) {
        game_allow_retry(0); game_cancel_tickets(); game_api.reported = 0;
        game_api.ticket_request_retry_at = 0; game_api.ticket_request_deadline = 0;
        if (game_api.initialized) {
            game_api.shutdown(); game_api.initialized = 0;
            game_api.steam_user = game_api.apps = game_api.utils = NULL; game_api.pipe = game_api.user = 0;
        }
        game_api.authenticated_attempt = 0;
        game_api.init_retry_deadline = session_now() + 300; game_api.init_retry_at = session_now();
    }
    if (native_logged_on && !game_api.authenticated_attempt) {
        game_api.authenticated_attempt = 1;
        game_api.init_retry_deadline = session_now() + 300;
        game_api.init_retry_at = session_now() + 5;
        if (!game_api.initialized) game_initialize();
    } else if (native_logged_on && !game_api.initialized && game_api.init_result == 1 &&
               session_now() >= game_api.init_retry_at && session_now() < game_api.init_retry_deadline) {
        // The original SDK cleans up failed initialization. Retry its genuine
        // app connection while account app/license information can arrive.
        // Interface/version failures are not retried or replaced.
        game_api.init_retry_at = session_now() + 5;
        game_initialize();
    }
    if (!game_api.initialized) {
        if (native_logged_on && game_api.authenticated_attempt && game_api.init_result == 1 &&
            session_now() >= game_api.init_retry_deadline) { game_allow_retry(1); game_publish(8, game_api.init_result); }
        return;
    }
    if (native_logged_on && !game_api.logged_on(game_api.steam_user)) {
        if (session_now() >= game_api.init_retry_deadline) { game_allow_retry(1); game_publish(8, 3); }
        return;
    }
    if (native_logged_on && game_api.logged_on(game_api.steam_user) && !game_api.reported &&
        session_now() >= game_api.ticket_request_retry_at) {
        game_api.reported = 1;
        int owns = game_api.subscribed(game_api.apps, 1408230);
        printf("[steam-probe] Walkabout ownership subscribed=%d\n", owns);
        if (!game_api.ticket_request_deadline) game_api.ticket_request_deadline = game_api.init_retry_deadline;
        if (!owns) {
            if (session_now() + 5 < game_api.ticket_request_deadline) {
                game_api.reported = 0; game_api.ticket_request_retry_at = session_now() + 5; game_publish(5, 0);
            } else { game_allow_retry(1); game_publish(8, 9); }
            return;
        }
        if (!game_api.ticket_deadline) game_inspect_dlc();
        // Match the supplied game's observed request arguments. This remains
        // an independent diagnostic, not execution of its PlayFab consumer.
        struct { int32_t type, size; uint8_t data[128]; } recipient = {0};
        _Static_assert(sizeof recipient == 136, "SteamNetworkingIdentity ABI");
        if (!game_api.set_recipient(&recipient, "PlayFab")) {
            game_api.failed = 1; game_publish(8, 0); return;
        }
        // Cancellation clears the old gate, including its user ID. Every new
        // attempt must match callbacks against the real current SDK user.
        game_api.tickets.user = game_api.user;
        struct { uint8_t before[16], bytes[1024], after[16]; } guarded;
        memset(&guarded, 0xa5, sizeof guarded); uint32_t size = 0;
        game_api.tickets.session_handle = game_api.session_ticket(game_api.steam_user, guarded.bytes, sizeof guarded.bytes, &size, &recipient);
        session_clear(&recipient, sizeof recipient);
        int guard_ok = 1;
        for (unsigned i = 0; i < 16; i++) if (guarded.before[i] != 0xa5 || guarded.after[i] != 0xa5) guard_ok = 0;
        if (guard_ok && size <= sizeof guarded.bytes) {
            game_api.tickets.session_size = size; memcpy(game_api.tickets.session_bytes, guarded.bytes, size);
        }
        session_clear(&guarded, sizeof guarded);
        game_api.requested = 1;
        printf("[steam-probe] Walkabout session ticket requested handle_present=%d bytes=%u bounds_valid=%d recipient_set=1\n",
               game_api.tickets.session_handle != 0, size, guard_ok && size <= sizeof guarded.bytes);
        if (!guard_ok || size > sizeof guarded.bytes) {
            game_api.failed = 1; game_publish(8, 0); game_cancel_tickets(); return;
        }
        if (!game_api.tickets.session_handle) { game_api.tickets.session_done = 1; game_api.tickets.session_result = 0; }
        game_api.tickets.web_handle = game_api.web_ticket(game_api.steam_user, "AzurePlayFab");
        printf("[steam-probe] Walkabout Web API ticket requested handle_present=%d service_specific=1\n", game_api.tickets.web_handle != 0);
        if (!game_api.tickets.web_handle) { game_api.tickets.web_done = 1; game_api.tickets.web_result = 0; }
        if (!game_api.tickets.session_handle && !game_api.tickets.web_handle) {
            game_retry_tickets(); return;
        }
        if (game_api.tickets.web_handle && game_api.tickets.web_handle == game_api.tickets.session_handle) {
            game_api.failed = 1; game_publish(8, 0); game_cancel_tickets(); return;
        }
        game_api.ticket_deadline = session_now() + 60;
        if (game_api.ticket_deadline > game_api.ticket_request_deadline) game_api.ticket_deadline = game_api.ticket_request_deadline;
        game_allow_retry(0); game_publish(6, 0);
    }
    game_api.manual_frame(game_api.pipe);
    for (unsigned i = 0; i < 256; i++) {
        probe_callback callback = {0}; if (!game_api.manual_next(game_api.pipe, &callback)) break;
        int malformed = callback.size < 0 || (callback.size && !callback.payload);
        int accepted = malformed ? -1 : probe_ticket_callback(&game_api.tickets, callback.user, callback.id, callback.payload, callback.size);
        if (malformed || callback.id == 163 || callback.id == 168)
            printf("[steam-probe] Walkabout callback metadata id=%d size=%d user_matches=%d accepted=%d\n",
                   callback.id, callback.size, callback.user == game_api.tickets.user, accepted);
        if (accepted > 0) printf("[steam-probe] Walkabout ticket callback id=%d result=%d delivered=%d\n", callback.id,
            callback.id == 163 ? game_api.tickets.session_result : game_api.tickets.web_result, probe_tickets_delivered(&game_api.tickets));
        game_api.manual_free(game_api.pipe);
        if (accepted < 0) { game_api.failed = 1; game_publish(8, 0); game_cancel_tickets(); break; }
    }
    if (game_api.requested && !native_logged_on) { game_publish(8, 3); game_cancel_tickets(); game_allow_retry(1); }
    else if (probe_tickets_delivered(&game_api.tickets)) game_publish(7, 1);
    else if (game_api.requested && (session_now() > game_api.ticket_deadline || !native_logged_on ||
             (game_api.tickets.session_done && game_api.tickets.web_done && !probe_tickets_delivered(&game_api.tickets)))) {
        printf("[steam-probe] Walkabout ticket gate failed timeout=%d logged_on=%d session_done=%d session_result=%d web_done=%d web_result=%d\n",
               session_now() > game_api.ticket_deadline, native_logged_on, game_api.tickets.session_done,
               game_api.tickets.session_result, game_api.tickets.web_done, game_api.tickets.web_result);
        int transient =
            ((!game_api.tickets.session_done || game_api.tickets.session_result == 1 || game_ticket_transient(game_api.tickets.session_result)) &&
             (!game_api.tickets.web_done || game_api.tickets.web_result == 1 || game_ticket_transient(game_api.tickets.web_result)));
        if (transient) game_retry_tickets();
        else { game_cancel_tickets(); game_allow_retry(1); game_publish(8, 0); }
    }
}
static void game_shutdown(void) {
    if (!game_api.enabled) return;
    game_cancel_tickets();
    if (game_api.initialized) { game_api.shutdown(); game_api.initialized = 0; printf("[steam-probe] Walkabout SteamAPI_Shutdown returned\n"); }
}
#ifdef KL_STEAM_GAME_HOST
static int game_release_for_host(void) {
    if (!game_api.enabled || !game_api.initialized || game_api.failed ||
        game_status != 7 || !probe_tickets_delivered(&game_api.tickets)) return 0;
    // Release all startup ticket/callback ownership before Unity starts. The
    // backend remains alive; the game owns its subsequent original SDK pipe.
    game_allow_retry(0); game_shutdown(); game_api.enabled = 0;
    game_publish(9, 1);
    printf("[steam-probe] local backend ready for game; startup SDK released\n");
    return 1;
}
#endif
#else
static void game_prepare(kl_image *backend_image) { (void)backend_image; }
static void game_advance(int native_logged_on) { (void)native_logged_on; }
static void game_shutdown(void) {}
#endif
#endif
