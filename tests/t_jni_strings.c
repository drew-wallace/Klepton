// Unity unboxes JavaScript callback results through String.toString().
#include <assert.h>
#include <stdio.h>
#include <string.h>
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
    const char *(*chars)(void *, void *, unsigned char *) = table[SLOT_GetStringUTFChars];
    void (*release)(void *, void *, const char *) = table[SLOT_ReleaseStringUTFChars];
    const char *values[] = {"", "browser result", "{\"ready\":true}", "Unicode \xe2\x9c\x93"};
    for (unsigned i=0; i<sizeof values/sizeof values[0]; ++i) {
        kl_jni_local_frame_push();
        void *string = kl_jni_new_string(values[i]);
        void *cls = getclass(env, string);
        assert(cls == kl_jni_class("java/lang/String"));
        const char *signatures[] = {"()Ljava/lang/String;", ""};
        for (unsigned j=0; j<sizeof signatures/sizeof signatures[0]; ++j) {
            void *mid = method(env, cls, "toString", signatures[j]);
            assert(mid);
            void *result = call(env, string, mid, NULL);
            assert(result == string);
            const char *text = chars(env, result, NULL);
            assert(text && !strcmp(text, values[i]));
            release(env, result, text);
        }
        kl_jni_local_frame_pop();
    }
    puts("JNI String.toString preserves identity and contents through typed and name-only calls");
}
