#import <AppKit/AppKit.h>
#include <assert.h>
#include "kl_jni_int.h"
#include "kl_jni_vuplex.h"

static int ready,loaded,clicked,focused,context;
static void message(void *env,void *cls,void *target,void *method,void *bytes) {
    assert(NSThread.isMainThread);
    const char *name=klj_str(method);klj_array *array=klj_arr(bytes);assert(array);
    char text[2048];size_t n=array->len<sizeof text-1?array->len:sizeof text-1;
    memcpy(text,array->data,n);text[n]=0;
    if (!strcmp(name,"HandleInitFinished"))ready++;
    if (!strcmp(name,"HandleLoadFinished"))loaded++;
    if (!strcmp(name,"HandleMessageEmitted")&&!strcmp(text,"clicked"))clicked++;
    if (!strcmp(name,"HandleMessageEmitted")&&!strcmp(text,"context"))context++;
    if (!strcmp(name,"HandleMessageEmitted")&&strstr(text,"focusedInputFieldChanged")&&strstr(text,"TEXT"))focused++;
}
static kl_va arguments(uint64_t *args,int n) {
    int regs=n>8?8:n;
    return (kl_va){.gr_top=args?args+regs:NULL,.gr_offs=-8*regs,.stack=args&&n>8?args+8:NULL};
}
static klj_val call(void *self,const char *cls,const char *name,const char *sig,uint64_t *args,int n,char ret) {
    void *clazz=kl_jni_class(cls),*mid=klj_want(clazz,name,sig,self?'m':'M');
    kl_va va=arguments(args,n);return klj_call(kl_jni_env(),self?:clazz,mid,&va,ret);
}
static void pump(double seconds) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while (end.timeIntervalSinceNow>0) {
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        klj_vuplex_drain_events();
    }
}
int main(void) { @autoreleasepool {
    [NSApplication sharedApplication];
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,240) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;[window makeKeyAndOrderFront:nil];
    kl_jni_method native={"nativeUnitySendMessage","(Ljava/lang/String;Ljava/lang/String;[B)V",message};
    assert(!klj_RegisterNatives(kl_jni_env(),kl_jni_class("com/unity3d/player/UnityPlayer"),&native,1));
    const char *web="com/vuplex/webview/WebView3D",*big="com/bigscreenvr/webview/BigWebView";
    uint64_t args[9]={(uintptr_t)kl_jni_new_string("BrowserInputFixture"),0,320,240,0,0,0,0,0};
    void *clazz=kl_jni_class(web),*ctor=klj_want(clazz,"<init>","(Ljava/lang/String;IIIZZZLcom/vuplex/webview/BiConsumer;Ljava/lang/Object;)V",'m');
    kl_va va=arguments(args,9);void *view=klj_NewObjectV(kl_jni_env(),clazz,ctor,&va);assert(view);
    for(int i=0;i<100&&!ready;i++)pump(.1);assert(ready==1);
    uint64_t html[]={(uintptr_t)kl_jni_new_string("<body><button style='position:absolute;left:20px;top:20px;width:120px;height:60px' onclick=\"window.vuplex.postMessage('clicked')\" oncontextmenu=\"event.preventDefault();window.vuplex.postMessage('context')\">Click</button><input style='position:absolute;left:20px;top:120px;width:200px;height:40px'></body>")};
    call(view,web,"loadHtml","(Ljava/lang/String;)V",html,1,'V');
    for(int i=0;i<100&&!loaded;i++)pump(.1);assert(loaded==1);
    uint64_t enable[]={1};call(view,web,"setFocusedInputFieldEventsEnabled","(Z)V",enable,1,'V');
    int client=(int)call(NULL,big,"CreateBigWebClient","()I",NULL,0,'I').j;
    void *nativeView=call(view,web,"getNativeWebView","()Landroid/webkit/WebView;",NULL,0,'L').l;
    uint64_t setup[]={(uint64_t)client,(uintptr_t)nativeView,320,240};
    call(NULL,big,"SetupClient","(ILandroid/webkit/WebView;II)V",setup,4,'V');
    // Exact constants emitted by Bigscreen.Browser.BigWebView's managed code:
    // DEFAULT_BUTTON_ID=1, DEFAULT_BUTTON_STATE=1, native-page input target=2.
    uint64_t click[]={(uint64_t)client,50,40,1,1,2};
    call(NULL,big,"PointerDown","(IIIIII)V",click,6,'V');pump(.1);
    call(NULL,big,"PointerUp","(IIIIII)V",click,6,'V');
    for(int i=0;i<50&&!clicked;i++)pump(.1);assert(clicked==1);
    // The separate Vuplex API still interprets MouseButton.Right explicitly.
    klj_vuplex_pointer(view,"pointerdown",50,40,1);pump(.1);
    klj_vuplex_pointer(view,"pointerup",50,40,1);
    for(int i=0;i<50&&!context;i++)pump(.1);assert(context==1&&clicked==1);
    click[1]=50;click[2]=140;
    call(NULL,big,"PointerDown","(IIIIII)V",click,6,'V');pump(.1);
    call(NULL,big,"PointerUp","(IIIIII)V",click,6,'V');
    for(int i=0;i<50&&!focused;i++)pump(.1);assert(focused>0);
    call(view,web,"destroy","()V",NULL,0,'V');pump(.2);[window close];
    puts("Bigscreen input: original JNI click constants activate a real DOM button and focus a text field");
}return 0;}
