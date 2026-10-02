#include <assert.h>
#include <stdio.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"

#define JNI_SLOT_ENUM(n, name) SLOT_##name = n,
enum { KL_JNI_SLOTS(JNI_SLOT_ENUM) };

int main(void) {
    void *env = kl_jni_env();
    void **table = *(void ***)env;
    void *(*method)(void *, void *, const char *, const char *) = table[SLOT_GetMethodID];
    void *(*call)(void *, void *, void *, const void *) = table[SLOT_CallObjectMethodA];
    void *(*getclass)(void *, void *) = table[SLOT_GetObjectClass];
    void *activity = kl_jni_activity();
    void *cls = getclass(env, activity);
    const char *signatures[] = {"()Landroid/content/ContentResolver;", "()Ljava/lang/Object;"};
    void *first = NULL;
    for (int i = 0; i < 8; i++) {
        kl_jni_local_frame_push();
        void *mid = method(env, cls, "getContentResolver", signatures[i % 2]);
        assert(mid);
        void *resolver = call(env, activity, mid, NULL);
        assert(resolver);
        assert(getclass(env, resolver) == kl_jni_class("android/content/ContentResolver"));
        if (!first) first = resolver;
        assert(resolver == first);
        kl_jni_local_frame_pop();
    }
    puts("Cached ContentResolver identity and class survive JNI local-frame retirement");
}
