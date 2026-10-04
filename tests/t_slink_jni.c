// Steam Link 2.0.23 JNI startup and IME bridge, without an APK or Steam host.
#include <assert.h>
#include <string.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "kl_mono.h"

#define JNI_SLOT_ENUM(n, name) SLOT_##name = n,
enum { KL_JNI_SLOTS(JNI_SLOT_ENUM) };
typedef union { int32_t i; void *l; } argument;
static int commits, cursor, key_down, key_up;
static char text[128];

static void commit(void *env, void *cls, void *str, int32_t position) {
    (void)cls;
    void **table = *(void ***)env;
    const char *(*get)(void *, void *, void *) = table[SLOT_GetStringUTFChars];
    void (*release)(void *, void *, const char *) = table[SLOT_ReleaseStringUTFChars];
    const char *utf8 = get(env, str, NULL);
    assert(utf8);
    snprintf(text, sizeof text, "%s", utf8);
    release(env, str, utf8);
    commits++;
    cursor = position;
}
static void down(void *env, void *cls, int32_t code) { key_down = code; }
static void up(void *env, void *cls, int32_t code) { key_up = code; }
static void mouse(void *env, void *cls, int32_t state, int32_t action,
                  float x, float y, uint8_t relative) {}

int main(void) {
    void *env = kl_jni_env();
    void **table = *(void ***)env;
    void *(*method)(void *, void *, const char *, const char *) = table[SLOT_GetStaticMethodID];
    void (*call_void)(void *, void *, void *, const void *) = table[SLOT_CallStaticVoidMethodA];
    uint8_t (*call_bool)(void *, void *, void *, const void *) = table[SLOT_CallStaticBooleanMethodA];
    int32_t (*reg)(void *, void *, const kl_jni_method *, int32_t) = table[SLOT_RegisterNatives];
    void *sdl = kl_jni_class("org/libsdl/app/SDLActivity");
    argument enabled[] = {{.i = 1}};
    void *trap = method(env, sdl, "setBackButtonTrapEnabled", "(Z)V");
    call_void(env, sdl, trap, enabled);
    enabled[0].i = 0;
    call_void(env, sdl, trap, enabled);
    void *steam = kl_jni_class("com/valvesoftware/steamlink/SteamLink");
    void *(*instance_method)(void *, void *, const char *, const char *) = table[SLOT_GetMethodID];
    void (*instance_void)(void *, void *, void *, const void *) = table[SLOT_CallVoidMethodA];
    kl_jni_set_activity_class("com/valvesoftware/steamlink/SteamLink");
    instance_void(env, kl_jni_activity(),
                  instance_method(env, steam, "trySetupUsbNetwork", "()V"), NULL);

    const kl_jni_method keyboard[] = {
        {"onNativeKeyDown", "(I)V", down},
        {"onNativeKeyUp", "(I)V", up},
        {"onNativeMouse", "(IIFFZ)V", mouse},
    };
    assert(reg(env, sdl, keyboard, 3) == 0);
    const kl_jni_method ime[] = {{"nativeCommitText", "(Ljava/lang/String;I)V", commit}};
    assert(reg(env, kl_jni_class("org/libsdl/app/SDLInputConnection"), ime, 1) == 0);
    argument rectangle[] = {{.i=10}, {.i=20}, {.i=100}, {.i=30}, {.i=1}};
    const char *signatures[] = {"(IIII)Z", "(IIIII)Z"};
    for (int i = 0; i < 2; i++) {
        kl_mono_set_text_input(0);
        assert(call_bool(env, sdl, method(env, sdl, "showTextInput", signatures[i]), rectangle));
        assert(kl_mono_text_input_wanted());
        kl_mono_set_text_input(0);
        kl_mono_commit_text("192.168.1.50 é");
        assert(commits == i + 1 && cursor == 1);
        assert(strcmp(text, "192.168.1.50 é") == 0);
    }
    int backspace = kl_mono_keycode_for_char(8);
    kl_mono_key(1, backspace);
    kl_mono_key(0, backspace);
    assert(backspace == 67 && key_down == backspace && key_up == backspace);
    puts("Steam Link startup, both SDL IME signatures, UTF-8 text and backspace passed");
}
