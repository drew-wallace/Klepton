// Route the audited Unity wrapper to Unity's actual visionOS Vivox SDK.
// Its C wrapper exports match the Android wrapper; the native SDK owns voice,
// network, cryptography, and callbacks instead of depending on Android Java.
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <dlfcn.h>
#include <TargetConditionals.h>
#include "kl_vivox.h"
#include "kl_jni.h"
#include "kl_env.h"

static void *g_native;
static const char g_handle[]="klepton-native-vivox";
#if TARGET_OS_VISION && !TARGET_OS_SIMULATOR
static pthread_once_t g_once=PTHREAD_ONCE_INIT;
static void load_native(void) {
    const char *directory=kl_env_str("KL_DYLIB_DIR",NULL);
    if (!directory) return;
    char path[2048];
    snprintf(path,sizeof path,"%s/KleptonVivox.framework/KleptonVivox",directory);
    g_native=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (g_native) fprintf(stderr,"  [vivox] loaded native visionOS SDK 16.12.1\n");
    else fprintf(stderr,"  [vivox] native framework unavailable: %s\n",dlerror());
}
#endif

void *kl_vivox_dlopen(const char *path) {
    if (!path) return NULL;
    const char *base=strrchr(path,'/');base=base?base+1:path;
    if (strcmp(base,"libVivoxNative.so") && strcmp(base,"libvivox-sdk.so") &&
        strcmp(base,"VivoxNative") && strcmp(base,"vivox-sdk")) return NULL;
    if (!kl_env_on("KL_VEGAS_NATIVE_VIVOX",1)) return NULL;
    const char *package=kl_jni_guest_package();long version=0;
    kl_jni_guest_version(&version,NULL);
    if (!package || strcmp(package,"com.LuckyVR.PokerStarsVR") || version!=1102036) return NULL;
#if TARGET_OS_VISION && !TARGET_OS_SIMULATOR
    pthread_once(&g_once,load_native);
#endif
    return g_native?(void*)g_handle:NULL;
}
int kl_vivox_is_handle(const void *handle) { return handle==g_handle; }
int kl_vivox_native_active(void) { return g_native!=NULL; }
void *kl_vivox_sym(const char *name) { return g_native && name?dlsym(g_native,name):NULL; }
