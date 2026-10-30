#import <AppKit/AppKit.h>
#include <assert.h>
#include <unistd.h>
#include "kl_webview.h"
static int initialized, finished, failed, message, evaluated;
static int focusedText, focusedNone;
static int pushedURL, replacedURL, changedTitle;
static char result[512];
static void event(uint64_t view,const char *name,const char *value,uint64_t request) {
    assert([NSThread isMainThread]);
    if (!strcmp(name,"HandleInitFinished")) initialized++;
    if (!strcmp(name,"HandleLoadFinished")) finished++;
    if (!strcmp(name,"HandleLoadFailed")) failed++;
    if (!strcmp(name,"HandleMessageEmitted")&&!strcmp(value,"clicked")) message++;
    if (!strcmp(name,"HandleMessageEmitted")&&strstr(value,"vuplex.webview.focusedInputFieldChanged")) {
        NSDictionary *object=[NSJSONSerialization JSONObjectWithData:[[NSString stringWithUTF8String:value] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        assert(object.count==2); // Focus classification must never include field contents.
        if ([object[@"value"] isEqual:@"TEXT"])focusedText++;
        else if ([object[@"value"] isEqual:@"NONE"])focusedNone++;
    }
    if (!strcmp(name,"HandleMessageEmitted")&&value[0]=='{') {
        NSDictionary *object=[NSJSONSerialization JSONObjectWithData:[[NSString stringWithUTF8String:value] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        if ([object[@"type"] isEqual:@"vuplex.webview.urlChanged"]) {
            NSDictionary *action=object[@"urlAction"];
            assert(action[@"Title"]!=nil && action[@"Url"]!=nil);
            if ([action[@"Type"] isEqual:@"PUSH_STATE"])pushedURL++;
            if ([action[@"Type"] isEqual:@"REPLACE_STATE"])replacedURL++;
        }
        if ([object[@"type"] isEqual:@"vuplex.webview.titleChanged"] && [object[@"value"] isEqual:@"SPA fixture"])changedTitle++;
    }
    if (request) { evaluated++; snprintf(result,sizeof result,"%s",value); }
}
static void pump(double seconds) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while (end.timeIntervalSinceNow>0) [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}
int main(void) { @autoreleasepool {
    [NSApplication sharedApplication];
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,240) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;
    [window makeKeyAndOrderFront:nil];
    assert(!kl_webview_create(1,0,240,NULL,0,event));
    void *view=kl_webview_create(1,320,240,NULL,0,event);
    pump(1); assert(initialized==1);
    assert(!kl_webview_is_fullscreen(view));
    kl_webview_command(view,"exitFullscreen",0);pump(.1);
    assert(!kl_webview_is_fullscreen(view));
    kl_webview_load(view,"<html><body style='margin:0;background:#ff0000'><button style='width:200px;height:100px' onclick=\"document.title='Clicked';window.vuplex.postMessage('clicked')\">Click me</button></body></html>","https://example.invalid/",1);
    for(int i=0;i<150&&!finished;i++) pump(.1);
    assert(finished==1);
    kl_webview_pointer(view,"pointerdown",50,40,0);
    kl_webview_pointer(view,"pointerup",50,40,0);
    for(int i=0;i<50&&!message;i++) pump(.1);
    assert(message==1);
    kl_webview_pointer(view,"pointerdown",50,40,2);
    kl_webview_pointer(view,"pointerup",50,40,2);pump(.2);assert(message==1);
    kl_webview_eval(view,"document.title",41);
    for(int i=0;i<50&&!evaluated;i++) pump(.1);
    assert(evaluated==1&&!strcmp(result,"Clicked"));
    kl_webview_eval(view,"const input=document.createElement('input');input.id='fixture';input.value='not-to-be-logged';document.body.append(input);input.focus()",0);
    pump(.3);assert(!focusedText&&!focusedNone);
    kl_webview_command(view,"focusedInputFieldEvents",1);
    for(int i=0;i<50&&!focusedText;i++)pump(.1);assert(focusedText==1);
    kl_webview_command(view,"focusedInputFieldEvents",0);
    kl_webview_eval(view,"document.activeElement.blur()",0);pump(.3);
    assert(focusedNone==0);
    kl_webview_command(view,"focusedInputFieldEvents",1);
    for(int i=0;i<50&&!focusedNone;i++)pump(.1);assert(focusedNone==1);
    kl_webview_eval(view,"history.pushState({},'', '#watch-fixture');history.replaceState({},'', '#replace-fixture');document.title='SPA fixture'",0);
    for(int i=0;i<50&&(!pushedURL||!replacedURL||!changedTitle);i++)pump(.1);
    assert(pushedURL==1&&replacedURL==1&&changedTitle==1);
    uint64_t serial=0; int w=0,h=0; void *pixels=NULL;
    for(int i=0;i<50&&!pixels;i++) {pump(.1);pixels=kl_webview_copy_frame(view,&serial,&w,&h);}
    assert(pixels&&w==320&&h==240);
    unsigned char *p=pixels; size_t offset=((size_t)200*w+250)*4;
    assert(p[offset]<10&&p[offset+1]<10&&p[offset+2]>245&&p[offset+3]>245);
    free(pixels);
    void *second=kl_webview_create(2,100,80,NULL,0,event);
    kl_webview_load(second,"<body style='background:#00ff00'>Independent</body>",NULL,1);
    pump(2); assert(initialized==2&&finished==2);
    // Pausing one browser freezes its texture while a separate view keeps
    // producing frames. Resume preserves an explicitly disabled renderer.
    kl_webview_command(view,"paused",1);pump(.3);
    pixels=kl_webview_copy_frame(view,&serial,&w,&h);free(pixels);
    pump(.4);assert(!kl_webview_copy_frame(view,&serial,&w,&h));
    uint64_t secondSerial=0;
    pixels=kl_webview_copy_frame(second,&secondSerial,&w,&h);assert(pixels);free(pixels);
    pump(.3);
    pixels=kl_webview_copy_frame(second,&secondSerial,&w,&h);assert(pixels);free(pixels);
    kl_webview_command(view,"rendering",0);
    kl_webview_command(view,"paused",0);pump(.4);
    assert(!kl_webview_copy_frame(view,&serial,&w,&h));
    kl_webview_command(view,"rendering",1);pump(.4);
    pixels=kl_webview_copy_frame(view,&serial,&w,&h);assert(pixels);free(pixels);
    kl_webview_resize(view,160,120); pump(1);
    pixels=kl_webview_copy_frame(view,&serial,&w,&h);assert(pixels&&w==160&&h==120);free(pixels);
    kl_webview_eval(view,"window.vuplex.addEventListener('message',e=>document.title=e.data);window.vuplex._emit('message',{data:'from native'});document.title",42);
    for(int i=0;i<50&&evaluated<2;i++)pump(.1);
    assert(evaluated==2&&!strcmp(result,"from native"));
    NSString *root=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
    [@"body{background:rgb(0,255,0)}" writeToFile:[root stringByAppendingPathComponent:@"style.css"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [@"<link rel='stylesheet' href='style.css'><body>Local asset</body>" writeToFile:[root stringByAppendingPathComponent:@"page one.html"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    kl_webview_load(view,"file:///android_asset/page%20one.html",root.UTF8String,0);
    for(int i=0;i<50&&finished<3;i++)pump(.1);assert(finished==3);
    kl_webview_eval(view,"getComputedStyle(document.body).backgroundColor",43);
    for(int i=0;i<50&&evaluated<3;i++)pump(.1);
    assert(evaluated==3&&!strcmp(result,"rgb(0, 255, 0)"));
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    kl_webview_user_agent(second,"Independent browser");
    kl_webview_user_agent(view,"Custom browser");
    kl_webview_eval(view,"navigator.userAgent",44);
    for(int i=0;i<50&&evaluated<4;i++)pump(.1);
    assert(evaluated==4&&!strcmp(result,"Custom browser"));
    kl_webview_user_agent_mode(view,0);
    kl_webview_load(view,"<body>Desktop</body>",NULL,1);
    for(int i=0;i<50&&finished<4;i++)pump(.1);assert(finished==4);
    kl_webview_eval(view,"navigator.userAgent",45);
    for(int i=0;i<50&&evaluated<5;i++)pump(.1);
    assert(evaluated==5&&strstr(result,"AppleWebKit/")&&!strstr(result,"Custom browser"));
    kl_webview_user_agent_mode(view,1);
    kl_webview_load(view,"<body>Mobile</body>",NULL,1);
    for(int i=0;i<50&&finished<5;i++)pump(.1);assert(finished==5);
    kl_webview_eval(view,"navigator.userAgent",46);
    for(int i=0;i<50&&evaluated<6;i++)pump(.1);
    assert(evaluated==6&&strstr(result,"AppleWebKit/")&&!strstr(result,"Custom browser"));
    kl_webview_eval(second,"navigator.userAgent",47);
    for(int i=0;i<50&&evaluated<7;i++)pump(.1);
    assert(evaluated==7&&!strcmp(result,"Independent browser"));
    kl_webview_load(view,"http://127.0.0.1:1/fails",NULL,0);
    for(int i=0;i<50&&!failed;i++)pump(.1);assert(failed==1);
    kl_webview_close(view);kl_webview_close(second);pump(1);
    int callbacks=initialized+finished+failed+message+evaluated;pump(.5);assert(callbacks==initialized+finished+failed+message+evaluated);
    puts("WebKit: real page, JavaScript result/message, pointer click, native-to-page message, local APK asset and CSS, BGRA pixels, resize, independent pause/resume, rendering policy, navigation failure, close");
    [window close];
}return 0;}
