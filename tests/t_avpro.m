// Reuse the generated public-domain color fixture and real ANGLE setup.
#define main bigscreen_stream_fixture_main
#include "t_bigscreen_stream.m"
#undef main
#include "klepton.h"
static const char *manager="com/RenderHeads/AVProVideo/AVProMobileVideo";
static const char *avplayer="com/RenderHeads/AVProVideo/AVProVideoPlayer";
static int other_service_frames;
static void other_service(void){other_service_frames++;}
static klj_val avcall(void *self,const char *cls,const char *name,const char *sig,uint64_t *a,int n,char kind){
    kl_va va={.gr_top=a?a+n:NULL,.gr_offs=-8*n};
    return klj_call(kl_jni_env(),self,klj_want(kl_jni_class(cls),name,sig,self?'m':'M'),&va,kind);
}
static void avfloat(void *self,const char *name,float value){
    void **table=*(void***)kl_jni_env();void(*invoke)(void*,void*,void*,const klj_jvalue*)=table[63];
    klj_jvalue a={.f=value};invoke(kl_jni_env(),self,klj_want(kl_jni_class(avplayer),name,"(F)V",'m'),&a);
}
int main(int argc,char **argv){@autoreleasepool{
    assert(argc==2);alarm(60);kl_thread_init();[NSApplication sharedApplication];
    kl_set_library_path(argv[1]);kl_jni_env();kl_jni_set_native_lib_dir(argv[1]);setenv("KL_GLFB_SIZE","64x64",1);
    folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]);makeVideo(NO);
    kl_jni_set_assets_dir(folder.UTF8String);
    assert(kl_glfb_init());kl_glfb_make_current();kl_glfb_set_frame_sink(sink,NULL);
    assert(kl_glfb_add_render_service(other_service));assert(kl_glfb_add_render_service(other_service));
    kl_va empty={0};void *cls=kl_jni_class(manager);
    void *mgr=klj_NewObjectV(kl_jni_env(),cls,klj_want(cls,"<init>","()V",'m'),&empty);assert(mgr);
    assert(!strcmp(klj_str(avcall(mgr,manager,"GetPluginVersion","()Ljava/lang/String;",NULL,0,'L').l),"1.9.17"));
    kl_image *plugin=kl_find_image("libAVProLocal.so");assert(plugin);
    int(*nativeWidth)(int)=kl_sym(plugin,"_GetWidth");int(*nativeFrame)(int)=kl_sym(plugin,"_GetFrameCount");
    unsigned(*nativeTexture)(int)=kl_sym(plugin,"_GetTextureHandle");assert(nativeWidth&&nativeFrame&&nativeTexture);
    uint64_t options[]={2,0,0,0,0};void *p=avcall(mgr,manager,"CreatePlayer","(IZZIZ)Lcom/RenderHeads/AVProVideo/AVProVideoPlayer;",options,5,'L').l;assert(p);
    NSString *url=@"jar:file:///fixture.apk!/assets/colors.mp4";
    uint64_t source[]={(uintptr_t)kl_jni_new_string(url.UTF8String),0,(uintptr_t)kl_jni_new_string(""),0};
    assert(avcall(p,avplayer,"OpenVideoFromFile","(Ljava/lang/String;JLjava/lang/String;I)Z",source,4,'Z').j);
    avcall(p,avplayer,"Play","()V",NULL,0,'V');
    void(*gen)(int,uint32_t*)=kl_glfb_sym("glGenFramebuffers");void(*bind)(uint32_t,uint32_t)=kl_glfb_sym("glBindFramebuffer");
    void(*attach)(uint32_t,uint32_t,uint32_t,uint32_t,int)=kl_glfb_sym("glFramebufferTexture2D");
    uint32_t(*status)(uint32_t)=kl_glfb_sym("glCheckFramebufferStatus");
    void(*read)(int,int,int,int,uint32_t,uint32_t,void*)=kl_glfb_sym("glReadPixels");
    uint32_t fb;gen(1,&fb);unsigned colors=0;
    for(int i=0;i<200&&colors!=7;i++){
        pump(.03);kl_glfb_present(NULL);
        assert(!avcall(p,avplayer,"GetLastErrorCode","()I",NULL,0,'I').j);
        if(!nativeFrame(0))continue;
        assert(nativeWidth(0)==320);assert(nativeTexture(0));
        bind(0x8d40,fb);attach(0x8d40,0x8ce0,0x0de1,nativeTexture(0),0);assert(status(0x8d40)==0x8cd5);
        uint8_t pixel[4];read(160,120,1,1,0x1908,0x1401,pixel);
        if(pixel[0]>180&&pixel[1]<90&&pixel[2]<90)colors|=1;
        if(pixel[2]>180&&pixel[0]<90&&pixel[1]<90)colors|=2;
        if(pixel[1]>180&&pixel[0]<90&&pixel[2]<90)colors|=4;
    }
    assert(colors==7);assert(other_service_frames>0);assert(avcall(p,avplayer,"HasVideo","()Z",NULL,0,'Z').j);
    assert(avcall(p,avplayer,"GetDurationMs","()J",NULL,0,'J').j>3500);
    avcall(p,avplayer,"Pause","()V",NULL,0,'V');pump(.1);assert(avcall(p,avplayer,"IsPaused","()Z",NULL,0,'Z').j);
    avfloat(p,"SetVolume",.25f);assert(fabs(avcall(p,avplayer,"GetVolume","()F",NULL,0,'F').d-.25)<.001);
    uint64_t seekTo[]={1000};avcall(p,avplayer,"Seek","(I)V",seekTo,1,'V');pump(.2);
    assert(llabs((int64_t)avcall(p,avplayer,"GetCurrentTimeMs","()J",NULL,0,'J').j-1000)<100);
    source[0]=(uintptr_t)kl_jni_new_string("file:///missing-avpro-video.mp4");
    assert(avcall(p,avplayer,"OpenVideoFromFile","(Ljava/lang/String;JLjava/lang/String;I)Z",source,4,'Z').j);
    int error=0;for(int i=0;i<100&&!error;i++){pump(.03);error=avcall(p,avplayer,"GetLastErrorCode","()I",NULL,0,'I').j;}
    assert(error==100);assert(!avcall(p,avplayer,"GetLastErrorCode","()I",NULL,0,'I').j);
    uint64_t id[]={0};avcall(mgr,manager,"DestroyPlayer","(I)V",id,1,'V');kl_glfb_present(NULL);pump(.1);
    assert(avcall(p,avplayer,"GetDeinitialised","()Z",NULL,0,'Z').j);
    void *replacement=avcall(mgr,manager,"CreatePlayer","(IZZIZ)Lcom/RenderHeads/AVProVideo/AVProVideoPlayer;",options,5,'L').l;assert(replacement);
    assert(!avcall(replacement,avplayer,"GetPlayerIndex","()I",NULL,0,'I').j);
    avcall(mgr,manager,"DestroyPlayer","(I)V",id,1,'V');kl_glfb_present(NULL);pump(.1);
    [NSFileManager.defaultManager removeItemAtPath:folder error:nil];
    puts("AVPro original native JNI getters, real decode/GPU colors, pause/seek/volume, consumed errors and player reuse pass");
}return 0;}
