// System WebKit adapter. No guest JNI or GL calls may run on the main thread.
#import <WebKit/WebKit.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#include <pthread.h>
#include "kl_webview.h"
#if __has_feature(objc_arc)
#define KL_RELEASE(x) ((void)0)
#define KL_HANDLE(x) ((__bridge_retained void *)(x))
#define KL_OBJECT(x) ((__bridge KLBrowser *)(x))
#else
#define KL_RELEASE(x) [(x) release]
#define KL_HANDLE(x) ((void *)(x))
#define KL_OBJECT(x) ((KLBrowser *)(x))
#endif

static NSString *string(const char *s) { return s ? [NSString stringWithUTF8String:s] : @""; }
static NSString *json(id value) {
    if (!value || value == [NSNull null]) return @"null";
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:nil];
    if (!data) return @"null";
    NSString *result = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
#if !__has_feature(objc_arc)
    return [result autorelease];
#else
    return result;
#endif
}
// Deliberately our own small implementation of the public page message API.
// Existing Vuplex page scripts can also use the Android-style bridge object.
static NSString *const bridgeScript = @"(()=>{"
"const send=x=>window.webkit.messageHandlers.klepton.postMessage(typeof x==='string'?x:JSON.stringify(x));"
"window._vuplexBridge={postMessage:send};"
"if(!window.vuplex){const t=new EventTarget();window.vuplex={"
"addEventListener:(...a)=>t.addEventListener(...a),removeEventListener:(...a)=>t.removeEventListener(...a),"
"postMessage:send,_emit:(name,event)=>t.dispatchEvent(new MessageEvent(name,{data:event&&event.data!==undefined?event.data:event}))};}"
"if(window===window.top){let previous=null;window.__kleptonNotifyFocus=force=>{"
"let e=document.activeElement;while(e&&e.shadowRoot&&e.shadowRoot.activeElement)e=e.shadowRoot.activeElement;"
"if(e===previous&&!force)return;previous=e;const tag=e&&e.tagName;"
"const editable=e&&!e.disabled&&!e.readOnly&&(e.isContentEditable||tag==='TEXTAREA'||"
"(tag==='INPUT'&&['text','search','email','url','tel','password','number'].includes(e.type)));"
"send({type:'vuplex.webview.focusedInputFieldChanged',value:tag==='IFRAME'?'IFRAME':editable?'TEXT':'NONE'});};"
"document.addEventListener('focusin',()=>window.__kleptonNotifyFocus(false));"
"document.addEventListener('focusout',()=>queueMicrotask(()=>window.__kleptonNotifyFocus(false)));"
"document.addEventListener('click',()=>window.__kleptonNotifyFocus(true));"
"document.addEventListener('DOMContentLoaded',()=>window.__kleptonNotifyFocus(false));"
"const urlChanged=type=>send({type:'vuplex.webview.urlChanged',urlAction:{Url:document.URL,Title:document.title,Type:type}});"
"for(const [method,type] of [['pushState','PUSH_STATE'],['replaceState','REPLACE_STATE']]){"
"const original=history[method];history[method]=function(...args){const result=Reflect.apply(original,this,args);urlChanged(type);return result;};}"
"window.addEventListener('hashchange',()=>urlChanged('HASH_CHANGE'));"
"window.addEventListener('popstate',()=>urlChanged('LOAD'));"
"let previousTitle=document.title;new MutationObserver(()=>{if(document.title!==previousTitle){"
"previousTitle=document.title;send({type:'vuplex.webview.titleChanged',value:previousTitle});}})"
".observe(document,{subtree:true,childList:true,characterData:true});}"
"window.dispatchEvent(new Event('vuplexready'));})();";

@interface KLBrowser : NSObject <WKNavigationDelegate, WKScriptMessageHandler> {
@public
    pthread_mutex_t lock;
    void *pixels;
    int pixelWidth, pixelHeight;
    uint64_t serial, identity;
    kl_webview_event callback;
    BOOL closed, snapshotPending, rendering, paused, focusedInputFieldEvents;
    int fullscreen;
    WKContentMode contentMode;
    unsigned snapshotFailures;
}
@property(nonatomic,strong) WKWebView *web;
@property(nonatomic,strong) NSTimer *timer;
- (void)emit:(NSString *)event value:(NSString *)value request:(uint64_t)request;
- (void)snapshot;
@end
@implementation KLBrowser
- (instancetype)init {
    if ((self = [super init])) { pthread_mutex_init(&lock, NULL); rendering = YES; }
    return self;
}
- (void)dealloc {
    free(pixels);
    pthread_mutex_destroy(&lock);
#if !__has_feature(objc_arc)
    [_web release]; [_timer release]; [super dealloc];
#endif
}
- (void)emit:(NSString *)event value:(NSString *)value request:(uint64_t)request {
    if (!closed && callback) callback(identity, event.UTF8String, (value ?: @"").UTF8String, request);
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    NSString *value=[message.body isKindOfClass:NSString.class] ? message.body : json(message.body);
    if (!focusedInputFieldEvents) {
        id object=[NSJSONSerialization JSONObjectWithData:[value dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingFragmentsAllowed error:nil];
        if ([object isKindOfClass:NSDictionary.class] && [object[@"type"] isEqual:@"vuplex.webview.focusedInputFieldChanged"]) return;
    }
    [self emit:@"HandleMessageEmitted" value:value request:0];
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (object==self.web && [keyPath isEqual:@"fullscreenState"]) {
        __atomic_store_n(&fullscreen,self.web.fullscreenState!=WKFullscreenStateNotInFullscreen,__ATOMIC_RELEASE);
    } else [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action preferences:(WKWebpagePreferences *)preferences decisionHandler:(void (^)(WKNavigationActionPolicy, WKWebpagePreferences *))decisionHandler {
    preferences.preferredContentMode=contentMode;
    decisionHandler(WKNavigationActionPolicyAllow,preferences);
}
- (void)webView:(WKWebView *)web didStartProvisionalNavigation:(WKNavigation *)navigation {
    [self emit:@"HandleLoadStarted" value:@"" request:0];
}
- (void)webView:(WKWebView *)web didFinishNavigation:(WKNavigation *)navigation {
    [self emit:@"HandleMessageEmitted" value:json(@{@"type":@"vuplex.webview.urlChanged", @"urlAction":@{@"Type":@"LOAD",@"Url":web.URL.absoluteString ?: @"",@"Title":web.title ?: @""}}) request:0];
    [self emit:@"HandleMessageEmitted" value:json(@{@"type":@"vuplex.webview.titleChanged", @"value":web.title ?: @""}) request:0];
    [self emit:@"HandleLoadFinished" value:@"" request:0];
    [self snapshot];
}
- (void)failed:(NSError *)error {
    if (error.code == NSURLErrorCancelled) return;
    NSURL *failedURL = error.userInfo[NSURLErrorFailingURLErrorKey];
    NSString *url = failedURL.absoluteString ?: self.web.URL.absoluteString ?: @"";
    fprintf(stderr,"[webview] navigation failed %ld: %s (%s)\n",(long)error.code,error.localizedDescription.UTF8String,url.UTF8String);
    [self emit:@"HandleLoadFailed" value:[NSString stringWithFormat:@"%ld,%@",(long)error.code,url] request:0];
}
- (void)webView:(WKWebView *)web didFailProvisionalNavigation:(WKNavigation *)nav withError:(NSError *)error { [self failed:error]; }
- (void)webView:(WKWebView *)web didFailNavigation:(WKNavigation *)nav withError:(NSError *)error { [self failed:error]; }
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web {
    fprintf(stderr,"[webview] WebKit content process terminated\n");
    [self emit:@"HandleLoadFailed" value:[@"-1," stringByAppendingString:web.URL.absoluteString ?: @""] request:0];
}
- (void)snapshot {
    if (closed || paused || !rendering || snapshotPending || !self.web.window) return;
    snapshotPending = YES;
    WKSnapshotConfiguration *config = [[WKSnapshotConfiguration alloc] init];
    config.snapshotWidth = @(self.web.bounds.size.width);
    [self.web takeSnapshotWithConfiguration:config completionHandler:^(id image, NSError *error) {
        self->snapshotPending = NO;
        if (self->closed || self->paused || !self->rendering) return;
        if (!image) {
            // Initial about:blank snapshots can fail while WebKit starts.
            // Only a sustained failure on a loaded page is actionable.
            if (self.web.URL && ++self->snapshotFailures==30)
                fprintf(stderr,"[webview] snapshot failed: %s\n",error.localizedDescription.UTF8String);
            return;
        }
        self->snapshotFailures=0;
#if TARGET_OS_OSX
        CGImageRef cg = [(NSImage *)image CGImageForProposedRect:NULL context:nil hints:nil];
#else
        CGImageRef cg = [(UIImage *)image CGImage];
#endif
        if (!cg) return;
        // Snapshot scale can be 2x/3x. The game texture is specified in CSS
        // pixels, so rasterize into exactly the requested viewport dimensions.
        int w = (int)self.web.bounds.size.width, h = (int)self.web.bounds.size.height;
        if (w < 1 || h < 1 || w > 4096 || h > 4096) return;
        void *data = calloc((size_t)w*h, 4);
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = data ? CGBitmapContextCreate(data,w,h,8,w*4,color,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst) : NULL;
        CGColorSpaceRelease(color);
        if (!ctx) { free(data); return; }
        CGContextDrawImage(ctx, CGRectMake(0,0,w,h), cg);
        CGContextRelease(ctx);
        if (!self->serial) fprintf(stderr,"[webview] first snapshot %llu %dx%d\n",(unsigned long long)self->identity,w,h);
        pthread_mutex_lock(&self->lock);
        free(self->pixels); self->pixels=data; self->pixelWidth=w; self->pixelHeight=h; self->serial++;
        pthread_mutex_unlock(&self->lock);
    }];
    KL_RELEASE(config);
}
@end

void *kl_webview_create(uint64_t id, int width, int height, const char *agent, int autoplay, kl_webview_event callback) {
    @autoreleasepool {
        if (width < 1 || height < 1 || width > 4096 || height > 4096) return NULL;
        KLBrowser *browser = [[KLBrowser alloc] init];
        browser->identity=id; browser->callback=callback;
        NSString *ua = string(agent);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (browser->closed) return;
            WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
    #if !TARGET_OS_OSX
            config.allowsInlineMediaPlayback = YES;
    #endif
            config.mediaTypesRequiringUserActionForPlayback = autoplay ? WKAudiovisualMediaTypeNone : WKAudiovisualMediaTypeAll;
            [config.userContentController addScriptMessageHandler:browser name:@"klepton"];
            WKUserScript *script = [[WKUserScript alloc] initWithSource:bridgeScript injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO];
            [config.userContentController addUserScript:script]; KL_RELEASE(script);
            WKWebView *web = [[WKWebView alloc] initWithFrame:CGRectMake(-width-32,0,width,height) configuration:config];
            browser.web=web; KL_RELEASE(web); KL_RELEASE(config);
            [browser.web addObserver:browser forKeyPath:@"fullscreenState" options:NSKeyValueObservingOptionInitial|NSKeyValueObservingOptionNew context:NULL];
            browser.web.navigationDelegate=browser;
            if (ua.length) browser.web.customUserAgent=ua;
    #if TARGET_OS_OSX
            NSWindow *window = NSApp.keyWindow ?: NSApp.windows.firstObject;
            [window.contentView addSubview:browser.web positioned:NSWindowBelow relativeTo:nil];
    #else
            UIWindow *window = nil;
            for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
                if (![scene isKindOfClass:UIWindowScene.class]) continue;
                for (UIWindow *candidate in ((UIWindowScene *)scene).windows) {
                    if (!candidate.hidden && candidate.rootViewController) { window=candidate; break; }
                }
                if (window) break;
            }
            browser.web.userInteractionEnabled=NO;
            [window insertSubview:browser.web atIndex:0];
    #endif
            if (!browser.web.window) {
                fprintf(stderr,"[webview] no host window; cannot render browser textures\n");
                [browser emit:@"HandleLoadFailed" value:@"-1,no host window" request:0];
                return;
            }
            browser.timer=[NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer){ [browser snapshot]; }];
            fprintf(stderr,"[webview] WKWebView %llu initialized %dx%d\n",(unsigned long long)id,width,height);
            [browser emit:@"HandleInitFinished" value:@"" request:0];
        });
        void *handle = KL_HANDLE(browser);
    #if __has_feature(objc_arc)
        // bridge_retained owns the reference returned to C.
    #else
        // alloc already owns the reference returned to C.
    #endif
        return handle;
    }
}
void kl_webview_close(void *view) {
    @autoreleasepool {
        if (!view) return;
        KLBrowser *browser=KL_OBJECT(view);
        dispatch_async(dispatch_get_main_queue(), ^{
            browser->closed=YES; browser->callback=NULL;
            [browser.timer invalidate]; browser.timer=nil;
            [browser.web removeObserver:browser forKeyPath:@"fullscreenState"];
            [browser.web stopLoading]; browser.web.navigationDelegate=nil;
            [browser.web.configuration.userContentController removeScriptMessageHandlerForName:@"klepton"];
            [browser.web removeFromSuperview]; browser.web=nil;
    #if __has_feature(objc_arc)
            CFRelease(view);
    #else
            [browser release];
    #endif
        });
    }
}
void kl_webview_load(void *view, const char *content, const char *base, int html) {
    @autoreleasepool {
        if (!view) return;
        KLBrowser *browser=KL_OBJECT(view); NSString *text=string(content), *baseURL=string(base);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (browser->closed) return;
            if (html) [browser.web loadHTMLString:text baseURL:[NSURL URLWithString:baseURL]];
            else {
                NSURL *url=[NSURL URLWithString:text];
                if ([text hasPrefix:@"file:///android_asset/"]) {
                    NSString *relative = [url.path substringFromIndex:@"/android_asset/".length];
                    url=[NSURL fileURLWithPath:[baseURL stringByAppendingPathComponent:relative]];
                }
                fprintf(stderr,"[webview] load %s\n",url.absoluteString.UTF8String);
                if (url.isFileURL) [browser.web loadFileURL:url allowingReadAccessToURL:url.URLByDeletingLastPathComponent];
                else if (url) [browser.web loadRequest:[NSURLRequest requestWithURL:url]];
                else [browser emit:@"HandleLoadFailed" value:[@"-1000," stringByAppendingString:text] request:0];
            }
        });
    }
}
void kl_webview_eval(void *view, const char *script, uint64_t request) {
    @autoreleasepool {
        if (!view) return;
        KLBrowser *browser=KL_OBJECT(view); NSString *source=string(script);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (browser->closed) return;
            [browser.web evaluateJavaScript:source completionHandler:^(id result,NSError *error) {
                if (error) fprintf(stderr,"[webview] JavaScript: %s\n",error.localizedDescription.UTF8String);
                if (request) [browser emit:@"@callback" value:[result isKindOfClass:NSString.class] ? result : json(result) request:request];
            }];
        });
    }
}
void kl_webview_resize(void *view,int w,int h) {
    @autoreleasepool {
        if (!view || w<1 || h<1 || w>4096 || h>4096) return;
        KLBrowser *browser=KL_OBJECT(view);
        dispatch_async(dispatch_get_main_queue(), ^{ if (!browser->closed) { browser.web.frame=CGRectMake(-w-32,0,w,h); [browser snapshot]; } });
    }
}
void kl_webview_command(void *view,const char *command,int value) {
    @autoreleasepool {
        if (!view) return;
        KLBrowser *browser=KL_OBJECT(view); NSString *cmd=string(command);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (browser->closed) return;
            if ([cmd isEqual:@"reload"]) [browser.web reload];
            else if ([cmd isEqual:@"back"]) [browser.web goBack];
            else if ([cmd isEqual:@"forward"]) [browser.web goForward];
            else if ([cmd isEqual:@"exitFullscreen"])
                [browser.web closeAllMediaPresentationsWithCompletionHandler:nil];
            else if ([cmd isEqual:@"rendering"]) browser->rendering=!!value;
            else if ([cmd isEqual:@"focusedInputFieldEvents"]) {
                browser->focusedInputFieldEvents=!!value;
                if (value) [browser.web evaluateJavaScript:@"window.__kleptonNotifyFocus?.(true)" completionHandler:nil];
            }
            else if ([cmd isEqual:@"paused"] && browser->paused!=!!value) {
                // Vuplex pause/resume wraps Android WebView.onPause/onResume.
                // Keep the page and last texture alive, suspend its media, and
                // stop taking snapshots. Rendering policy is independent: a
                // resume must not undo setRenderingEnabled(false).
                browser->paused=!!value;
                [browser.web setAllMediaPlaybackSuspended:browser->paused completionHandler:nil];
                if (!browser->paused) [browser snapshot];
            }
            else if ([cmd isEqual:@"focus"]) {
    #if TARGET_OS_OSX
                if (value) [browser.web.window makeFirstResponder:browser.web];
    #else
                if (value) [browser.web becomeFirstResponder]; else [browser.web resignFirstResponder];
    #endif
            }
        });
    }
}
int kl_webview_is_fullscreen(void *view) {
    return view ? __atomic_load_n(&KL_OBJECT(view)->fullscreen,__ATOMIC_ACQUIRE) : 0;
}
void kl_webview_user_agent(void *view,const char *agent) {
    @autoreleasepool {
        if (!view) return;
        KLBrowser *browser=KL_OBJECT(view); NSString *ua=string(agent);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!browser->closed) {
                browser->contentMode=WKContentModeRecommended;
                browser.web.customUserAgent=ua.length?ua:nil;
            }
        });
    }
}
void kl_webview_user_agent_mode(void *view,int mobile) {
    @autoreleasepool {
        if (!view) return;
        KLBrowser *browser=KL_OBJECT(view);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (browser->closed) return;
            browser->contentMode=mobile?WKContentModeMobile:WKContentModeDesktop;
            browser.web.customUserAgent=nil;
        });
    }
}
void kl_webview_pointer(void *view,const char *type,int x,int y,int button) {
    @autoreleasepool {
        NSString *source=[NSString stringWithFormat:@"(()=>{const e=document.elementFromPoint(%d,%d),kind=%@,button=%d;if(!e)return;const state=window.__kleptonPointer||(window.__kleptonPointer={target:null,buttons:0});if(kind==='pointerdown'){state.target=e;state.buttons=button===0?1:button===2?2:4;e.focus();}if(kind==='pointerup'||kind==='pointercancel')state.buttons=0;const o={bubbles:true,cancelable:true,clientX:%d,clientY:%d,button,buttons:state.buttons,pointerId:1,pointerType:'mouse'};e.dispatchEvent(new PointerEvent(kind,o));e.dispatchEvent(new MouseEvent(kind.replace('pointer','mouse'),o));if(kind==='pointerup'){if(state.target===e){if(button===0)e.click();else if(button===2)e.dispatchEvent(new MouseEvent('contextmenu',o));}state.target=null;}if(kind==='pointercancel')state.target=null;})();",x,y,json(string(type)),button,x,y];
        kl_webview_eval(view,source.UTF8String,0);
    }
}
void kl_webview_scroll(void *view,int dx,int dy,int x,int y) {
    @autoreleasepool {
        NSString *source=[NSString stringWithFormat:@"(()=>{let e=document.elementFromPoint(%d,%d);while(e&&e!==document.documentElement){if(e.scrollHeight>e.clientHeight||e.scrollWidth>e.clientWidth){e.scrollBy(%d,%d);return;}e=e.parentElement;}window.scrollBy(%d,%d);})();",x,y,dx,dy,dx,dy];
        kl_webview_eval(view,source.UTF8String,0);
    }
}
void kl_webview_key(void *view,const char *key) {
    @autoreleasepool {
        NSString *source=[NSString stringWithFormat:@"(()=>{const e=document.activeElement,k=%@;if(!e)return;e.dispatchEvent(new KeyboardEvent('keydown',{key:k,bubbles:true}));if('value'in e){if(k==='BACKSPACE'||k==='Backspace')e.value=e.value.slice(0,-1);else if(k.length===1)e.value+=k;e.dispatchEvent(new Event('input',{bubbles:true}));}e.dispatchEvent(new KeyboardEvent('keyup',{key:k,bubbles:true}));})();",json(string(key))];
        kl_webview_eval(view,source.UTF8String,0);
    }
}
void *kl_webview_copy_frame(void *view,uint64_t *serial,int *w,int *h) {
    @autoreleasepool {
        if (!view) return NULL;
        KLBrowser *browser=KL_OBJECT(view); void *copy=NULL;
        pthread_mutex_lock(&browser->lock);
        if (browser->pixels && *serial!=browser->serial) {
            size_t size=(size_t)browser->pixelWidth*browser->pixelHeight*4;
            copy=malloc(size);
            if (copy) { memcpy(copy,browser->pixels,size); *serial=browser->serial; *w=browser->pixelWidth; *h=browser->pixelHeight; }
        }
        pthread_mutex_unlock(&browser->lock);
        return copy;
    }
}
