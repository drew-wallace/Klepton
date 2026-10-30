// Android's SurfaceTexture consumer backed by the existing decoded-image queue.
// Construction is CPU-only; updateTexImage binds real frames on the GL thread.
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "kl_jni_int.h"
#include "kl_jni_surface.h"
#include "kl_mediandk.h"
#include "kl_glfb.h"
#include "kl_egl.h"

typedef struct {
    atomic_uint refs;
    pthread_mutex_t lock;
    unsigned texture;
    int released;
    int64_t timestamp;
    void *reader, *window, *image;
} surface_texture;
typedef struct { surface_texture *texture; } surface_handle;

static void reader_delete(void *reader) {
    void (*fn)(void *) = kl_mediandk_lookup("AImageReader_delete"); fn(reader);
}
static void texture_unref(void *data) {
    surface_texture *s = data;
    if (!s || atomic_fetch_sub(&s->refs, 1) != 1) return;
    if (s->image) kl_glfb_image_destroy(s->image);
    if (s->reader) reader_delete(s->reader);
    pthread_mutex_destroy(&s->lock); free(s);
}
static void surface_unref(void *data) {
    surface_handle *h=data;
    if(h){texture_unref(h->texture);free(h);}
}
static surface_texture *texture_of(void *self, const char *cls) {
    klj_object *o = klj_as_object(self);
    return o && !strcmp(o->cls, cls) ? o->data : NULL;
}
static klj_val texture_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; if (n != 1) return (klj_val){0};
    surface_texture *s = calloc(1, sizeof *s);
    if (!s) return (klj_val){0};
    atomic_init(&s->refs, 1); pthread_mutex_init(&s->lock, NULL);
    s->texture = (unsigned)a[0].j;
    int (*create)(int32_t,int32_t,int32_t,uint64_t,int32_t,void **) = kl_mediandk_lookup("AImageReader_newWithUsage");
    int (*window)(void *,void **) = kl_mediandk_lookup("AImageReader_getWindow");
    if (create(1,1,0x22,0x100,3,&s->reader) || window(s->reader,&s->window)) {
        if (s->reader) reader_delete(s->reader);
        pthread_mutex_destroy(&s->lock); free(s); return (klj_val){0};
    }
    void *object=kl_jni_new_object("android/graphics/SurfaceTexture");
    klj_object *o = klj_as_object(object); o->data = s; o->destroy = texture_unref;
    return (klj_val){.l=object};
}
static klj_val surface_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    surface_texture *s = n == 1 ? texture_of(a[0].l,"android/graphics/SurfaceTexture") : NULL;
    if (s) {
        surface_handle *h=calloc(1,sizeof *h);
        if (!h) return (klj_val){0};
        atomic_fetch_add(&s->refs,1);
        h->texture=s;
        void *object=kl_jni_new_object("android/view/Surface");
        klj_object *o = klj_as_object(object); o->data=h; o->destroy=surface_unref;
        return (klj_val){.l=object};
    }
    return (klj_val){0};
}
int klj_surface_window(void *surface, void **window) {
    klj_object *o=klj_as_object(surface);
    if (!o || strcmp(o->cls,"android/view/Surface") || o->destroy!=surface_unref) return 0;
    surface_handle *h=o->data; surface_texture *s=h->texture;
    *window=NULL;
    if(s){pthread_mutex_lock(&s->lock);*window=s->released?NULL:s->window;pthread_mutex_unlock(&s->lock);}
    return 1;
}
static klj_val update(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    surface_texture *s=texture_of(self,"android/graphics/SurfaceTexture");
    if (!s || !kl_glfb_context_current()) return (klj_val){0};
    pthread_mutex_lock(&s->lock);
    void *frame=NULL;
    int (*acquire)(void *,void **) = kl_mediandk_lookup("AImageReader_acquireLatestImage");
    if (!s->released && !acquire(s->reader,&frame)) {
        void *hw=NULL;
        int (*hardware)(void *,void **) = kl_mediandk_lookup("AImage_getHardwareBuffer");
        int (*timestamp)(void *,int64_t *) = kl_mediandk_lookup("AImage_getTimestamp");
        void (*delete_frame)(void *) = kl_mediandk_lookup("AImage_delete");
        if (!hardware(frame,&hw)) {
            void *image=kl_glfb_image_from_pixels(kl_mediandk_buffer_pixels(hw),NULL,NULL);
            if (image) {
                void (*bind)(unsigned,unsigned)=kl_egl_sym("glBindTexture");
                bind(0x8d65,s->texture); // updateTexImage also binds texName.
                if (kl_glfb_image_bind(image)) {
                    if (s->image) kl_glfb_image_destroy(s->image);
                    s->image=image; timestamp(frame,&s->timestamp);
                } else kl_glfb_image_destroy(image);
            }
        }
        delete_frame(frame);
    }
    pthread_mutex_unlock(&s->lock); return (klj_val){0};
}
static klj_val timestamp(void *env, void *self, const klj_val *a, int n) {
    (void)env;(void)a;(void)n;
    surface_texture *s=texture_of(self,"android/graphics/SurfaceTexture");
    int64_t value=0;
    if(s){pthread_mutex_lock(&s->lock);value=s->timestamp;pthread_mutex_unlock(&s->lock);}
    return (klj_val){.j=value};
}
static klj_val release_texture(void *env, void *self, const klj_val *a, int n) {
    (void)env;(void)a;(void)n;
    surface_texture *s=texture_of(self,"android/graphics/SurfaceTexture");
    if(s){
        pthread_mutex_lock(&s->lock);s->released=1;
        if(s->image){kl_glfb_image_destroy(s->image);s->image=NULL;}
        if(s->reader){reader_delete(s->reader);s->reader=NULL;s->window=NULL;}
        pthread_mutex_unlock(&s->lock);
    }
    return (klj_val){0};
}
static klj_val release_surface(void *env, void *self, const klj_val *a, int n) {
    (void)env;(void)a;(void)n;
    klj_object *o=klj_as_object(self);
    if(o && o->destroy==surface_unref){
        surface_handle *h=o->data;surface_texture *s=h->texture;
        h->texture=NULL;texture_unref(s);
    }
    return (klj_val){0};
}
static klj_val valid(void *env, void *self, const klj_val *a, int n) {
    (void)env;(void)a;(void)n;void *window=NULL;
    return (klj_val){.j=klj_surface_window(self,&window)&&window!=NULL};
}
const klj_binding klj_bind_surface[] = {
    {"android/graphics/SurfaceTexture","<init>","(I)V",texture_init},
    {"android/graphics/SurfaceTexture","updateTexImage","()V",update},
    {"android/graphics/SurfaceTexture","getTimestamp","()J",timestamp},
    {"android/graphics/SurfaceTexture","release","()V",release_texture},
    {"android/view/Surface","<init>","(Landroid/graphics/SurfaceTexture;)V",surface_init},
    {"android/view/Surface","release","()V",release_surface},
    {"android/view/Surface","isValid","()Z",valid},
    {NULL,NULL,NULL,NULL}
};
