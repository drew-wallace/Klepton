// UE4 bitmap-video pixels, GL state, archive data source, and offline results.
// Optional arguments: OBB directory, virtual movie path, movie byte length.
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_glfb.h"
#include "kl_avdec.h"
#include "kl_obbmap.h"
#include "kl_ovrplat.h"
#include "kl_jni.h"
#include "kl_jni_slots.h"

#define JNI_SLOT_ENUM(n, name) SLOT_##name = n,
enum { KL_JNI_SLOTS(JNI_SLOT_ENUM) };
typedef union { int32_t i; int64_t j; void *l; } argument;

int main(int argc, char **argv) {
    const struct { const char *name; uint32_t type; } requests[] = {
        {"ovr_Leaderboard_GetEntries", 0x5DB3474C},
        {"ovr_Leaderboard_GetEntriesAfterRank", 0x18378BEF},
        {"ovr_Leaderboard_GetEntriesByIds", 0x39607BFC},
        {"ovr_Leaderboard_GetNextEntries", 0x4E207CD9},
        {"ovr_Leaderboard_GetPreviousEntries", 0x4901DAC0},
        {"ovr_Leaderboard_WriteEntry", 0x117FC8FE},
    };
    void *(*pop)(void) = kl_ovrplat_sym("ovr_PopMessage");
    uint64_t (*idof)(void *) = kl_ovrplat_sym("ovr_Message_GetRequestID");
    uint64_t (*typeofmsg)(void *) = kl_ovrplat_sym("ovr_Message_GetType");
    uint64_t (*iserror)(void *) = kl_ovrplat_sym("ovr_Message_IsError");
    void *(*geterror)(void *) = kl_ovrplat_sym("ovr_Message_GetError");
    const char *(*reason)(void *) = kl_ovrplat_sym("ovr_Error_GetMessage");
    int32_t (*code)(void *) = kl_ovrplat_sym("ovr_Error_GetCode");
    int32_t (*http)(void *) = kl_ovrplat_sym("ovr_Error_GetHttpCode");
    void (*release)(void *) = kl_ovrplat_sym("ovr_FreeMessage");
    assert(pop && idof && typeofmsg && iserror && geterror && reason && code && http && release);
    for (unsigned round = 0; round < 4; round++) {
        uint64_t ids[6];
        for (unsigned i = 0; i < 6; i++) {
            uint64_t (*request)(const char *, uint64_t, uint64_t, uint64_t) = kl_ovrplat_sym(requests[i].name);
            assert(request && (ids[i] = request("mission", 10, 0, 0)) != 0);
            if (i) assert(ids[i] != ids[i-1]);
        }
        for (unsigned i = 0; i < 6; i++) {
            void *m = pop();
            assert(m && idof(m) == ids[i] && typeofmsg(m) == requests[i].type && iserror(m));
            void *error = geterror(m);
            assert(error && strstr(reason(error), "unavailable") && code(error) == -1 && http(error) == 0);
            release(m);
        }
        assert(pop() == NULL);
        // Ring reuse must clear error state on ordinary completions.
        uint64_t (*version)(void) = kl_ovrplat_sym("ovr_Application_GetVersion");
        uint64_t id = version();
        void *m = pop();
        assert(m && idof(m) == id && !iserror(m) && !geterror(m));
        release(m);
    }
    uint64_t (*request)(const char *, uint64_t, uint64_t, uint64_t) = kl_ovrplat_sym(requests[0].name);
    uint64_t held_id = request("mission", 10, 0, 0);
    void *held = pop();
    assert(held);
    for (unsigned i = 0; i < 32; i++) {
        uint64_t next = request("mission", 10, 0, 0);
        void *m = pop();
        assert(m && idof(m) == next && idof(held) == held_id);
        release(m);
    }
    release(held);
    for (unsigned i = 0; i < 16; i++) assert(request("mission", 10, 0, 0));
    assert(request("mission", 10, 0, 0) == 0);
    for (unsigned i = 0; i < 16; i++) { void *m = pop(); assert(m); release(m); }
    assert(pop() == NULL);
    puts("PASS: leaderboard error callbacks, message lifetime, queue reuse and exhaustion");

    setenv("KL_GLFB", "1", 1);
    setenv("KL_GLFB_SIZE", "64x64", 1);
    assert(kl_glfb_init());
    kl_glfb_make_current();
    const char *dir = getenv("KL_ANGLE_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/libGLESv2.dylib", dir ? dir : "vendor/out/Debug");
    void *lib = dlopen(path, RTLD_NOW);
    assert(lib);
#define GL(ret, name, args) ret (*name) args = dlsym(lib, #name); assert(name)
    GL(void, glGenTextures, (int, uint32_t *));
    GL(void, glBindTexture, (uint32_t, uint32_t));
    GL(void, glTexStorage2D, (uint32_t, int, uint32_t, int, int));
    GL(void, glGenBuffers, (int, uint32_t *));
    GL(void, glBindBuffer, (uint32_t, uint32_t));
    GL(void, glPixelStorei, (uint32_t, int));
    GL(void, glGetIntegerv, (uint32_t, int *));
    GL(uint32_t, glGetError, (void));
    GL(void, glGenFramebuffers, (int, uint32_t *));
    GL(void, glBindFramebuffer, (uint32_t, uint32_t));
    GL(void, glFramebufferTexture2D, (uint32_t, uint32_t, uint32_t, uint32_t, int));
    GL(uint32_t, glCheckFramebufferStatus, (uint32_t));
    GL(void, glReadPixels, (int, int, int, int, uint32_t, uint32_t, void *));
    uint32_t tex[2], pbo, fbo;
    glGenTextures(2, tex);
    glBindTexture(0x0DE1, tex[0]);
    glTexStorage2D(0x0DE1, 1, 0x8058, 2, 2);
    glBindTexture(0x0DE1, tex[1]);
    glGenBuffers(1, &pbo);
    glBindBuffer(0x88EC, pbo);
    const uint32_t names[] = {0x0CF5, 0x0CF2, 0x0CF3, 0x0CF4, 0x806E, 0x806D};
    const int values[] = {8, 7, 2, 1, 9, 3};
    for (unsigned i = 0; i < 6; i++) glPixelStorei(names[i], values[i]);
    const uint8_t bgra[] = {0,0,255,255, 0,255,0,255, 255,0,0,255, 3,2,1,255};
    while (glGetError()) {}
    assert(kl_glfb_upload_video_bgra(tex[0], 2, 2, bgra));
    assert(glGetError() == 0);
    int value;
    glGetIntegerv(0x8069, &value); assert((uint32_t)value == tex[1]);
    glGetIntegerv(0x88EF, &value); assert((uint32_t)value == pbo);
    for (unsigned i = 0; i < 6; i++) { glGetIntegerv(names[i], &value); assert(value == values[i]); }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(0x8D40, fbo);
    glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex[0], 0);
    assert(glCheckFramebufferStatus(0x8D40) == 0x8CD5);
    uint8_t rgba[16];
    glReadPixels(0, 0, 2, 2, 0x1908, 0x1401, rgba);
    const uint8_t expected[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 1,2,3,255};
    assert(glGetError() == 0 && memcmp(rgba, expected, sizeof rgba) == 0);
    puts("PASS: GPU receives exact BGRA video colors and upload preserves texture/PBO/unpack state");

    if (argc == 4) {
        char root[] = "/tmp/klepton-media-test-XXXXXX";
        assert(mkdtemp(root));
        kl_guest_extstorage_map(root);
        char base[1024];
        snprintf(base, sizeof base, "%s/UE4Game/RoboRecall", root);
        kl_obbmap_init(argv[1], base);
        kl_avdec *dec = kl_avdec_open(argv[2], 0, atoll(argv[3]));
        assert(dec && kl_avdec_width(dec) > 0 && kl_avdec_height(dec) > 0);
        kl_avdec_play(dec, 1);
        unsigned long long serial = 0;
        const uint8_t *pixels = NULL;
        int w, h; size_t bytes;
        for (int i = 0; i < 100 && !pixels; i++) {
            usleep(50000);
            pixels = kl_avdec_frame(dec, &w, &h, &bytes, &serial);
        }
        assert(pixels && serial && bytes == (size_t)w*h*4);
        printf("PASS: virtual archive movie decodes %dx%d, %lld ms, frame %llu\n",
               w, h, kl_avdec_duration_ms(dec), serial);
        kl_avdec_close(dec);

        // Exercise the same JNI constructor/data-source/texture call as UE4.
        void *env = kl_jni_env();
        void **table = *(void ***)env;
        void *(*method)(void *, void *, const char *, const char *) = table[SLOT_GetMethodID];
        void *(*create)(void *, void *, void *, const void *) = table[SLOT_NewObjectA];
        uint8_t (*boolean)(void *, void *, void *, const void *) = table[SLOT_CallBooleanMethodA];
        void (*callvoid)(void *, void *, void *, const void *) = table[SLOT_CallVoidMethodA];
        void *cls = kl_jni_class("com/epicgames/ue4/MediaPlayer14");
        argument ctor[] = {{.i=0}, {.i=0}};
        void *player = create(env, cls, method(env, cls, "<init>", "(ZZ)V"), ctor);
        assert(player);
        argument source[] = {{.l=kl_jni_new_string(argv[2])}, {.j=0}, {.j=atoll(argv[3])}};
        assert(boolean(env, player, method(env, cls, "setDataSource", "(Ljava/lang/String;JJ)Z"), source));
        void *(*callobject)(void *, void *, void *, const void *) = table[SLOT_CallObjectMethodA];
        int32_t (*length)(void *, void *) = table[SLOT_GetArrayLength];
        void *(*element)(void *, void *, int32_t) = table[SLOT_GetObjectArrayElement];
        void *(*field)(void *, void *, const char *, const char *) = table[SLOT_GetFieldID];
        int32_t (*getint)(void *, void *, void *) = table[SLOT_GetIntField];
        float (*getfloat)(void *, void *, void *) = table[SLOT_GetFloatField];
        void *tracks = callobject(env, player, method(env, cls, "GetVideoTracks",
             "()[Lcom/epicgames/ue4/MediaPlayer14$VideoTrackInfo;"), NULL);
        assert(tracks && length(env, tracks) == 1);
        void *track = element(env, tracks, 0);
        void *trackcls = kl_jni_class("com/epicgames/ue4/MediaPlayer14$VideoTrackInfo");
        assert(getint(env, track, field(env, trackcls, "Width", "I")) == w);
        assert(getint(env, track, field(env, trackcls, "Height", "I")) == h);
        assert(getfloat(env, track, field(env, trackcls, "FrameRate", "F")) > 0);
        puts("PASS: JNI exposes the decoded video track and measured dimensions/frame rate");
        callvoid(env, player, method(env, cls, "start", "()V"), NULL);
        glBindBuffer(0x88EC, 0);
        for (unsigned i = 0; i < 6; i++) glPixelStorei(names[i], i ? 0 : 1);
        uint32_t movie;
        glGenTextures(1, &movie);
        glBindTexture(0x0DE1, movie);
        glTexStorage2D(0x0DE1, 1, 0x8058, w, h);
        argument texture = {.i=(int32_t)movie};
        void *frame = method(env, cls, "getVideoLastFrame", "(I)Z");
        int ready = 0;
        for (int i = 0; i < 100 && !ready; i++) {
            usleep(50000);
            ready = boolean(env, player, frame, &texture);
        }
        assert(ready && glGetError() == 0);
        glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, movie, 0);
        uint8_t *picture = malloc((size_t)w*h*4);
        assert(picture && glCheckFramebufferStatus(0x8D40) == 0x8CD5);
        glReadPixels(0, 0, w, h, 0x1908, 0x1401, picture);
        // At least one decoded RGB pixel is present; do not accept blank alpha.
        int content = 0;
        for (size_t i = 0; i < (size_t)w*h*4; i += 4)
            if (picture[i] || picture[i+1] || picture[i+2]) { content = 1; break; }
        assert(content && glGetError() == 0);
        free(picture);
        callvoid(env, player, method(env, cls, "release", "()V"), NULL);
        puts("PASS: UE4 JNI data source and getVideoLastFrame put archive video into a GPU texture");
    }
    return 0;
}
