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
static void makeVideo(NSURL *url) {
    NSError *error=nil;
    AVAssetWriter *writer=[[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
    assert(writer);
    AVAssetWriterInput *input=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
        outputSettings:@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@320,AVVideoHeightKey:@240}];
    AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:
        @{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
          (id)kCVPixelBufferWidthKey:@320,(id)kCVPixelBufferHeightKey:@240}];
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
        for(int y=0;y<240;y++)for(int x=0;x<320;x++) {
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
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,240)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;[window makeKeyAndOrderFront:nil];
    NSString *folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]);
    makeVideo([NSURL fileURLWithPath:[folder stringByAppendingPathComponent:@"colors.mp4"]]);
    NSString *html=@"<body style='margin:0;background:black'><video autoplay muted loop playsinline style='width:320px;height:240px' src='colors.mp4'></video></body>";
    NSString *page=[folder stringByAppendingPathComponent:@"index.html"];
    assert([html writeToFile:page atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    void *view=kl_webview_create(1,320,240,NULL,1,event);
    kl_webview_load(view,[NSURL fileURLWithPath:page].absoluteString.UTF8String,NULL,0);
    for(int i=0;i<100&&!loaded;i++)pump(.1);assert(loaded==1);
    uint64_t serial=0;
    for(int phase=0;phase<2;phase++) {
        if(phase) {
            kl_webview_command(view,"exitFullscreen",0);
            kl_webview_command(view,"reload",0);
            for(int i=0;i<100&&loaded<2;i++)pump(.1);assert(loaded==2);
        }
        unsigned colors=0;uint8_t last[4]={0};
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
            free(pixels);
        }
        kl_webview_eval(view,"(()=>{const v=document.querySelector('video');return JSON.stringify({time:v.currentTime,paused:v.paused,ready:v.readyState,error:v.error?.code||0})})()",phase+1);
        for(int i=0;i<50&&evaluated<phase+1;i++)pump(.1);assert(evaluated==phase+1);
        printf("Local H.264 %s: %s; snapshot colors=%u; last BGRA=%u,%u,%u,%u\n",phase?"after refresh":"playback",result,colors,last[0],last[1],last[2],last[3]);
        NSDictionary *state=[NSJSONSerialization JSONObjectWithData:
            [[NSString stringWithUTF8String:result] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        assert(state&&![state[@"paused"] boolValue]&&[state[@"ready"] intValue]>=2&&[state[@"error"] intValue]==0);
        assert(colors==7);
    }
    kl_webview_close(view);pump(.2);[window close];
    [[NSFileManager defaultManager] removeItemAtPath:folder error:nil];
    puts("WebKit video: decoded red, blue and green frames reach the texture snapshot path before and after refresh");
}return 0;}
