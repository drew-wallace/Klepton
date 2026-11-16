#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_env.h"
#include "kl_unity_compat.h"

typedef void (*settings_fn)(void *, const void *);
static _Atomic(settings_fn) g_settings;

static void configure_settings(void *settings, const void *unity_version) {
    // OculusPlugin.UserDefinedSettings is fifteen sequential UInt16 fields.
    // Verified in this APK's setter at +0xd1ec: it copies 30 input bytes into
    // its native settings at +0x10. spaceWarp is input +0x16 (native +0x26).
    // The setter also writes normalized settings back to this mutable record.
    if (settings) {
        uint16_t space_warp;
        memcpy(&space_warp,(char*)settings+0x16,sizeof space_warp);
        if (space_warp) {
            const uint16_t disabled=0;
            memcpy((char*)settings+0x16,&disabled,sizeof disabled);
            fprintf(stderr,"  [unity] Vegas: disabled unsupported Application SpaceWarp before XR texture creation\n");
        }
    }
    settings_fn real=atomic_load(&g_settings);
    if (real) real(settings,unity_version);
}

void *kl_unity_oculus_interpose(const char *name, void *real) {
    if (!name || strcmp(name,"SetUserDefinedSettings") || !real || kl_env_on("KL_VEGAS_SPACEWARP",0)) return NULL;
    const char *package=kl_jni_guest_package(); long version=0;
    kl_jni_guest_version(&version,NULL);
    if (!package || strcmp(package,"com.LuckyVR.PokerStarsVR") || version!=1102036) return NULL;
    size_t offset=0;const char *image=kl_addr_image(real,&offset);
    // Do not apply a structure layout from this binary to another SDK build.
    if (!image || strcmp(image,"libOculusXRPlugin.so") || offset!=0xd1ec) return NULL;
    atomic_store(&g_settings,(settings_fn)real);
    return (void*)configure_settings;
}

static int vivox_unavailable(const void *config, size_t config_size) {
    (void)config; (void)config_size;
    // VxcErrors.h: VxErrorNotSupported. Returning failure lets the managed
    // SDK handle it; the Android SDK otherwise aborts in GetJniServices.
    fprintf(stderr,"  [unity] Vegas: Vivox Android Java services unavailable; vx_initialize3 -> VxErrorNotSupported (5018)\n");
    return 5018;
}

void *kl_unity_vivox_interpose(const char *name, void *real) {
    if (!name || strcmp(name,"vx_initialize3") || !real || kl_env_on("KL_VEGAS_VIVOX",0)) return NULL;
    const char *package=kl_jni_guest_package();long version=0;
    kl_jni_guest_version(&version,NULL);
    if (!package || strcmp(package,"com.LuckyVR.PokerStarsVR") || version!=1102036) return NULL;
    size_t offset=0;const char *image=kl_addr_image(real,&offset);
    if (!image || strcmp(image,"libvivox-sdk.so") || offset!=0x8a3410) return NULL;
    return (void*)vivox_unavailable;
}
