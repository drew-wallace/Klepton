// `make vidtex` — the video TEXTURE gate: a decoded frame all the way to
// sampled pixels, on both storages.
//
// `make hevc` proves frames come out of the decoder; this proves the guest
// could SEE them. The native path has four links the decoder gate cannot
// exercise — the CVMetalTextureCache wrap (kl_cvmtl.m), ANGLE accepting the
// private YCbCr pixel format as an EGLImage sibling (the vendored ANGLE's
// format-table patch), the glEGLImageTargetTexture2DOES retarget, and the
// sampler's own YUV->RGB conversion — and a regression in any of them is a
// black or garbage video pane in a live Steam Link session, which costs a
// fresh pairing to even look at. Here it is one draw.
//
// The assertion of record: the SAME picture decoded twice, once native
// (biplanar YCbCr, sampler converts) and once as BGRA (KL_VTDEC_BGRA=1,
// VideoToolbox converts), drawn with the same shader, reads back nearly equal.
// Neither path is the reference for the other in principle, but they only
// agree if the sampler's matrix, range and plane addressing are all right —
// a wrong range reads as a global contrast shift, a swapped chroma plane as
// hue inversion, a wrong stride as diagonal tearing; all far outside the
// tolerance here, which only forgives the two paths' different chroma
// upsampling filters at the test card's hard edges.
//
// Needs the PATCHED ANGLE (`make angle-debug`) and is not part of
// `make check` — a bare checkout has no vendor/ to load.
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreVideo/CoreVideo.h>

#include "../runtime/media/kl_vtdec.h"
#include "../runtime/gfx/kl_glfb.h"
#ifdef KL_TEST_VIDEO_LAYER_MIRROR
#include "t_mtl_provider.h"
#endif

// Two streams, one per YCbCr row in klfb_video_mtl_format: 8-bit (private
// format 500) and Main10 (packed, 508 — the row a live Steam Link session
// actually takes, which the 8-bit stream alone failed to prove once). Both
// ffmpeg-generated testsrc2 like the `make hevc` stream; the 10-bit one adds
// `-profile:v main10 -pix_fmt p010le`.
#define STREAM8  "tests/data/t_hevc.h265"
#define STREAM10 "tests/data/t_hevc10.h265"
#define W 320
#define H 240

static int fails;
static void check(int ok, const char *what) {
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// ---- the slice of GL this test speaks ----
#define GL_TEXTURE_2D_          0x0DE1
#define GL_RGBA_                0x1908
#define GL_UNSIGNED_BYTE_       0x1401
#define GL_COLOR_ATTACHMENT0_   0x8CE0
#define GL_FRAMEBUFFER_         0x8D40
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_TRIANGLES_           0x0004
#define GL_VERTEX_SHADER_       0x8B31
#define GL_FRAGMENT_SHADER_     0x8B30
#define GL_COMPILE_STATUS_      0x8B81
#define GL_LINK_STATUS_         0x8B82

static void (*p_glGenTextures)(int32_t, uint32_t *);
static void (*p_glBindTexture)(uint32_t, uint32_t);
static void (*p_glTexImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                              int32_t, uint32_t, uint32_t, const void *);
static void (*p_glGenFramebuffers)(int32_t, uint32_t *);
static void (*p_glBindFramebuffer)(uint32_t, uint32_t);
static void (*p_glFramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static uint32_t (*p_glCheckFramebufferStatus)(uint32_t);
static void (*p_glViewport)(int32_t, int32_t, int32_t, int32_t);
static void (*p_glDrawArrays)(uint32_t, int32_t, int32_t);
static void (*p_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                              uint32_t, void *);
static uint32_t (*p_glCreateShader)(uint32_t);
static void (*p_glShaderSource)(uint32_t, int32_t, const char **, const int32_t *);
static void (*p_glCompileShader)(uint32_t);
static void (*p_glGetShaderiv)(uint32_t, uint32_t, int32_t *);
static uint32_t (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(uint32_t, uint32_t);
static void (*p_glLinkProgram)(uint32_t);
static void (*p_glGetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*p_glUseProgram)(uint32_t);
static void (*p_glFinish)(void);
static uint32_t (*p_glGetError)(void);

static int gl_resolve(void) {
#define R(n) (*(void **)&p_##n = kl_glfb_sym(#n))
    return R(glGenTextures) && R(glBindTexture) && R(glTexImage2D) &&
           R(glGenFramebuffers) && R(glBindFramebuffer) &&
           R(glFramebufferTexture2D) && R(glCheckFramebufferStatus) &&
           R(glViewport) && R(glDrawArrays) && R(glReadPixels) &&
           R(glCreateShader) && R(glShaderSource) && R(glCompileShader) &&
           R(glGetShaderiv) && R(glCreateProgram) && R(glAttachShader) &&
           R(glLinkProgram) && R(glGetProgramiv) && R(glUseProgram) &&
           R(glFinish) && R(glGetError);
#undef R
}

static uint32_t make_program(void) {
    // gl_VertexID's fullscreen triangle: no buffers, no attributes, and the
    // sampler at its default unit 0 — the least GL that can sample a texture.
    static const char *vs_src =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D t;\n"
        "in vec2 uv; out vec4 o;\n"
        "void main(){ o = texture(t, uv); }\n";
    int32_t ok = 0;
    uint32_t vs = p_glCreateShader(GL_VERTEX_SHADER_);
    p_glShaderSource(vs, 1, &vs_src, NULL);
    p_glCompileShader(vs);
    p_glGetShaderiv(vs, GL_COMPILE_STATUS_, &ok);
    if (!ok) return 0;
    uint32_t fs = p_glCreateShader(GL_FRAGMENT_SHADER_);
    p_glShaderSource(fs, 1, &fs_src, NULL);
    p_glCompileShader(fs);
    p_glGetShaderiv(fs, GL_COMPILE_STATUS_, &ok);
    if (!ok) return 0;
    uint32_t prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    p_glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    return ok ? prog : 0;
}

// Decode the stream's first frame under whatever KL_VTDEC_* is in force.
static int is_vcl(const unsigned char *nal) { return ((nal[0] >> 1) & 0x3f) <= 31; }
static CVPixelBufferRef first_frame(const unsigned char *buf, long n) {
    kl_vtdec *d = kl_vtdec_create("video/hevc");
    if (!d) return NULL;
    CVPixelBufferRef out = NULL;
    long i = 0, prev = 0;
    int seen_vcl = 0;
    while (i + 3 < n && !out) {
        if (!(buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1)) { i++; continue; }
        long payload = i + 3;
        long sc = (i > 0 && buf[i-1] == 0) ? i - 1 : i;
        if (seen_vcl) {
            kl_vtdec_submit(d, buf + prev, (size_t)(sc - prev), 0);
            prev = sc;
            seen_vcl = 0;
            for (int spin = 0; spin < 400 && !(out = kl_vtdec_pull(d, NULL)); spin++)
                usleep(500);
        }
        if (payload < n && is_vcl(buf + payload)) seen_vcl = 1;
        i = payload;
    }
    kl_vtdec_destroy(d);        // `out` is the caller's reference, unaffected
    return out;
}

// Frame -> image -> texture -> draw -> readback. Returns 0 and says why not.
static int draw_frame(CVPixelBufferRef pb, uint32_t prog, unsigned char *rgba,
                      const char *label) {
    int w = 0, h = 0;
    void *img = kl_glfb_image_from_pixels(pb, &w, &h);
    if (!img) { printf("  %s: no image from the frame\n", label); return 0; }
    if (w != W || h != H) { printf("  %s: image is %dx%d\n", label, w, h); return 0; }

    uint32_t tex = 0, fb_tex = 0, fb = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_2D_, tex);
    if (!kl_glfb_image_bind(img)) {
        printf("  %s: kl_glfb_image_bind refused\n", label);
        return 0;
    }

    p_glGenTextures(1, &fb_tex);
    p_glBindTexture(GL_TEXTURE_2D_, fb_tex);
    p_glTexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA_, W, H, 0, GL_RGBA_,
                   GL_UNSIGNED_BYTE_, NULL);
    p_glGenFramebuffers(1, &fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER_, fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                             GL_TEXTURE_2D_, fb_tex, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
        printf("  %s: framebuffer incomplete\n", label);
        return 0;
    }

    p_glViewport(0, 0, W, H);
    p_glUseProgram(prog);
    p_glBindTexture(GL_TEXTURE_2D_, tex);   // unit 0, where the sampler looks
    while (p_glGetError()) {}
    p_glDrawArrays(GL_TRIANGLES_, 0, 3);
    uint32_t e = p_glGetError();
    if (e) { printf("  %s: draw -> GL error 0x%x\n", label, e); return 0; }
    p_glFinish();
    p_glReadPixels(0, 0, W, H, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);
    e = p_glGetError();
    if (e) { printf("  %s: readback -> GL error 0x%x\n", label, e); return 0; }
#ifdef KL_TEST_VIDEO_LAYER_MIRROR
    static int next_layer=40;
    int layer=next_layer++;
    void (*active)(uint32_t)=kl_glfb_sym("glActiveTexture");
    void (*geti)(uint32_t,int*)=kl_glfb_sym("glGetIntegerv");
    void (*getb)(uint32_t,uint8_t*)=kl_glfb_sym("glGetBooleanv");
    void (*enable)(uint32_t)=kl_glfb_sym("glEnable");
    void (*disable)(uint32_t)=kl_glfb_sym("glDisable");
    uint8_t (*enabled)(uint32_t)=kl_glfb_sym("glIsEnabled");
    void (*mask)(uint8_t,uint8_t,uint8_t,uint8_t)=kl_glfb_sym("glColorMask");
    void (*gensampler)(int,uint32_t*)=kl_glfb_sym("glGenSamplers");
    void (*bindsampler)(uint32_t,uint32_t)=kl_glfb_sym("glBindSampler");
    void (*samplerparam)(uint32_t,uint32_t,int)=kl_glfb_sym("glSamplerParameteri");
    void (*genvao)(int,uint32_t*)=kl_glfb_sym("glGenVertexArrays");
    void (*bindvao)(uint32_t)=kl_glfb_sym("glBindVertexArray");
    assert(active&&geti&&getb&&enable&&disable&&enabled&&mask&&gensampler&&bindsampler&&samplerparam&&genvao&&bindvao);
    uint32_t guard,sampler,vao;int oldvao=0,readfb=0,drawfb=0;
    geti(0x85b5,&oldvao);geti(0x8caa,&readfb);geti(0x8ca6,&drawfb);
    p_glGenTextures(1,&guard);active(0x84c3);p_glBindTexture(GL_TEXTURE_2D_,guard);
    gensampler(1,&sampler);bindsampler(0,sampler);samplerparam(sampler,0x2801,0x2703);
    genvao(1,&vao);bindvao(vao);
    const uint32_t caps[]={0xbe2,0xb71,0xb90,0xc11,0xb44,0x8c89,0x809e,0x80a0,0xbd0};
    uint8_t original[sizeof caps/sizeof *caps];
    for(unsigned i=0;i<sizeof caps/sizeof *caps;i++){original[i]=enabled(caps[i]);enable(caps[i]);}
    mask(0,1,0,1);p_glViewport(2,3,19,21);
    if(!kl_glfb_mirror_layer(layer,0,tex,-1,w,h,0x8c43)) {
        printf("  %s: video layer mirror refused the decoded source\n",label);
        kl_glfb_image_destroy(img);return 0;
    }
    int state=0,vp[4];uint8_t colour_mask[4];
    geti(0x84e0,&state);assert(state==0x84c3);
    geti(0x8069,&state);assert(state==(int)guard);
    geti(0x8b8d,&state);assert(state==(int)prog);
    geti(0x85b5,&state);assert(state==(int)vao);
    geti(0x8caa,&state);assert(state==readfb);geti(0x8ca6,&state);assert(state==drawfb);
    geti(0xba2,vp);assert(vp[0]==2&&vp[1]==3&&vp[2]==19&&vp[3]==21);
    getb(0xc23,colour_mask);assert(!colour_mask[0]&&colour_mask[1]&&!colour_mask[2]&&colour_mask[3]);
    for(unsigned i=0;i<sizeof caps/sizeof *caps;i++){assert(enabled(caps[i]));if(!original[i])disable(caps[i]);}
    active(0x84c0);geti(0x8069,&state);assert(state==(int)tex);
    geti(0x8919,&state);assert(state==(int)sampler);bindsampler(0,0);
    bindvao((uint32_t)oldvao);mask(1,1,1,1);p_glViewport(0,0,W,H);
    p_glFinish();
    unsigned char *mirrored=malloc(W*H*4);assert(mirrored);
    assert(kl_mtl_layer_rgba(layer,0,mirrored,W*H*4));
    unsigned long difference=0;
    for(int i=0;i<W*H*4;i++)difference+=(unsigned)abs((int)rgba[i]-(int)mirrored[i]);
    printf("  %s layer: mean pixel difference %.2f\n",label,(double)difference/(W*H*4));
    free(mirrored);
    if((double)difference/(W*H*4)>1.0) {
        printf("  %s: layer pixels differ from sampled source by %.2f\n",label,(double)difference/(W*H*4));
        kl_glfb_image_destroy(img);return 0;
    }
    void (*delete_program)(uint32_t)=kl_glfb_sym("glDeleteProgram");assert(delete_program);
    uint32_t doomed=make_program();assert(doomed);p_glUseProgram(doomed);delete_program(doomed);
    // A current, delete-pending guest program cannot survive an internal
    // glUseProgram switch. Copy must defer without destroying that program.
    assert(!kl_glfb_mirror_layer(layer+1000,0,tex,-1,w,h,0x8c43));
    geti(0x8b8d,&state);assert(state==(int)doomed);
    p_glGetProgramiv(doomed,0x8b80,&state);assert(state);
    p_glUseProgram(prog);
#endif
    kl_glfb_image_destroy(img);
    return 1;
}

static void run_stream(const char *path, const char *tag, uint32_t prog) {
    printf("  --- %s (%s) ---\n", tag, path);
    char what[128];
    FILE *f = fopen(path, "rb");
    snprintf(what, sizeof what, "%s: stream opens (run from the repo root)", tag);
    check(f != NULL, what);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return; }
    fclose(f);

    unsetenv("KL_VTDEC_BGRA");
    CVPixelBufferRef native = first_frame(buf, n);
    snprintf(what, sizeof what, "%s: a frame decodes in the native format", tag);
    check(native != NULL, what);
    unsigned pf = native ? CVPixelBufferGetPixelFormatType(native) : 0;
    snprintf(what, sizeof what, "%s: the native frame is biplanar", tag);
    check(native && CVPixelBufferGetPlaneCount(native) == 2, what);
    if (native)
        printf("  %s native format '%c%c%c%c'\n", tag, (char)(pf >> 24),
               (char)(pf >> 16), (char)(pf >> 8), (char)pf);

    setenv("KL_VTDEC_BGRA", "1", 1);
    CVPixelBufferRef bgra = first_frame(buf, n);
    unsetenv("KL_VTDEC_BGRA");
    snprintf(what, sizeof what, "%s: the same frame decodes as BGRA", tag);
    check(bgra != NULL, what);
    if (!native || !bgra) { free(buf); return; }

    size_t sz = (size_t)W * H * 4;
    unsigned char *px_native = malloc(sz), *px_bgra = malloc(sz);
    int drew = draw_frame(native, prog, px_native, tag);
    snprintf(what, sizeof what, "%s: native frame: image, bind, draw, readback", tag);
    check(drew, what);
    int drew_b = draw_frame(bgra, prog, px_bgra, tag);
    snprintf(what, sizeof what, "%s: BGRA frame: image, bind, draw, readback", tag);
    check(drew_b, what);

    if (drew && drew_b) {
        // The native readback carries a picture of its own...
        int varied = 0;
        for (size_t i = 4; i < sz && !varied; i += 64)
            if (px_native[i] != px_native[0]) varied = 1;
        snprintf(what, sizeof what, "%s: native readback is a picture, not a flat fill", tag);
        check(varied, what);
        int opaque = 1;
        for (size_t i = 3; i < sz; i += 4)
            if (px_native[i] != 255) { opaque = 0; break; }
        snprintf(what, sizeof what, "%s: native readback is opaque (YCbCr alpha answers 1)", tag);
        check(opaque, what);

        // ...and it is the SAME picture the BGRA path produces. Mean abs
        // difference over RGB: the two chroma upsamplers disagree only at the
        // test card's hard edges, and a matrix/range/plane mistake is tens of
        // levels everywhere.
        unsigned long long diff = 0;
        unsigned long count = 0;
        int worst = 0;
        for (size_t i = 0; i < sz; i += 4) {
            for (int c = 0; c < 3; c++) {
                int a = px_native[i + c], b = px_bgra[i + c];
                int d = a > b ? a - b : b - a;
                diff += (unsigned)d;
                if (d > worst) worst = d;
                count++;
            }
        }
        double mean = (double)diff / (double)count;
        printf("  %s native vs BGRA: mean abs diff %.2f, worst %d\n", tag, mean, worst);
        snprintf(what, sizeof what, "%s: the two storages agree (mean abs diff < 4)", tag);
        check(mean < 4.0, what);
    }

    CVPixelBufferRelease(native);
    CVPixelBufferRelease(bgra);
    free(px_native);
    free(px_bgra);
    free(buf);
}

int main(void) {
    printf("=== video texture (native YCbCr vs BGRA, sampled through ANGLE) ===\n");

    setenv("KL_GLFB", "1", 1);
#ifdef KL_TEST_VIDEO_LAYER_MIRROR
    setenv("KL_GLFB_MTL","1",1);
#endif
    if (!kl_glfb_init()) {
        printf("  kl_glfb_init failed — this gate needs the PATCHED ANGLE "
               "(`make angle-debug`)\n");
        return 1;
    }
    kl_glfb_make_current();
#ifdef KL_TEST_VIDEO_LAYER_MIRROR
    kl_mtl_provider_install();
#endif
    check(gl_resolve(), "GL entry points resolve through kl_glfb_sym");
    uint32_t prog = make_program();
    check(prog != 0, "the sampling program compiles and links");
    if (!prog) return 1;

    run_stream(STREAM8, "8-bit", prog);
    run_stream(STREAM10, "main10", prog);

    printf(fails ? "\nFAIL: %d check(s) failed\n" : "\nPASS: video textures sample\n",
           fails);
    return fails ? 1 : 0;
}
