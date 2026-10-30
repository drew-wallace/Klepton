// Exercise both APK callback generations without proprietary guest fixtures.
#include <assert.h>
#include <sys/wait.h>
// Replace only the plugin lookup in the family implementation. The rest of
// the JNI runtime and its real object model are linked normally.
#define kl_find_image fixture_find_image
#define kl_sym fixture_sym
#include "../runtime/jni/kl_jni_services.c"
#undef kl_find_image
#undef kl_sym

static int legacy, completed, searched;
static void surface_ready(void *env, void *self, void *surface) {
    assert(env == kl_jni_env());
    assert(self && surface);
    assert(strcmp(klj_as_object(surface)->cls, "android/view/Surface") == 0);
    completed++;
}
kl_image *fixture_find_image(const char *name) {
    assert(strcmp(name, "libOculusXRPlugin.so") == 0);
    return (kl_image *)(uintptr_t)1;
}
void *fixture_sym(kl_image *image, const char *name) {
    assert(image);
    searched++;
    if (!strcmp(name, "Java_com_unity_oculus_OculusUnity_surfaceCreated"))
        return legacy ? NULL : surface_ready;
    assert(!strcmp(name, "Java_com_unity_oculus_OculusUnity_initComplete"));
    return surface_ready;
}
int main(void) {
    for (int variant = 0; variant < 2; variant++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            legacy = variant;
            void *env = kl_jni_env();
            void *self = kl_jni_new_object("com/unity/oculus/OculusUnity");
            klj_OculusUnity_initOculus(env, self, NULL, 0);
            assert(completed == 1 && searched == (legacy ? 2 : 1));
            // Repeated initialization uses the resolved callback.
            klj_OculusUnity_initOculus(env, self, NULL, 0);
            assert(completed == 2 && searched == (legacy ? 2 : 1));
            _exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Oculus surfaceCreated and legacy initComplete receive the JNI Surface");
}
