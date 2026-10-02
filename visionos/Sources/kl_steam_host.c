// Integration of the measured local Steam backend, including physical testing.
// The project generator requires verified, pinned runtime artifacts. A device
// build permits testing; physical login and game acceptance still need evidence.
#define KL_STEAM_GAME_HOST 1
#define KL_STEAM_PROBE_LIBRARY 1
#define KL_STEAM_LOGIN_ABI_PINNED 1
#define KL_WALKABOUT_API_PINNED 1
#include "../../tools/steam_probe.c"

// Restore the app's working directory after the Steam backend's initialization,
// so Unity resolves its packaged resources from the same directory as at boot.
// Signal-mask clearing remains an explicitly selected comparison control.
#include <limits.h>
#include <signal.h>
static char host_initial_directory[PATH_MAX];
static sigset_t host_initial_mask;
void kl_steam_host_trace_context(const char *stage) {
    if (!kl_env_on("KL_TRACE_SIGMASK", 0)) return;
    sigset_t mask = 0;
    int result = pthread_sigmask(SIG_SETMASK, NULL, &mask);
    fprintf(stderr, "[steam-host-context] stage=%s thread=%p mask=%#x result=%d\n",
        stage, (void *)pthread_self(), mask, result);
}
void kl_steam_host_capture_context(void) {
    if (!getcwd(host_initial_directory, sizeof host_initial_directory))
        host_initial_directory[0] = 0;
    pthread_sigmask(SIG_SETMASK, NULL, &host_initial_mask);
}
int kl_steam_host_prepare_game(void) {
    char before_directory[PATH_MAX], after_directory[PATH_MAX];
    sigset_t before_mask = 0, after_mask = 0;
    int restore = kl_env_on("KL_STEAM_HOST_RESTORE_CWD", 1);
    int clear = kl_env_on("KL_STEAM_HOST_ZERO_GAME_MASK", 0);
    int before_matches = getcwd(before_directory, sizeof before_directory) &&
        host_initial_directory[0] && !strcmp(before_directory, host_initial_directory);
    int result = pthread_sigmask(SIG_SETMASK, NULL, &before_mask);
    if (result) return result;
    if (restore) {
        if (!host_initial_directory[0]) return ENOENT;
        if (chdir(host_initial_directory)) return errno;
    }
    if (clear) {
        sigset_t empty = 0;
        result = pthread_sigmask(SIG_SETMASK, &empty, NULL);
        if (result) return result;
    }
    result = pthread_sigmask(SIG_SETMASK, NULL, &after_mask);
    if (result) return result;
    int after_matches = getcwd(after_directory, sizeof after_directory) &&
        host_initial_directory[0] && !strcmp(after_directory, host_initial_directory);
    if (kl_env_on("KL_TRACE_SIGMASK", 0) || restore || clear)
        fprintf(stderr, "[steam-host-test] restore_cwd=%d zero_game_mask=%d cwd_matches_before=%d cwd_matches_after=%d mask_before=%#x mask_after=%#x initial_parent_mask=%#x\n",
            restore, clear, before_matches, after_matches, before_mask, after_mask, host_initial_mask);
    return 0;
}
