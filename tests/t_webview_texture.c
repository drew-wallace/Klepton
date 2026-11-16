// Real ANGLE readback: Android external names have no storage until the first
// browser frame. Also verify updates, resize and the existing video path.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "kl_glfb.h"
#define LOAD(ret,name,args) ret (*name) args=kl_glfb_sym(#name);assert(name)
int main(void) {
    assert(kl_glfb_init());kl_glfb_make_current();
    LOAD(void,glGenTextures,(int,uint32_t*));
    LOAD(void,glBindTexture,(uint32_t,uint32_t));
    LOAD(void,glTexStorage2D,(uint32_t,int,uint32_t,int,int));
    LOAD(void,glGenBuffers,(int,uint32_t*));
    LOAD(void,glBindBuffer,(uint32_t,uint32_t));
    LOAD(void,glPixelStorei,(uint32_t,int));
    // The guest capability gateway deliberately filters unlisted pnames.
    // Inspect ANGLE's actual saved unpack state for this host helper test.
    const char *dir=getenv("KL_ANGLE_DIR");assert(dir);
    char path[4096];snprintf(path,sizeof path,"%s/libGLESv2.dylib",dir);
    void *native=dlopen(path,RTLD_LAZY|RTLD_NOLOAD);assert(native);
    void (*glGetIntegerv)(uint32_t,int*)=dlsym(native,"glGetIntegerv");assert(glGetIntegerv);
    LOAD(void,glGenFramebuffers,(int,uint32_t*));
    LOAD(void,glBindFramebuffer,(uint32_t,uint32_t));
    LOAD(void,glFramebufferTexture2D,(uint32_t,uint32_t,uint32_t,uint32_t,int));
    LOAD(uint32_t,glCheckFramebufferStatus,(uint32_t));
    LOAD(void,glReadPixels,(int,int,int,int,uint32_t,uint32_t,void*));
    LOAD(uint32_t,glGetError,(void));
    while(glGetError()){}
    uint32_t textures[2],buffer,fb;glGenTextures(2,textures);glBindTexture(0x0de1,textures[1]);
    glGenBuffers(1,&buffer);glBindBuffer(0x88ec,buffer);
    uint32_t names[]={0x0cf5,0x0cf2,0x0cf3,0x0cf4,0x806e,0x806d};
    int values[]={8,7,3,2,4,1};
    for(int i=0;i<6;i++)glPixelStorei(names[i],values[i]);
    unsigned char bgra[]={0,0,255,255,255,0,0,255};
    assert(kl_glfb_upload_webview_bgra(textures[0],2,1,bgra,1));
    int actual;glGetIntegerv(0x8069,&actual);assert(actual==textures[1]);
    glGetIntegerv(0x88ef,&actual);assert(actual==buffer);
    for(int i=0;i<6;i++){glGetIntegerv(names[i],&actual);assert(actual==values[i]);}
    glGenFramebuffers(1,&fb);glBindFramebuffer(0x8d40,fb);glFramebufferTexture2D(0x8d40,0x8ce0,0x0de1,textures[0],0);
    assert(glCheckFramebufferStatus(0x8d40)==0x8cd5);
    unsigned char rgba[8],expected[]={255,0,0,255,0,0,255,255};
    glReadPixels(0,0,2,1,0x1908,0x1401,rgba);assert(!memcmp(rgba,expected,8));
    unsigned char green[]={0,255,0,255,0,255,0,255};
    assert(kl_glfb_upload_webview_bgra(textures[0],2,1,green,0));
    glReadPixels(0,0,2,1,0x1908,0x1401,rgba);assert(!memcmp(rgba,green,8));
    assert(kl_glfb_upload_webview_bgra(textures[0],1,2,bgra,1));
    glReadPixels(0,0,1,2,0x1908,0x1401,rgba);assert(!memcmp(rgba,expected,8));
    assert(kl_glfb_upload_video_bgra(textures[0],1,2,green));
    glReadPixels(0,0,1,2,0x1908,0x1401,rgba);assert(!memcmp(rgba,green,8));
    assert(glGetError()==0);
    // Unity can hand the browser a texture with immutable RGBA storage.
    // The first browser frame must upload into it, rather than redefine it.
    uint32_t immutable;glGenTextures(1,&immutable);glBindTexture(0x0de1,immutable);
    glTexStorage2D(0x0de1,1,0x8058,2,1);glBindTexture(0x0de1,textures[1]);
    assert(kl_glfb_upload_webview_bgra(immutable,2,1,bgra,1));
    glFramebufferTexture2D(0x8d40,0x8ce0,0x0de1,immutable,0);
    glReadPixels(0,0,2,1,0x1908,0x1401,rgba);
    assert(!memcmp(rgba,expected,8)&&!glGetError());
    assert(kl_glfb_upload_webview_bgra(immutable,2,1,green,0));
    glReadPixels(0,0,2,1,0x1908,0x1401,rgba);assert(!memcmp(rgba,green,8));
    unsigned char too_wide[12]={0};
    assert(!kl_glfb_upload_webview_bgra(immutable,3,1,too_wide,1));
    glReadPixels(0,0,2,1,0x1908,0x1401,rgba);assert(!memcmp(rgba,green,8)&&!glGetError());
    glGetIntegerv(0x8069,&actual);assert(actual==textures[1]);
    glGetIntegerv(0x88ef,&actual);assert(actual==buffer);
    for(int i=0;i<6;i++){glGetIntegerv(names[i],&actual);assert(actual==values[i]);}
    puts("WebView ANGLE texture: mutable/immutable first upload, BGRA pixels, update, resize rejection, state restoration, video regression");
}
