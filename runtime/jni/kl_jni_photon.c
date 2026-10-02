// Photon Voice AudioInAEC's public Java contract, measured from the supplied
// Walkabout DEX. Deliver PCM16 into its retained short[] before OnData(); OnStop
// follows the last callback. Capture and permission stay behind KleptonMic.
#include "kl_jni_int.h"
#include "kl_audio.h"
#include "klepton.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdatomic.h>
#define PHOTON_AUDIO "com/exitgames/photon/audioinaec/AudioInAEC"
#define PHOTON_CALLBACK PHOTON_AUDIO "$DataCallback"
#define PHOTON_START_SIG "(Landroid/app/Activity;L" PHOTON_CALLBACK ";IIIZZZ)Z"

const char *klj_photon_reflected_signature(const char *cls, const char *name,
                                          const char *sig, int is_static) {
    if (!cls || !name || !sig || is_static || strcmp(cls, PHOTON_AUDIO) || strcmp(name, "Start"))
        return sig;
    // The supplied Java Start method has exactly these eight arguments.
    // Unity's reflection helper uses assignability rather than exact equality:
    // its concrete activity and generated proxy still resolve to this method.
    static const char *activities[] = {"Landroid/app/Activity;", "Lcom/unity3d/player/UnityPlayerActivity;"};
    static const char *callbacks[] = {"L" PHOTON_CALLBACK ";", "Ljava/lang/Object;", "L" KLJ_CLASS_PROXY ";"};
    if (*sig != '(') return sig;
    for (unsigned i = 0; i < sizeof activities / sizeof activities[0]; i++) {
        size_t len = strlen(activities[i]);
        if (strncmp(sig + 1, activities[i], len)) continue;
        const char *second = sig + 1 + len;
        for (unsigned j = 0; j < sizeof callbacks / sizeof callbacks[0]; j++) {
            size_t count = strlen(callbacks[j]);
            if (!strncmp(second, callbacks[j], count) && !strcmp(second + count, "IIIZZZ)Z"))
                return PHOTON_START_SIG;
        }
    }
    return sig;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done;
    pthread_t thread;
    _Atomic int stop;
    int running;
    unsigned rate, channels, source_rate, source_channels;
    void *self, *callback, *buffer;
    int16_t *held;
    size_t capacity, have;
    double position;
} photon_audio;
static pthread_mutex_t capture_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static photon_audio *capture_owner;
static photon_audio *photon_state(void *self) {
    klj_object *o = klj_as_object(self);
    return o && !strcmp(o->cls, PHOTON_AUDIO) ? o->data : NULL;
}
static void photon_destroy(void *data) {
    photon_audio *r = data;
    // Active workers own a global reference to self and cannot be recycled.
    // JNI calls payload destructors under g_lock, so release the retained array
    // here without recursively taking that registry lock.
    klj_object *buffer = klj_as_object(r->buffer);
    if (buffer && buffer->pinned) buffer->pinned--;
    pthread_mutex_destroy(&r->lock); pthread_cond_destroy(&r->done);
    free(r->held); free(r);
}
static klj_val photon_init(void *env, void *self, const klj_val *a, int n) {
    photon_audio *r = calloc(1, sizeof *r);
    if (!r) return (klj_val){0};
    pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&r->lock, &attr); pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&r->done, NULL);
    r->self = klj_own(klj_new_object_data(PHOTON_AUDIO, r), photon_destroy);
    return (klj_val){.l = r->self};
}
static klj_val photon_min_buffer(void *env, void *self, const klj_val *a, int n) {
    int rate = n >= 2 ? (int)a[0].j : 0, channels = n >= 2 ? (int)a[1].j : 0;
    if (rate < 8000 || rate > 192000 || (channels != 1 && channels != 2))
        return (klj_val){.j = (uint64_t)(int64_t)-2}; // Android ERROR_BAD_VALUE
    return (klj_val){.j = (uint64_t)((rate + 49) / 50) * channels * 2}; // 20 ms PCM16
}
static klj_val photon_rate(void *env, void *self, const klj_val *a, int n) {
    photon_audio *r = photon_state(self);
    if (!r) return (klj_val){0};
    pthread_mutex_lock(&r->lock); unsigned rate = r->rate; pthread_mutex_unlock(&r->lock);
    return (klj_val){.j = rate};
}
static klj_val photon_set_buffer(void *env, void *self, const klj_val *a, int n) {
    photon_audio *r = photon_state(self); klj_array *array = n ? klj_arr(a[0].l) : NULL;
    if (!r || !array || array->kind != 'S' || array->len <= 0 || array->len > 262144) return (klj_val){0};
    kl_jni_pin_object(a[0].l);
    pthread_mutex_lock(&r->lock); void *old = r->buffer; r->buffer = a[0].l; pthread_mutex_unlock(&r->lock);
    kl_jni_unpin_object(old);
    return (klj_val){.j = 1};
}
static int photon_samples(photon_audio *r, int16_t *dst, int frames) {
    double ratio = (double)r->source_rate / r->rate;
    size_t need = (size_t)(frames * ratio) + 2;
    if (need > r->capacity) {
        int16_t *p = realloc(r->held, need * r->source_channels * sizeof *p);
        if (!p) return 0;
        r->held = p; r->capacity = need;
    }
    if (r->have < need) {
        int got = kl_audio_mic_read_i16(r->held + r->have * r->source_channels, (int)(need - r->have));
        if (got > 0) r->have += (size_t)got;
    }
    int out = 0;
    while (out < frames && (size_t)r->position + 1 < r->have) {
        size_t i = (size_t)r->position; float frac = (float)(r->position - i);
        for (unsigned c = 0; c < r->channels; c++) {
            float sample = 0;
            unsigned first = r->channels == 1 ? 0 : (c < r->source_channels ? c : 0);
            unsigned last = r->channels == 1 ? r->source_channels : first + 1;
            for (unsigned sc = first; sc < last; sc++) {
                float x = r->held[i * r->source_channels + sc], y = r->held[(i + 1) * r->source_channels + sc];
                sample += x + (y - x) * frac;
            }
            dst[(size_t)out * r->channels + c] = (int16_t)lrintf(sample / (last - first));
        }
        out++; r->position += ratio;
    }
    size_t consumed = (size_t)r->position;
    if (consumed > r->have) consumed = r->have;
    if (consumed) {
        r->have -= consumed;
        memmove(r->held, r->held + consumed * r->source_channels, r->have * r->source_channels * sizeof(int16_t));
        r->position -= consumed;
    }
    return out;
}
static void *photon_worker(void *arg) {
    photon_audio *r = arg; kl_thread_init();
    size_t filled = 0; void *current_buffer = NULL;
    unsigned callbacks = 0;
    while (!atomic_load(&r->stop) && kl_audio_mic_enabled()) {
        pthread_mutex_lock(&r->lock);
        klj_array *array = klj_arr(r->buffer);
        if (current_buffer != r->buffer) { current_buffer = r->buffer; filled = 0; }
        if (array && array->len % r->channels == 0) {
            int frames = array->len / r->channels;
            filled += photon_samples(r, (int16_t *)array->data + filled * r->channels, frames - (int)filled);
            if (filled == (size_t)frames && !atomic_load(&r->stop)) {
                klj_proxy_invoke(r->callback, PHOTON_CALLBACK, "OnData", "()V", NULL);
                filled = 0; callbacks++;
                if (callbacks == 1) KLJ_LOG("Photon voice first PCM callback delivered rate=%u channels=%u", r->rate, r->channels);
            }
        }
        pthread_mutex_unlock(&r->lock);
        usleep(2000); // No polling or guest callbacks on CoreAudio's render thread.
    }
    // Release ownership before announcing stop so a replacement can open capture.
    pthread_mutex_lock(&capture_owner_lock);
    if (capture_owner == r) { kl_audio_mic_close(); capture_owner = NULL; }
    pthread_mutex_unlock(&capture_owner_lock);
    klj_proxy_invoke(r->callback, PHOTON_CALLBACK, "OnStop", "()V", NULL);
    kl_jni_unpin_object(r->callback);
    pthread_mutex_lock(&r->lock); r->callback = NULL; r->running = 0;
    pthread_cond_broadcast(&r->done); pthread_mutex_unlock(&r->lock);
    kl_jni_unpin_object(r->self); // Last use: Stop may now release the recorder.
    return NULL;
}
static klj_val photon_start(void *env, void *self, const klj_val *a, int n) {
    photon_audio *r = photon_state(self);
    klj_object *callback = n == 8 ? klj_as_object(a[1].l) : NULL;
    unsigned rate = n == 8 ? (unsigned)a[2].j : 0, channels = n == 8 ? (unsigned)a[3].j : 0;
    int bytes = n == 8 ? (int)a[4].j : 0;
    if (!r || !callback || strcmp(callback->cls, KLJ_CLASS_PROXY) ||
        (rate && (rate < 8000 || rate > 192000)) || (channels != 1 && channels != 2) ||
        bytes <= 0 || bytes > 1048576 || !kl_audio_mic_enabled()) return (klj_val){0};
    pthread_mutex_lock(&capture_owner_lock); pthread_mutex_lock(&r->lock);
    if (r->running || capture_owner || kl_audio_mic_open(rate, channels)) {
        pthread_mutex_unlock(&r->lock); pthread_mutex_unlock(&capture_owner_lock); return (klj_val){0};
    }
    r->source_rate = kl_audio_mic_rate(); r->source_channels = kl_audio_mic_channels();
    if (!r->source_rate || !r->source_channels || r->source_channels > 2) {
        kl_audio_mic_close(); pthread_mutex_unlock(&r->lock); pthread_mutex_unlock(&capture_owner_lock); return (klj_val){0};
    }
    r->rate = rate ? rate : r->source_rate; r->channels = channels; r->callback = a[1].l;
    r->have = 0; r->position = 0; atomic_store(&r->stop, 0); r->running = 1; capture_owner = r;
    kl_jni_pin_object(r->self); kl_jni_pin_object(r->callback);
    int result = pthread_create(&r->thread, NULL, photon_worker, r);
    if (result) {
        kl_jni_unpin_object(r->self); kl_jni_unpin_object(r->callback);
        r->callback = NULL; r->running = 0; capture_owner = NULL; kl_audio_mic_close();
    } else pthread_detach(r->thread);
    pthread_mutex_unlock(&r->lock); pthread_mutex_unlock(&capture_owner_lock);
    KLJ_LOG("Photon voice capture start result=%d rate=%u channels=%u", result, r->rate, r->channels);
    return (klj_val){.j = result == 0};
}
static klj_val photon_stop(void *env, void *self, const klj_val *a, int n) {
    photon_audio *r = photon_state(self); if (!r) return (klj_val){0};
    atomic_store(&r->stop, 1);
    pthread_mutex_lock(&r->lock);
    while (r->running && !pthread_equal(pthread_self(), r->thread)) pthread_cond_wait(&r->done, &r->lock);
    pthread_mutex_unlock(&r->lock);
    return (klj_val){.j = 1};
}
static klj_val photon_no_effect(void *env, void *self, const klj_val *a, int n) { return (klj_val){0}; }
const klj_binding klj_bind_photon[] = {
    {PHOTON_AUDIO, "<init>", "()V", photon_init},
    {PHOTON_AUDIO, "GetMinBufferSize", "(II)I", photon_min_buffer},
    {PHOTON_AUDIO, "GetSampleRate", "()I", photon_rate},
    {PHOTON_AUDIO, "SetBuffer", "([S)Z", photon_set_buffer},
    {PHOTON_AUDIO, "Start", PHOTON_START_SIG, photon_start},
    {PHOTON_AUDIO, "Stop", "()Z", photon_stop},
    // The supplied Reset body is empty. Android effect objects aren't exposed;
    // native capture may independently use its own VoiceProcessingIO path.
    {PHOTON_AUDIO, "Reset", "()V", photon_no_effect},
    {PHOTON_AUDIO, "AECIsAvailable", "()Z", photon_no_effect},
    {PHOTON_AUDIO, "AGCIsAvailable", "()Z", photon_no_effect},
    {PHOTON_AUDIO, "NSIsAvailable", "()Z", photon_no_effect},
    {NULL, NULL, NULL, NULL}
};
