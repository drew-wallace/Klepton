// Real JNI -> AVPlayer -> ANGLE regression, using local H.264 and generated HLS.
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <assert.h>
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <dlfcn.h>
#include "kl_jni_int.h"
#include "kl_jni_bigscreen.h"
#include "kl_glfb.h"
extern char **environ;
static NSString *folder;
@interface Segments : NSObject <AVAssetWriterDelegate>
@property(nonatomic,strong) NSMutableArray<NSNumber *> *durations;
@end
@implementation Segments
- (void)assetWriter:(AVAssetWriter *)writer didOutputSegmentData:(NSData *)data segmentType:(AVAssetSegmentType)type segmentReport:(AVAssetSegmentReport *)report {
    @synchronized(self){
        NSString *name;
        if(type==AVAssetSegmentTypeInitialization)name=@"init.mp4";
        else {
            name=[NSString stringWithFormat:@"segment%lu.m4s",(unsigned long)self.durations.count];
            double duration=CMTimeGetSeconds(report.trackReports.firstObject.duration);assert(isfinite(duration)&&duration>0);
            [self.durations addObject:@(duration)];
        }
        assert([data writeToFile:[folder stringByAppendingPathComponent:name] atomically:YES]);
    }
}
@end
static void pump(double seconds){
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(end.timeIntervalSinceNow>0)[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
static void makeVideo(BOOL hls){
    NSError *error=nil;Segments *segments=[Segments new];segments.durations=[NSMutableArray array];
    AVAssetWriter *writer=hls?[[AVAssetWriter alloc]initWithContentType:UTTypeMPEG4Movie]:
        [[AVAssetWriter alloc]initWithURL:[NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"colors.mp4"]] fileType:AVFileTypeMPEG4 error:&error];assert(writer);
    if(hls){writer.delegate=segments;writer.outputFileTypeProfile=AVFileTypeProfileMPEG4AppleHLS;writer.preferredOutputSegmentInterval=CMTimeMake(1,1);writer.initialSegmentStartTime=kCMTimeZero;}
    AVAssetWriterInput *input=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@320,AVVideoHeightKey:@240,
        AVVideoCompressionPropertiesKey:@{AVVideoMaxKeyFrameIntervalKey:@24,AVVideoAllowFrameReorderingKey:@NO}}];
    AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:@{
        (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),(id)kCVPixelBufferWidthKey:@320,(id)kCVPixelBufferHeightKey:@240}];
    [writer addInput:input];assert([writer startWriting]);[writer startSessionAtSourceTime:kCMTimeZero];
    for(int f=0;f<96;f++){
        for(int spin=0;!input.readyForMoreMediaData&&spin<1000;spin++)pump(.01);assert(input.readyForMoreMediaData);
        CVPixelBufferRef pixel;assert(!CVPixelBufferPoolCreatePixelBuffer(NULL,adaptor.pixelBufferPool,&pixel));
        CVPixelBufferLockBaseAddress(pixel,0);uint8_t *bytes=CVPixelBufferGetBaseAddress(pixel);size_t stride=CVPixelBufferGetBytesPerRow(pixel);
        for(int y=0;y<240;y++)for(int x=0;x<320;x++){
            uint8_t *p=bytes+y*stride+x*4;int color=f/24%3;p[0]=color==1?255:0;p[1]=color==2?255:0;p[2]=color==0?255:0;p[3]=255;
        }
        CVPixelBufferUnlockBaseAddress(pixel,0);assert([adaptor appendPixelBuffer:pixel withPresentationTime:CMTimeMake(f,24)]);CVPixelBufferRelease(pixel);
    }
    [writer endSessionAtSourceTime:CMTimeMake(4,1)];[input markAsFinished];__block BOOL done=NO;
    [writer finishWritingWithCompletionHandler:^{done=YES;}];for(int i=0;i<1000&&!done;i++)pump(.01);
    assert(done&&writer.status==AVAssetWriterStatusCompleted);
    if(hls){
        NSMutableString *list=[@"#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-MAP:URI=\"init.mp4\"\n" mutableCopy];
        @synchronized(segments){for(NSUInteger i=0;i<segments.durations.count;i++)[list appendFormat:@"#EXTINF:%.6f,\nsegment%lu.m4s\n",segments.durations[i].doubleValue,(unsigned long)i];}
        [list appendString:@"#EXT-X-ENDLIST\n"];assert([list writeToFile:[folder stringByAppendingPathComponent:@"index.m3u8"] atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    }
}
static const char *api="com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI";
static klj_val call(const char *cls,const char *name,const char *sig,uint64_t *a,int n,char kind){
    kl_va va={.gr_top=a?a+n:NULL,.gr_offs=-8*n};
    return klj_call(kl_jni_env(),kl_jni_class(cls),klj_want(kl_jni_class(cls),name,sig,'M'),&va,kind);
}
static int ready,playing,ended,errors,drm,reentrant;
static pthread_t guest;
static void message(void *env,void *cls,void *target,void *method,void *bytes){
    assert(pthread_equal(guest,pthread_self()));assert(!strcmp(klj_str(target),"MediaFixture"));
    const char *name=klj_str(method);klj_array *data=klj_arr(bytes);assert(data);
    if(!strcmp(name,"ready")){
        ready++;
        if(reentrant){uint64_t a[]={0};call(api,"Stop","(I)V",a,1,'V');}
    }
    if(!strcmp(name,"playing"))playing++;
    if(!strcmp(name,"ended"))ended++;
    if(!strcmp(name,"drm"))drm++;
    if(!strcmp(name,"error")){
        errors++;NSData *text=[NSData dataWithBytes:data->data length:data->len];
        NSDictionary *error=[NSJSONSerialization JSONObjectWithData:text options:0 error:nil];assert(error[@"errorCode"]&&error[@"message"]);
        assert(![error[@"message"] containsString:@"secret"]&&! [error[@"message"] containsString:@"http"]);
    }
}
static void start(const char *url,const char *mime){
    uint64_t a[]={0,(uintptr_t)kl_jni_new_string(url),(uintptr_t)kl_jni_new_string(mime)};
    call(api,"StartStream","(ILjava/lang/String;Ljava/lang/String;)V",a,3,'V');
}
static void setFloat(const char *name,float value){
    void *env=kl_jni_env();void **table=*(void***)env;
    void(*invoke)(void*,void*,void*,const klj_jvalue*)=table[143];
    klj_jvalue a[2]={{.i=0},{.f=value}};
    void *cls=kl_jni_class(api);invoke(env,cls,klj_want(cls,name,"(IF)V",'M'),a);
}
static void sink(const uint8_t *rgba,int w,int h,void *ctx){(void)rgba;(void)w;(void)h;(void)ctx;}
static void *present_without_context(void *unused){assert(!kl_glfb_context_current());kl_glfb_present(NULL);return NULL;}
int main(void){@autoreleasepool{
    alarm(90);[NSApplication sharedApplication];guest=pthread_self();
    folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]);
    makeVideo(NO);makeVideo(YES);
    // An ephemeral loopback server exercises actual HLS requests with no service/account.
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(fd,(void*)&addr,sizeof addr));socklen_t len=sizeof addr;assert(!getsockname(fd,(void*)&addr,&len));int port=ntohs(addr.sin_port);close(fd);
    char portText[16];snprintf(portText,sizeof portText,"%d",port);
    char *args[]={"python3","-m","http.server",portText,"--bind","127.0.0.1","--directory",(char*)folder.UTF8String,NULL};
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);posix_spawn_file_actions_adddup2(&actions,1,2);
    pid_t server;assert(!posix_spawnp(&server,"python3",&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);pump(.3);
    setenv("KL_GLFB_SIZE","64x64",1);assert(kl_glfb_init());kl_glfb_make_current();kl_glfb_set_frame_sink(sink,NULL);
    void **table=*(void***)kl_jni_env();int(*reg)(void*,void*,const kl_jni_method*,int)=table[215];
    kl_jni_method callback={"nativeUnitySendMessage","(Ljava/lang/String;Ljava/lang/String;[B)V",message};
    assert(!reg(kl_jni_env(),kl_jni_class("com/unity3d/player/UnityPlayer"),&callback,1));
    const char *callbacks="com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks";
    const char *registers[]={"RegisterPlaybackReadyCallback","RegisterPlayingCallback","RegisterPlaybackEndedCallback","RegisterPlaybackErrorCallback","RegisterDRMLicenseErrorCallback"};
    const char *names[]={"ready","playing","ended","error","drm"};
    for(int i=0;i<5;i++){uint64_t a[]={(uintptr_t)kl_jni_new_string("MediaFixture"),(uintptr_t)kl_jni_new_string(names[i])};call(callbacks,registers[i],"(Ljava/lang/String;Ljava/lang/String;)V",a,2,'V');}
    assert(!call(api,"CreateBigMediaPlayer","()I",NULL,0,'I').j);uint64_t id[]={0},autoplay[]={0,1};
    call(api,"InitiateMediaPlayer","(I)V",id,1,'V');call(api,"SetAutoplay","(IZ)V",autoplay,2,'V');
    uint32_t streamTexture=(uint32_t)call(api,"GetVideoTextureId","(I)I",id,1,'I').j;
    void(*flush)(void)=kl_glfb_sym("glFlush");assert(flush);
    void(*genFB)(int,uint32_t*)=kl_glfb_sym("glGenFramebuffers");void(*bindFB)(uint32_t,uint32_t)=kl_glfb_sym("glBindFramebuffer");
    void(*attach)(uint32_t,uint32_t,uint32_t,uint32_t,int)=kl_glfb_sym("glFramebufferTexture2D");
    void(*read)(int,int,int,int,uint32_t,uint32_t,void*)=kl_glfb_sym("glReadPixels");uint32_t(*status)(uint32_t)=kl_glfb_sym("glCheckFramebufferStatus");
    uint32_t fb;genFB(1,&fb);
    uint32_t sentinel;void(*genTexture)(int,uint32_t*)=kl_glfb_sym("glGenTextures");
    void(*bindTexture)(uint32_t,uint32_t)=kl_glfb_sym("glBindTexture");
    genTexture(1,&sentinel);bindTexture(0x0de1,sentinel);
    NSString *angle=[NSString stringWithFormat:@"%s/libGLESv2.dylib",getenv("KL_ANGLE_DIR")];
    void *gles=dlopen(angle.UTF8String,RTLD_LAZY|RTLD_NOLOAD);assert(gles);
    void(*getInteger)(uint32_t,int*)=dlsym(gles,"glGetIntegerv");assert(getInteger);
    for(int phase=0;phase<2;phase++){
        ready=playing=ended=errors=0;
        NSString *url=phase?[NSString stringWithFormat:@"http://127.0.0.1:%d/index.m3u8",port]:[NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"colors.mp4"]].absoluteString;
        if(phase){uint64_t a[]={0,(uintptr_t)kl_jni_new_string(url.UTF8String)};call(api,"StartStream","(ILjava/lang/String;)V",a,2,'V');}
        else start(url.UTF8String,"video/mp4");
        unsigned colors=0;int paused=0,seeked=0;
        for(int i=0;i<250&&(!ended||colors!=7);i++){
            pump(.03);
            if(!(i%2)){pthread_t thread;assert(!pthread_create(&thread,NULL,present_without_context,NULL));assert(!pthread_join(thread,NULL));flush();}
            else kl_glfb_present(NULL);
            klj_bigscreen_drain_events();assert(!errors);
            int binding=0;getInteger(0x8069,&binding);assert(binding==sentinel);
            if(!call(api,"IsFrameAvailable","(I)Z",id,1,'Z').j)continue;
            // Presentation must upload before the guest queries its texture ID.
            bindFB(0x8d40,fb);attach(0x8d40,0x8ce0,0x0de1,streamTexture,0);assert(status(0x8d40)==0x8cd5);
            uint32_t texture=(uint32_t)call(api,"GetVideoTextureId","(I)I",id,1,'I').j;assert(texture==streamTexture);
            uint8_t p[4];read(160,120,1,1,0x1908,0x1401,p);
            if(p[0]>180&&p[1]<90&&p[2]<90)colors|=1;
            if(p[2]>180&&p[0]<90&&p[1]<90)colors|=2;
            if(p[1]>180&&p[0]<90&&p[2]<90)colors|=4;
            int64_t position=call(api,"GetCurrentPosition","(I)J",id,1,'J').j;
            if(!phase&&!paused&&position>300){
                call(api,"Pause","(I)V",id,1,'V');pump(.15);
                int64_t before=call(api,"GetCurrentPosition","(I)J",id,1,'J').j;pump(.2);
                int64_t after=call(api,"GetCurrentPosition","(I)J",id,1,'J').j;assert(llabs(after-before)<80);
                setFloat("SetVolume",.25f);assert(fabs(call(api,"GetVolume","(I)F",id,1,'F').d-.25)<.001);
                call(api,"Resume","(I)V",id,1,'V');paused=1;
            }
            if(!phase&&!seeked&&position>1200){setFloat("SetCurrentTime",2.05f);pump(.15);assert(call(api,"GetCurrentPosition","(I)J",id,1,'J').j>=1900);seeked=1;}
        }
        assert(ready==1&&playing>=1&&ended==1&&colors==7);
        if(!phase)assert(paused&&seeked);
        assert(call(api,"GetDuration","(I)J",id,1,'J').j>3500);
        printf("Bigscreen %s: ready, playing, end, real dimensions and three GPU colors pass\n",phase?"HTTP HLS":"local H.264");
    }
    int before=errors;start("https://invalid.example/secret.mpd?secret=hidden","application/dash+xml");pump(.1);assert(errors==before);klj_bigscreen_drain_events();assert(errors==before+1);
    start("not-a-stream-secret","");pump(.1);klj_bigscreen_drain_events();assert(errors==before+2);
    start([NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"missing.mp4"]].absoluteString.UTF8String,"video/mp4");
    for(int i=0;i<150&&errors<before+3;i++){pump(.03);klj_bigscreen_drain_events();}assert(errors==before+3);
    uint64_t protected[]={0,0,0,0};call(api,"StartProtectedMediaSession","(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",protected,4,'V');klj_bigscreen_drain_events();assert(drm==1);
    reentrant=1;playing=ready=0;start([NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"colors.mp4"]].absoluteString.UTF8String,"video/mp4");
    for(int i=0;i<150&&!ready;i++){pump(.03);klj_bigscreen_drain_events();}
    assert(ready==1);int old=playing;pump(.2);klj_bigscreen_drain_events();assert(playing==old);
    call(api,"Shutdown","(I)V",id,1,'V');pump(.1);kill(server,SIGTERM);waitpid(server,NULL,0);
    [NSFileManager.defaultManager removeItemAtPath:folder error:nil];
    puts("Bigscreen streams: unsupported formats report errors; protected media reports DRM absence; callbacks allow reentrant stop");
}return 0;}
