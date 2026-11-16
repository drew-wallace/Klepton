// Real ANGLE regression: OVROverlay gets handles on a thread with no GL
// context; its image must be created by the render thread before retry succeeds.
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "kl_glfb.h"
#include "kl_ovrp.h"
#include "t_mtl_provider.h"

static uint64_t (*get_image)(int,int,int,uint64_t*);
typedef struct { int layer,stage; uint64_t result,image; } request;
static void *query(void *opaque) {
    request *r=opaque;
    r->image=0;r->result=get_image(r->layer,r->stage,0,&r->image);
    return NULL;
}
static void from_worker(request *r) {
    pthread_t t;assert(!pthread_create(&t,NULL,query,r));assert(!pthread_join(t,NULL));
}
#define GL(ret,name,args) ret (*name) args=kl_glfb_sym(#name);assert(name)
int main(void) {
    setenv("KL_GLFB","1",1);setenv("KL_GLFB_SIZE","64x64",1);
    setenv("KL_GLFB_MTL","1",1);
    assert(kl_glfb_init());kl_glfb_make_current();
    kl_mtl_provider_install();
    get_image=kl_ovrp_sym("ovrp_GetLayerTexturePtr");assert(get_image);
    uint64_t (*calculate)(int,int,const void*,int,int,int,int,void*)=kl_ovrp_sym("ovrp_CalculateLayerDesc");
    uint64_t (*setup)(void*,const void*,int*)=kl_ovrp_sym("ovrp_SetupLayer");
    uint64_t (*end)(int,const void*,int,void*)=kl_ovrp_sym("ovrp_EndFrame4");
    uint64_t (*destroy)(int)=kl_ovrp_sym("ovrp_DestroyLayer");
    assert(calculate&&setup&&end&&destroy);
    uint32_t desc[31]={0};int size[]={64,32},layer=0;
    assert(!calculate(0,1,size,1,1,1,0,desc));assert(!setup(NULL,desc,&layer));
    request r={.layer=layer};from_worker(&r);
    assert((int64_t)r.result==-1002&&!r.image);
    from_worker(&r);assert((int64_t)r.result==-1002&&!r.image);
    GL(void,glGenTextures,(int,uint32_t*));GL(void,glBindTexture,(uint32_t,uint32_t));
    GL(void,glGetIntegerv,(uint32_t,int*));
    uint32_t sentinel;glGenTextures(1,&sentinel);glBindTexture(0x0de1,sentinel);
    assert(!end(0,NULL,0,NULL));
    int binding=0;glGetIntegerv(0x8069,&binding);assert(binding==(int)sentinel);
    from_worker(&r);assert(!r.result&&r.image&&r.image!=sentinel);
    GL(void,glGenFramebuffers,(int,uint32_t*));GL(void,glBindFramebuffer,(uint32_t,uint32_t));
    GL(void,glFramebufferTexture2D,(uint32_t,uint32_t,uint32_t,uint32_t,int));
    GL(uint32_t,glCheckFramebufferStatus,(uint32_t));
    GL(void,glClearColor,(float,float,float,float));GL(void,glClear,(uint32_t));
    GL(void,glReadPixels,(int,int,int,int,uint32_t,uint32_t,void*));
    uint32_t fb;glGenFramebuffers(1,&fb);glBindFramebuffer(0x8d40,fb);
    glFramebufferTexture2D(0x8d40,0x8ce0,0x0de1,(uint32_t)r.image,0);
    assert(glCheckFramebufferStatus(0x8d40)==0x8cd5);
    glClearColor(1,0,1,1);glClear(0x4000);
    unsigned char pixel[4]={0};glReadPixels(0,0,1,1,0x1908,0x1401,pixel);
    assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==255&&pixel[3]==255);
    uint64_t (*submit)(uint32_t,void*,void*,int,int,const float*,const float*,int,int64_t,const float*,int64_t,const float*,const float*)=kl_ovrp_sym("ovrp_EnqueueSubmitLayer2");
    assert(submit);
    float pose[]={0,0,0,1,0,0,-1},scale[]={1,-1,1};
    assert(!submit(1,(void*)(uintptr_t)r.image,(void*)(uintptr_t)r.image,layer,7,pose,scale,0,0,NULL,0,NULL,NULL));
    assert(!submit(1,(void*)(uintptr_t)r.image,(void*)(uintptr_t)r.image,layer,7,pose,scale,0,0,NULL,0,NULL,NULL));
    assert(!end(1,NULL,0,NULL));
    GL(void,glFinish,(void));glFinish();
    int width=0,height=0;
    assert(kl_mtl_count_lit_layer(layer,0,&width,&height)>0);
    assert(width==64&&height==32); // Actual pixels reached compositor storage.
    kl_ovrp_overlay overlay={0};assert(kl_ovrp_overlay_get(0,&overlay));
    assert(overlay.size[0]==1&&overlay.size[1]==1&&overlay.mirror_y&&!overlay.mirror_x);
    assert(!overlay.underlay); // onTop bit set.
    float rects[16]={0,0,.5f,1,.5f,0,.5f,1,1,1,0,0,1,1,0,0};
    float colors[]={.5f,.5f,.5f,.5f},bias[]={0,0,0,.1f};
    uint32_t right;glGenTextures(1,&right);glBindTexture(0x0de1,right);
    GL(void,glTexImage2D,(uint32_t,int,int,int,int,int,uint32_t,uint32_t,const void*));
    unsigned char green[64*32*4];
    for(unsigned i=0;i<sizeof green;i+=4){green[i]=0;green[i+1]=255;green[i+2]=0;green[i+3]=255;}
    glTexImage2D(0x0de1,0,0x1908,64,32,0,0x1908,0x1401,green);
    // Android's eight-byte bool slots do not define their upper padding bytes.
    int64_t paddedTrue=INT64_C(0x1234567800000001);
    assert(!submit(0x44,(void*)(uintptr_t)r.image,(void*)(uintptr_t)right,layer,8,pose,scale,0,paddedTrue,rects,paddedTrue,colors,bias));
    assert(!submit(0x44,(void*)(uintptr_t)r.image,(void*)(uintptr_t)right,layer,8,pose,scale,0,paddedTrue,rects,paddedTrue,colors,bias));
    assert(!end(2,NULL,0,NULL));
    glFinish();
    assert(kl_ovrp_overlay_get(0,&overlay)&&overlay.underlay);
    assert(overlay.tex_right==right);
    int rightLayer=kl_ovrp_overlay_texture_layer(layer,1);
    assert(rightLayer!=layer&&kl_mtl_count_lit_layer(rightLayer,0,&width,&height)>0);
    assert(width==64&&height==32);
    assert(kl_glfb_layer_mtl_texture(rightLayer,0,&width,&height)!=
           kl_glfb_layer_mtl_texture(layer,0,&width,&height));
    assert(overlay.has_uv_rect&&overlay.uv_rect[0][2]==.5f&&overlay.uv_rect[1][0]==.5f);
    assert(overlay.has_color_scale_bias&&overlay.color_scale[3]==.5f&&overlay.color_bias[3]==.1f);
    // A pending request for a layer destroyed before the next frame is ignored.
    request stale={.layer=layer,.stage=1};from_worker(&stale);
    assert((int64_t)stale.result==-1002);assert(!destroy(layer));
    assert(kl_ovrp_overlay_count()==0); // Destroyed menus must stop compositing immediately.
    assert(!end(2,NULL,0,NULL));from_worker(&stale);
    assert((int64_t)stale.result==-1001&&!stale.image);
    // EnqueueSetupLayer2's depth, not creation or first-submit timing, puts the
    // small browser control panel ahead of a theater's screen underlay.
    uint64_t (*setup_ordered)(const void*,int,int*)=kl_ovrp_sym("ovrp_EnqueueSetupLayer2");
    assert(setup_ordered);int front=0,back=0;
    assert(!setup_ordered(desc,-10,&front));assert(!setup_ordered(desc,10,&back));
    assert(!submit(0x44,(void*)(uintptr_t)r.image,NULL,front,9,pose,scale,0,0,NULL,0,NULL,NULL));
    assert(!submit(0x44,(void*)(uintptr_t)right,NULL,back,9,pose,scale,1,0,NULL,0,NULL,NULL));
    assert(!submit(0x44,(void*)(uintptr_t)r.image,NULL,front,9,pose,scale,0,0,NULL,0,NULL,NULL));
    assert(!end(3,NULL,0,NULL));glFinish();
    assert(kl_ovrp_overlay_count()==2);
    assert(kl_ovrp_overlay_get(0,&overlay)&&overlay.layer_id==back&&overlay.composition_depth==10);
    assert(kl_ovrp_overlay_get(1,&overlay)&&overlay.layer_id==front&&overlay.composition_depth==-10);
    assert(!destroy(front));assert(!destroy(back));
    puts("Overlay images: deferred allocation, worker-thread retry, real GPU storage and Metal mirror, binding restoration, stale-layer request");
}
