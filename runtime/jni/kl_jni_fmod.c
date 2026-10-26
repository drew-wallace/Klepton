// org.fmod.FMOD — FMOD's Android Java helper class.
//
// FMOD's native core (libfmod.so) does not open the audio device itself on
// Android through raw syscalls; it goes through a small Java shim, org.fmod.FMOD
// (from fmod.jar), which owns the Android Context and answers the platform
// queries the native mixer needs — output sample rate, block size, whether the
// low-latency / AAudio fast paths are available. FMOD's JNI_OnLoad resolves this
// class and org.fmod.{AudioDevice,MediaCodec}; the native side then calls
// FMOD.checkInit() before it will bring the core up. With no host implementation
// the checkInit() call aborted (vampire), so FMOD::getGlobals / System::create
// never ran and there was no audio.
//
// The host answers the Java platform queries and sends FMOD output to
// kl_audio. Older builds use OpenSL; newer builds call AudioDevice.write with
// signed PCM. AAudio remains unavailable, and both supported routes share the
// same native output sink.
#include <stdint.h>
#include <stdlib.h>
#include "kl_audio.h"
#include "kl_jni.h"
#include "kl_jni_int.h"

#define FMODCLS "org/fmod/FMOD"

static klj_val klf_true(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 1};
}
static klj_val klf_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}
static klj_val klf_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}
// The Android Context FMOD stashes and hands back to its own code. The activity
// is the context here, exactly as GameActivity is elsewhere.
static klj_val klf_getContext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_activity()};
}
// The device's output format, as the native mixer configures itself from it.
// 48 kHz is what kl_audio's CoreAudio unit runs at; a mismatch just makes FMOD
// resample, but matching avoids a second resample on top of ours. 512 frames is
// a conventional Android output block.
static klj_val klf_sampleRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 48000};
}
static klj_val klf_blockSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 512};
}

// Newer FMOD Android builds send PCM through their Java AudioDevice rather
// than OpenSL. The supplied helper's args are channels, sample rate, DSP block
// length and block count; write's count is int16 samples, not bytes.
static const char klf_output_source;
static klj_val klf_device_init(void *env, void *self, const klj_val *a, int n) {
    klj_object *object = klj_as_object(self);
    if (!object || n != 4 || !a[0].j || !a[1].j) return (klj_val){.j = 0};
    if (!object->data) {
        object->data = calloc(1, sizeof(int));
        if (!object->data) return (klj_val){.j = 0};
        klj_own(self, free);
    }
    int ok = kl_audio_open((unsigned)a[1].j, (unsigned)a[0].j, 16) == 0;
    *(int *)object->data = ok;
    if (ok) kl_audio_play();
    return (klj_val){.j = ok};
}
static klj_val klf_device_close(void *env, void *self, const klj_val *a, int n) {
    klj_object *object = klj_as_object(self);
    if (object && object->data) *(int *)object->data = 0;
    // The output sink is shared with Unity/OpenSL. Stop this producer without
    // shutting down other active producers; its bounded queued audio drains.
    return (klj_val){0};
}
static klj_val klf_device_write(void *env, void *self, const klj_val *a, int n) {
    klj_object *object = klj_as_object(self);
    klj_array *samples = n == 2 ? klj_arr(a[0].l) : NULL;
    int count = n == 2 ? (int)a[1].j : -1;
    if (object && object->data && *(int *)object->data && samples &&
        samples->kind == 'S' && count > 0 && count <= samples->len)
        kl_audio_write_src(&klf_output_source, samples->data, (size_t)count * sizeof(int16_t));
    return (klj_val){0};
}

const klj_binding klj_bind_fmod[] = {
    {"org/fmod/AudioDevice", "init", "(IIII)Z", klf_device_init},
    {"org/fmod/AudioDevice", "close", "()V", klf_device_close},
    {"org/fmod/AudioDevice", "write", "([SI)V", klf_device_write},
    // The gate the native side checks before creating the core system.
    {FMODCLS, "checkInit",           "()Z",                    klf_true},
    // init(context)/close() — the Java side would store/clear the context; there
    // is nothing to store here (getContext answers from the activity), so no-ops.
    {FMODCLS, "init",                "(Landroid/content/Context;)V", klf_void},
    {FMODCLS, "init",                "(Lcom/unity3d/player/UnityPlayerActivity;)V", klf_void},
    {FMODCLS, "init",                "(Ljava/lang/Object;)V",  klf_void},
    {FMODCLS, "close",               "()V",                    klf_void},
    {FMODCLS, "getContext",          "()Landroid/content/Context;", klf_getContext},
    {FMODCLS, "getContext",          "()Ljava/lang/Object;",   klf_getContext},
    // Output format + fast-path capabilities the native mixer queries.
    {FMODCLS, "getOutputSampleRate", "()I",                    klf_sampleRate},
    {FMODCLS, "getOutputBlockSize",  "()I",                    klf_blockSize},
    {FMODCLS, "supportsLowLatency",  "()Z",                    klf_true},
    // AAudio reported UNsupported so FMOD stays on the OpenSL output Klepton
    // serves (kl_opensl); its AAudio backend is not wired here.
    {FMODCLS, "supportsAAudio",      "()Z",                    klf_false},
    {0}
};
