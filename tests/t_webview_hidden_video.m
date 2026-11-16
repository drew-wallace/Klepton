// Actual WebKit MediaStream and JPEG transfer, with a hidden native host.
// Synthetic canvas frames only; no network, camera, microphone or user media.
#import <AppKit/AppKit.h>
#include <assert.h>
#include "../runtime/media/kl_webview.m"
static int loaded;
static void event(uint64_t view,const char *name,const char *value,uint64_t request){
    if(!strcmp(name,"HandleLoadFinished"))loaded++;
}
static void pump(double seconds){
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
int main(void){@autoreleasepool{
    setenv("KL_WEBVIEW_VIDEO_CANVAS","1",1);
    [NSApplication sharedApplication];
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,240)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;[window makeKeyAndOrderFront:nil];
    void *view=kl_webview_create(1,320,240,NULL,1,event);KLBrowser *browser=KL_OBJECT(view);
    BOOL letterbox=getenv("KL_TEST_LETTERBOX_STREAM")!=NULL;
    kl_webview_load(view,"<body style='margin:0;background:black'><div id=host></div><script>"
        "const root=document.querySelector('#host').attachShadow({mode:'open'});"
        "root.innerHTML='<video autoplay muted playsinline style=\"position:absolute;inset:0;width:100vw;height:100vh\"></video>';"
        "const c=document.createElement('canvas');c.width=320;c.height=240;const x=c.getContext('2d');"
        "window.fixtureDraw=i=>{x.fillStyle=['red','blue','lime'][i%3];x.fillRect(0,0,320,240);"
        "x.fillStyle='yellow';x.fillRect(0,0,40,40);x.fillStyle='magenta';x.fillRect(280,200,40,40);};"
        "fixtureDraw(0);root.querySelector('video').srcObject=c.captureStream(30);</script>",NULL,1);
    for(int i=0;i<100&&!loaded;i++)pump(.1);assert(loaded==1);
    if(letterbox){
        kl_webview_eval(view,"const host=document.querySelector('#host');host.style.cssText='position:absolute;inset:0';host.shadowRoot.querySelector('video').style.cssText='position:absolute;left:50%;top:50%;width:280px;height:210px;transform:translate(-50%,-50%)'",0);
        pump(.3);
    }
    uint64_t serial=0;
    for(int phase=0;phase<5;phase++){
        if(phase==1)[window orderOut:nil];
        if(phase==2)[window makeKeyAndOrderFront:nil];
        if(phase==3)[browser.web removeFromSuperview];
        if(phase==4)[window.contentView addSubview:browser.web];
        pump(.3);unsigned direct=browser->videoStreamFrames,colors=0;
        for(int i=0;i<24;i++){
            char script[64];snprintf(script,sizeof script,"fixtureDraw(%d)",i/4);
            kl_webview_eval(view,script,0);pump(.15);
            int w=0,h=0;uint8_t *pixels=kl_webview_copy_frame(view,&serial,&w,&h);if(!pixels)continue;
            assert(w==320&&h==240);
            uint8_t *p=pixels+((size_t)120*w+160)*4;
            if(p[2]>180&&p[1]<90&&p[0]<90)colors|=1;
            if(p[0]>180&&p[1]<90&&p[2]<90)colors|=2;
            if(p[1]>180&&p[0]<90&&p[2]<90)colors|=4;
            assert(p[3]>=248);
            if(phase==1||phase==3){
                uint8_t *top=pixels+((size_t)(letterbox?35:20)*w+(letterbox?35:20))*4;
                uint8_t *bottom=pixels+((size_t)(letterbox?205:220)*w+(letterbox?285:300))*4;
                assert(top[2]>200&&top[1]>200&&top[0]<50);
                assert(bottom[2]>200&&bottom[0]>200&&bottom[1]<50);
                if(letterbox){uint8_t *bar=pixels+((size_t)120*w+5)*4;assert(bar[0]<20&&bar[1]<20&&bar[2]<20);}
            }
            free(pixels);
        }
        printf("MediaStream phase=%d hidden=%d colors=%u direct-frames=%u\n",phase,[browser hiddenHost],colors,browser->videoStreamFrames-direct);
        assert(colors==7);
        if(phase==1||phase==3)assert(browser->videoStreamFrames>direct);
        else assert(browser->videoStreamFrames==direct);
        if(phase==1||phase==3){
            uint64_t before=browser->serial;
            uint8_t blank[4*4*4]={0};CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
            CGContextRef context=CGBitmapContextCreate(blank,4,4,8,16,color,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
            CGImageRef empty=CGBitmapContextCreateImage(context);
            [browser consumeCGImage:empty];assert(browser->serial==before);
            CGImageRelease(empty);CGContextRelease(context);CGColorSpaceRelease(color);
        }
    }
    assert(!videoStreamImage(@42));
    assert(!videoStreamImage(@"data:image/svg+xml;base64,AAAA"));
    assert(!videoStreamImage(@"data:image/jpeg;base64,not base64"));
    kl_webview_close(view);pump(.2);[window close];
    puts("WebKit hidden/detached host: nested full-page MediaStream preserves changing RGB, orientation and opacity; native capture resumes when host returns");
}return 0;}
