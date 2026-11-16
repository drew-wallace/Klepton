// The Oculus Platform SDK front door — our stand-in for libovrplatformloader.so.
//
// Reconnaissance first, because it changes what this file has to be. The shipped
// libovrplatformloader.so is a *loader*, not an implementation: 1335 exports, and
// its only NEEDs are libc/libdl/liblog. Its strings name what it really does —
// bind to com.oculus.horizon / com.oculus.platformsdkruntime through
// com.oculus.platform.loader.EntryPoint. The actual platform lives in the Oculus
// system app on a real headset and is not in the APK at all.
//
// So on this machine the platform is *genuinely absent*, and no amount of loading
// the real library changes that: it runs, its async init can never complete, and
// then its own assert fires —
//   "ovr_PopMessage was called before ovr_PlatformInitializeAndroid()!"
// — which is what killed the run right after the first presented frame.
//
// We therefore serve the soname ourselves, same shape as the GL, OpenSL and
// OVRPlugin gateways.
//
// ---------------------------------------------------------------------------
// The DRM line, which is the whole point of this file existing separately
//
// Of those 1335 exports, exactly two concern entitlement. The rest is social and
// services plumbing — Message, Room, User, Voip, Party, NetSync, Matchmaking,
// Leaderboard, CloudStorage, Achievements — none of which can work here anyway,
// because there is no backing runtime to forward to.
//
// There is also no cryptographic DRM anywhere in this: the loader links no crypto,
// and the game's own assets are plaintext (IL2CPP metadata reads with strings(1);
// 71 files under assets/bin/Data hold plaintext GLSL ES). Entitlement here is a
// policy answer from a network service, not an encryption key.
//
// That makes it *easy* to fake, which is exactly why this file refuses to.
// Ownership and purchase queries do not get a stub, do not get a permissive zero,
// and are not reachable by accident: kl_ovrplat_sym routes them to a handler that
// always aborts naming the entry point, whatever KL_PERMISSIVE says. Everything
// else is measured and answered as the trace forces it.
//
// Reporting that the platform is unavailable is true here; answering "yes,
// entitled" would not be.
#ifndef KL_OVRPLAT_H
#define KL_OVRPLAT_H
#include <stdio.h>
#include <stdint.h>

// A frontend may connect a real, app-scoped Meta account service. Callbacks
// must copy app_id before returning and complete each accepted request once.
// No credentials travel through environment variables or diagnostic logs.
enum kl_ovrplat_account_kind {
    KL_OVRPLAT_ACCOUNT_USER = 1,
    KL_OVRPLAT_ACCOUNT_TOKEN,
    KL_OVRPLAT_ACCOUNT_PROOF,
    KL_OVRPLAT_ACCOUNT_ORG_ID,
};
typedef void (*kl_ovrplat_account_provider)(uint64_t request, int kind,
                                           const char *app_id, uint64_t user_id,
                                           void *context);
typedef struct {
    uint64_t user_id;
    const char *oculus_id;
    const char *display_name;
    const char *image_url;
    const char *small_image_url;
    const char *string;
    const char *error;
    int32_t error_code;
    int32_t http_code;
} kl_ovrplat_account_result;
void kl_ovrplat_set_account_provider(kl_ovrplat_account_provider provider, void *context);
// Copies the result. Stale/duplicate completions return 0. Payload storage
// belongs to the SDK message until ovr_FreeMessage, even after sign-out.
int kl_ovrplat_complete_account(uint64_t request, const kl_ovrplat_account_result *result);

void *kl_ovrplat_dlopen(const char *soname);   // NULL if not the platform loader
int   kl_ovrplat_claims(const char *soname);   // the same test, without opening
int   kl_ovrplat_is_handle(const void *h);
void *kl_ovrplat_sym(const char *name);

// Which ovr_* the guest resolved and called, and which DRM entry points it asked
// for — the last of those is worth seeing even when nothing aborted.
void kl_ovrplat_report(FILE *f);

#endif
