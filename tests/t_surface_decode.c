// The room-join video path: worker-thread JNI construction -> native window ->
// real MediaCodec decode -> SurfaceTexture.updateTexImage -> ANGLE pixels.
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kl_jni_int.h"
#include "kl_ndk.h"
#include "kl_mediandk.h"
#include "kl_glfb.h"

#define MEDIA(ret,name,args) ret (*name) args=kl_mediandk_lookup(#name);assert(name)
#define GL(ret,name,args) ret (*name) args=kl_glfb_sym(#name);assert(name)
static const char *st="android/graphics/SurfaceTexture",*surface_class="android/view/Surface";
static void *texture,*surface;
static uint32_t texture_name;
static kl_va args_va(uint64_t *a,int n){return (kl_va){.gr_top=a?a+n:NULL,.gr_offs=-8*n};}
static void *construct(const char *cls,const char *sig,uint64_t *a,int n){
    void *c=kl_jni_class(cls),*mid=klj_want(c,"<init>",sig,'m');
    kl_va va=args_va(a,n);return klj_NewObjectV(kl_jni_env(),c,mid,&va);
}
static klj_val call(void *self,const char *cls,const char *name,const char *sig,char kind){
    kl_va va=args_va(NULL,0);
    return klj_call(kl_jni_env(),self,klj_want(kl_jni_class(cls),name,sig,'m'),&va,kind);
}
static void *construct_on_worker(void *unused){
    assert(!kl_glfb_context_current());
    uint64_t a[]={texture_name};texture=construct(st,"(I)V",a,1);assert(texture);
    uint64_t b[]={(uintptr_t)texture};surface=construct(surface_class,"(Landroid/graphics/SurfaceTexture;)V",b,1);assert(surface);
    assert(!kl_glfb_context_current());return NULL;
}
static void *report_on_worker(void *unused){
    assert(!kl_glfb_context_current());
    FILE *report=tmpfile();assert(report);kl_glfb_draw_census(report);fclose(report);
    assert(!kl_glfb_context_current());return NULL;
}
typedef struct {int32_t offset,size;int64_t pts;uint32_t flags;} buffer_info;
int main(void){
    alarm(30);setenv("KL_VTDEC_BGRA","1",1);
    assert(kl_glfb_init());kl_glfb_make_current();
    GL(void,glGenTextures,(int,uint32_t*));GL(void,glBindTexture,(uint32_t,uint32_t));
    GL(void,glTexParameteri,(uint32_t,uint32_t,int));GL(uint32_t,glGetError,(void));
    GL(void,glGenFramebuffers,(int,uint32_t*));GL(void,glBindFramebuffer,(uint32_t,uint32_t));
    GL(void,glFramebufferTexture2D,(uint32_t,uint32_t,uint32_t,uint32_t,int));
    GL(uint32_t,glCheckFramebufferStatus,(uint32_t));
    GL(void,glReadPixels,(int,int,int,int,uint32_t,uint32_t,void*));
    while(glGetError()){}
    glGenTextures(1,&texture_name);glBindTexture(0x8d65,texture_name);
    glTexParameteri(0x8d65,0x2801,0x2601);glTexParameteri(0x8d65,0x2800,0x2601);
    pthread_t worker;assert(!pthread_create(&worker,NULL,construct_on_worker,NULL));assert(!pthread_join(worker,NULL));
    void *(*from_surface)(void*,void*)=kl_ndk_lookup("ANativeWindow_fromSurface");
    void (*release_window)(void*)=kl_ndk_lookup("ANativeWindow_release");
    void *window=from_surface(kl_jni_env(),surface);assert(window&&window!=kl_ndk_window());
    assert(call(surface,surface_class,"isValid","()Z",'Z').j);
    call(texture,st,"updateTexImage","()V",'V');assert(!call(texture,st,"getTimestamp","()J",'J').j);
    MEDIA(void*,AMediaFormat_new,(void));MEDIA(void,AMediaFormat_setString,(void*,const char*,const char*));
    MEDIA(void,AMediaFormat_setFloat,(void*,const char*,float));MEDIA(bool,AMediaFormat_getFloat,(void*,const char*,float*));
    MEDIA(bool,AMediaFormat_getInt32,(void*,const char*,int32_t*));MEDIA(int,AMediaFormat_delete,(void*));
    MEDIA(void*,AMediaCodec_createDecoderByType,(const char*));MEDIA(int,AMediaCodec_configure,(void*,void*,void*,void*,uint32_t));
    MEDIA(int,AMediaCodec_start,(void*));MEDIA(int,AMediaCodec_stop,(void*));MEDIA(int,AMediaCodec_delete,(void*));
    MEDIA(ssize_t,AMediaCodec_dequeueInputBuffer,(void*,int64_t));MEDIA(uint8_t*,AMediaCodec_getInputBuffer,(void*,size_t,size_t*));
    MEDIA(int,AMediaCodec_queueInputBuffer,(void*,size_t,off_t,size_t,uint64_t,uint32_t));
    MEDIA(ssize_t,AMediaCodec_dequeueOutputBuffer,(void*,buffer_info*,int64_t));
    MEDIA(int,AMediaCodec_releaseOutputBuffer,(void*,size_t,bool));
    void *format=AMediaFormat_new();AMediaFormat_setString(format,"mime","video/hevc");
    AMediaFormat_setFloat(format,"operating-rate",59.94f);float rate=0;int32_t integer=123;
    assert(AMediaFormat_getFloat(format,"operating-rate",&rate)&&rate==59.94f);
    assert(!AMediaFormat_getInt32(format,"operating-rate",&integer)&&integer==123);
    void *codec=AMediaCodec_createDecoderByType("video/hevc");assert(codec);
    assert(!AMediaCodec_configure(codec,format,window,NULL,0));assert(!AMediaCodec_start(codec));
    assert(!AMediaFormat_delete(format));
    FILE *file=fopen("tests/data/t_hevc.h265","rb");assert(file);fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
    uint8_t *stream=malloc(size);assert(fread(stream,1,size,file)==(size_t)size);fclose(file);
    long cuts[64];int count=0,seen_vcl=0;
    for(long i=0;i+4<size;i++){
        int prefix=!memcmp(stream+i,"\0\0\0\1",4)?4:!memcmp(stream+i,"\0\0\1",3)?3:0;
        if(!prefix)continue;
        if(seen_vcl){cuts[count++]=i;seen_vcl=0;}
        if(((stream[i+prefix]>>1)&63)<=31)seen_vcl=1;
        i+=prefix-1;
    }
    cuts[count++]=size;assert(count==30);
    long previous=0;
    for(int i=0;i<2;i++){
        ssize_t slot=AMediaCodec_dequeueInputBuffer(codec,0);assert(slot>=0);
        size_t capacity;uint8_t *input=AMediaCodec_getInputBuffer(codec,slot,&capacity);size_t n=cuts[i]-previous;
        assert(input&&n<=capacity);memcpy(input,stream+previous,n);previous=cuts[i];
        assert(!AMediaCodec_queueInputBuffer(codec,slot,0,n,(i+1)*33333,0));
        buffer_info info;ssize_t out;
        do{out=AMediaCodec_dequeueOutputBuffer(codec,&info,1000000);}while(out==-2);
        assert(out>=0&&info.pts==(i+1)*33333);assert(!AMediaCodec_releaseOutputBuffer(codec,out,true));
    }
    free(stream);
    call(texture,st,"updateTexImage","()V",'V');assert(call(texture,st,"getTimestamp","()J",'J').j==66666000);
    uint32_t fb;glGenFramebuffers(1,&fb);glBindFramebuffer(0x8d40,fb);
    glFramebufferTexture2D(0x8d40,0x8ce0,0x0de1,texture_name,0);assert(glCheckFramebufferStatus(0x8d40)==0x8cd5);
    uint8_t *pixels=malloc(320*240*4);glReadPixels(0,0,320,240,0x1908,0x1401,pixels);assert(!glGetError());
    unsigned different=0,opaque=0;
    for(int i=0;i<320*240;i++){if(memcmp(pixels,pixels+4*i,3))different++;if(pixels[4*i+3]==255)opaque++;}
    assert(different>1000&&opaque==320*240);free(pixels);
    // Reporting from a context-free worker must remain safe even with live FBOs.
    assert(!pthread_create(&worker,NULL,report_on_worker,NULL));assert(!pthread_join(worker,NULL));
    call(surface,surface_class,"release","()V",'V');call(surface,surface_class,"release","()V",'V');
    assert(!from_surface(kl_jni_env(),surface));
    call(texture,st,"release","()V",'V');call(texture,st,"release","()V",'V');
    // Acquired native windows and the configured codec keep the queue alive.
    int32_t w,h;kl_ndk_window_size(window,&w,&h);assert(w==320&&h==240);
    assert(!AMediaCodec_stop(codec));assert(!AMediaCodec_delete(codec));release_window(window);
    puts("SurfaceTexture: worker construction, real native window, HEVC decode, latest timestamp, GPU pixels and release lifetime pass");
}
