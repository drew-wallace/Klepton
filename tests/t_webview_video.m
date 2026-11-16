// Exercise the shipping WebKit snapshot path with locally encoded H.264.
// No network service, authentication or downloaded content is involved.
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#include <assert.h>
#include "kl_webview.h"

static int loaded, evaluated;
static char result[512];
static void event(uint64_t id,const char *name,const char *value,uint64_t request) {
    if (!strcmp(name,"HandleLoadFinished")) loaded++;
    if (request) { evaluated++;snprintf(result,sizeof result,"%s",value); }
}
static void pump(double seconds) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(end.timeIntervalSinceNow>0)
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
static double captureCadence(void *view,double seconds) {
    uint64_t serial=0;int w=0,h=0;
    free(kl_webview_copy_frame(view,&serial,&w,&h));uint64_t first=serial;
    double start=NSProcessInfo.processInfo.systemUptime;
    while(NSProcessInfo.processInfo.systemUptime-start<seconds){
        pump(.05);free(kl_webview_copy_frame(view,&serial,&w,&h));
    }
    return (serial-first)/(NSProcessInfo.processInfo.systemUptime-start);
}
static void makeVideo(NSURL *url) {
    int width=getenv("KL_TEST_HD_VIDEO")?1280:320,height=getenv("KL_TEST_HD_VIDEO")?720:240;
    NSError *error=nil;
    AVAssetWriter *writer=[[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
    assert(writer);
    AVAssetWriterInput *input=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
        outputSettings:@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(width),AVVideoHeightKey:@(height)}];
    AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:
        @{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
          (id)kCVPixelBufferWidthKey:@(width),(id)kCVPixelBufferHeightKey:@(height)}];
    [writer addInput:input];assert([writer startWriting]);
    [writer startSessionAtSourceTime:kCMTimeZero];
    for(int frame=0;frame<72;frame++) {
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:10];
        while(!input.readyForMoreMediaData&&deadline.timeIntervalSinceNow>0)pump(.01);
        assert(input.readyForMoreMediaData);
        CVPixelBufferRef pixel=NULL;
        assert(CVPixelBufferPoolCreatePixelBuffer(NULL,adaptor.pixelBufferPool,&pixel)==kCVReturnSuccess);
        CVPixelBufferLockBaseAddress(pixel,0);
        uint8_t *data=CVPixelBufferGetBaseAddress(pixel);size_t stride=CVPixelBufferGetBytesPerRow(pixel);
        for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
            uint8_t *p=data+y*stride+x*4;p[0]=frame/24==1?255:0;
            p[1]=frame/24==2?255:0;p[2]=frame/24==0?255:0;p[3]=255;
        }
        CVPixelBufferUnlockBaseAddress(pixel,0);
        assert([adaptor appendPixelBuffer:pixel withPresentationTime:CMTimeMake(frame,24)]);
        CVPixelBufferRelease(pixel);
    }
    [input markAsFinished];__block BOOL finished=NO;
    [writer finishWritingWithCompletionHandler:^{finished=YES;}];
    for(int i=0;i<1000&&!finished;i++)pump(.01);
    assert(finished&&writer.status==AVAssetWriterStatusCompleted);
}
int main(void) { @autoreleasepool {
    [NSApplication sharedApplication];
    int width=getenv("KL_TEST_HD_VIDEO")?1400:320,height=getenv("KL_TEST_HD_VIDEO")?788:240;
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,width,height)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;[window makeKeyAndOrderFront:nil];
    NSString *folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]);
    makeVideo([NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"colors.mp4"]]);
    NSString *media=[NSString stringWithFormat:@"<video autoplay muted loop playsinline style='width:%dpx;height:%dpx' src='colors.mp4'></video><button style='position:absolute;left:0;top:0;width:30px;height:30px;border:0;background:cyan;z-index:1' onclick='window.fixtureClicked=1'></button>",width,height];
    NSString *html=nil;
    if(getenv("KL_TEST_SHADOW_VIDEO")) {
        NSData *data=[NSJSONSerialization dataWithJSONObject:media options:NSJSONWritingFragmentsAllowed error:nil];
        NSString *quoted=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        html=[NSString stringWithFormat:@"<body style='margin:0;background:black'><div id='host'></div><script>const outer=document.querySelector('#host').attachShadow({mode:'open'});outer.innerHTML='<div id=inner></div>';const inner=outer.querySelector('#inner').attachShadow({mode:'open'});inner.innerHTML=%@;window.fixtureVideo=inner.querySelector('video');</script></body>",quoted];
    } else html=[NSString stringWithFormat:@"<body style='margin:0;background:black'>%@<script>window.fixtureVideo=document.querySelector('video')</script></body>",media];
    NSString *page=[folder stringByAppendingPathComponent:@"index.html"];
    assert([html writeToFile:page atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    void *view=kl_webview_create(1,width,height,NULL,1,event);
    kl_webview_load(view,[NSURL fileURLWithPath:page].absoluteString.UTF8String,NULL,0);
    for(int i=0;i<100&&!loaded;i++)pump(.1);assert(loaded==1);
    uint64_t serial=0;
    for(int phase=0;phase<2;phase++) {
        if(phase) {
            kl_webview_command(view,"exitFullscreen",0);
            kl_webview_command(view,"reload",0);
            for(int i=0;i<100&&loaded<2;i++)pump(.1);assert(loaded==2);
        }
        unsigned colors=0;uint8_t last[4]={0};int controlsVisible=0;
        uint64_t firstSerial=serial;
        double captureStart=NSProcessInfo.processInfo.systemUptime;
        for(int i=0;i<50;i++) {
            pump(.1);int w=0,h=0;uint8_t *pixels=kl_webview_copy_frame(view,&serial,&w,&h);
            if(!pixels)continue;
            uint8_t *p=pixels+((size_t)(h/2)*w+w/2)*4;
            memcpy(last,p,4);
            // H.264's RGB/YUV conversion is lossy; distinguish primaries without
            // demanding exact RGB endpoints from the decoder's color conversion.
            if(p[2]>180&&p[1]<90&&p[0]<90)colors|=1;
            if(p[0]>180&&p[1]<90&&p[2]<90)colors|=2;
            if(p[1]>180&&p[0]<90&&p[2]<90)colors|=4;
            uint8_t *control=pixels+((size_t)15*w+15)*4;
            controlsVisible|=control[0]>240&&control[1]>240&&control[2]<10;
            free(pixels);
        }
        kl_webview_eval(view,"(()=>{const v=window.fixtureVideo;return JSON.stringify({time:v.currentTime,paused:v.paused,ready:v.readyState,error:v.error?.code||0})})()",phase+1);
        for(int i=0;i<50&&evaluated<phase+1;i++)pump(.1);assert(evaluated==phase+1);
        printf("Local H.264 %s: %s; snapshot colors=%u; last BGRA=%u,%u,%u,%u\n",phase?"after refresh":"playback",result,colors,last[0],last[1],last[2],last[3]);
        NSDictionary *state=[NSJSONSerialization JSONObjectWithData:
            [[NSString stringWithUTF8String:result] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        assert(state&&![state[@"paused"] boolValue]&&[state[@"ready"] intValue]>=2&&[state[@"error"] intValue]==0);
        assert(colors==7);
        assert(controlsVisible);
        double captureFPS=(serial-firstSerial)/(NSProcessInfo.processInfo.systemUptime-captureStart);
        printf("Completed browser captures: %.1f FPS\n",captureFPS);
        if(getenv("KL_TEST_CAPTURE_CADENCE"))assert(captureFPS>15);
    }
    if(getenv("KL_WEBVIEW_VIDEO_CANVAS")) {
        if(getenv("KL_TEST_CAPTURE_CADENCE")) {
            kl_webview_eval(view,"window.fixtureVideo.pause()",0);pump(.3);
            double idleFPS=captureCadence(view,2);
            printf("Paused video captures: %.1f FPS\n",idleFPS);assert(idleFPS>=5&&idleFPS<=13);
            kl_webview_eval(view,"window.fixtureVideo.play();void 0",0);pump(.3);
            double resumedFPS=captureCadence(view,2);
            printf("Resumed video captures: %.1f FPS\n",resumedFPS);assert(resumedFPS>15);
        }
        kl_webview_pointer(view,"pointerdown",15,15,0);
        kl_webview_pointer(view,"pointerup",15,15,0);
        kl_webview_eval(view,"[...window.__kleptonVideoCanvasStats,window.fixtureClicked||0]",3);
        for(int i=0;i<50&&evaluated<3;i++)pump(.1);assert(evaluated==3);
        assert(!strcmp(result,"[1,1,0,1]"));
        kl_webview_eval(view,"window.fixtureVideo.remove();window.__kleptonVideoCanvas();window.__kleptonVideoCanvasStats",4);
        for(int i=0;i<50&&evaluated<4;i++)pump(.1);assert(evaluated==4);
        assert(!strcmp(result,"[0,0,0]")); // No retained canvases after navigation/UI removes video.
        // Native keyboard/scroll events must reach controls inside the same
        // nested open roots as the cloud player, rather than its outer host.
        kl_webview_eval(view,"(()=>{const host=document.createElement('div');host.style.cssText='position:absolute;left:0;top:40px;width:200px;height:120px';document.body.append(host);const root=host.attachShadow({mode:'open'});root.innerHTML='<div id=inner></div>';const inner=root.querySelector('#inner').attachShadow({mode:'open'});inner.innerHTML='<input style=\"width:150px;height:25px\"><div id=scroll style=\"height:50px;overflow:auto\"><div style=\"height:500px\"></div></div>';window.fixtureInput=inner.querySelector('input');window.fixtureScroll=inner.querySelector('#scroll')})()",0);
        pump(.2);
        kl_webview_pointer(view,"pointerdown",20,55,0);
        kl_webview_pointer(view,"pointerup",20,55,0);
        kl_webview_key(view,"a");kl_webview_key(view,"b");kl_webview_key(view,"Backspace");
        kl_webview_scroll(view,0,80,20,85);pump(.2);
        kl_webview_eval(view,"[window.fixtureInput.value,window.fixtureScroll.scrollTop>0]",5);
        for(int i=0;i<50&&evaluated<5;i++)pump(.1);assert(evaluated==5);
        assert(!strcmp(result,"[\"a\",true]"));
    }
    kl_webview_close(view);pump(.2);[window close];
    [[NSFileManager defaultManager] removeItemAtPath:folder error:nil];
    puts("WebKit video: decoded red, blue and green frames reach the texture snapshot path before and after refresh");
}return 0;}
