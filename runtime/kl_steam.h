#ifndef KL_STEAM_H
#define KL_STEAM_H
#include <stddef.h>

// Steamworks is a guest library with a host-side dependency: libsteam_api.so
// expects a running Steam client and its libsteamclient IPC endpoint. These
// hooks are deliberately opt-in. They let a simulator run expose the exact
// Steam entry points Walkabout resolves, and can replace only the bootstrap
// calls with an offline answer while the rest of the API remains visible.

// Redirect the observed $HOME/Steam client-data tree when explicitly configured.
const char *kl_steam_guest_path(const char *path, char *buffer, size_t capacity);

void  kl_steam_trace_lookup(const char *name);
void  kl_steam_trace_dlopen(const char *path);
void *kl_steam_interpose(const char *name, void *real);

#endif // KL_STEAM_H
