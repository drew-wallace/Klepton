// System WebKit adapter. No guest JNI or GL calls may run on the main thread.
#import <WebKit/WebKit.h>
#import <ImageIO/ImageIO.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#include <pthread.h>
#include <math.h>
#include "kl_webview.h"
#include "kl_env.h"
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
// Cloud-player URLs carry room identifiers and may contain signed queries.
// Keep normal navigation diagnostics useful without recording those values.
static NSString *diagnosticOrigin(NSURL *url) {
    if(url.isFileURL)return @"file://";
    if(!url.scheme.length)return @"invalid URL";
    return url.host.length?[NSString stringWithFormat:@"%@://%@",url.scheme,url.host]:[url.scheme stringByAppendingString:@":"];
}
// Main-thread cookie operations precede explicit navigations across all views.
static unsigned pendingCookieWrites;
static NSMutableArray *cookieLoads;
static void afterCookies(void (^load)(void)) {
    if(!pendingCookieWrites){load();return;}
    if(!cookieLoads)cookieLoads=[NSMutableArray new];
    id retained=[load copy];[cookieLoads addObject:retained];KL_RELEASE(retained);
}
static void cookieWriteFinished(void) {
    if(--pendingCookieWrites)return;
    NSArray *loads=[cookieLoads copy];[cookieLoads removeAllObjects];
    for(void (^load)(void) in loads)afterCookies(load);KL_RELEASE(loads);
}
void kl_webview_set_cookie(const char *source) {
    @autoreleasepool {
        if(!source||strnlen(source,65537)>65536)return;
        NSString *text=string(source);
        if(!text)return;
        dispatch_async(dispatch_get_main_queue(), ^{
            id object=[NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            if(![object isKindOfClass:NSDictionary.class])return;
            NSDictionary *fields=object;
            NSString *name=fields[@"Name"],*value=fields[@"Value"],*domain=fields[@"Domain"],*path=fields[@"Path"];
            if(![name isKindOfClass:NSString.class]||!name.length||
               ![value isKindOfClass:NSString.class]||![domain isKindOfClass:NSString.class]||!domain.length||
               ![path isKindOfClass:NSString.class]||![path hasPrefix:@"/"])return;
            NSCharacterSet *invalid=[NSCharacterSet characterSetWithCharactersInString:@"\r\n;"];
            for(NSString *part in @[name,value,domain,path])if([part rangeOfCharacterFromSet:invalid].location!=NSNotFound)return;
            if([name rangeOfString:@"="].location!=NSNotFound)return;
            NSString *host=[domain hasPrefix:@"."]?[domain substringFromIndex:1]:domain;
            NSURLComponents *origin=[NSURLComponents new];origin.scheme=@"https";origin.host=host;origin.path=path;
            NSURL *url=origin.URL;KL_RELEASE(origin);if(!url.host.length)return;
            NSMutableString *header=[NSMutableString stringWithFormat:@"%@=%@; Domain=%@; Path=%@",name,value,domain,path];
            if([fields[@"Secure"] isKindOfClass:NSNumber.class]&&[fields[@"Secure"] boolValue])[header appendString:@"; Secure"];
            if([fields[@"HttpOnly"] isKindOfClass:NSNumber.class]&&[fields[@"HttpOnly"] boolValue])[header appendString:@"; HttpOnly"];
            NSNumber *expiration=fields[@"ExpirationDate"];
            if([expiration isKindOfClass:NSNumber.class]&&expiration.doubleValue!=0){
                if(!isfinite(expiration.doubleValue))return;
                NSDateFormatter *date=[NSDateFormatter new];
                NSLocale *locale=[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
                date.locale=locale;KL_RELEASE(locale);
                date.timeZone=[NSTimeZone timeZoneForSecondsFromGMT:0];date.dateFormat=@"EEE, dd MMM yyyy HH:mm:ss 'GMT'";
                [header appendFormat:@"; Expires=%@",[date stringFromDate:[NSDate dateWithTimeIntervalSince1970:expiration.doubleValue]]];
                KL_RELEASE(date);
            }
            NSHTTPCookie *cookie=[NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie":header} forURL:url].firstObject;
            if(!cookie)return;
            afterCookies(^{
                pendingCookieWrites++;
                WKHTTPCookieStore *store=WKWebsiteDataStore.defaultDataStore.httpCookieStore;
                if(cookie.expiresDate&&cookie.expiresDate.timeIntervalSinceNow<=0)
                    [store deleteCookie:cookie completionHandler:^{cookieWriteFinished();}];
                else [store setCookie:cookie completionHandler:^{cookieWriteFinished();}];
            });
        });
    }
}
void kl_webview_clear_data(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        afterCookies(^{
            pendingCookieWrites++;
            [WKWebsiteDataStore.defaultDataStore removeDataOfTypes:WKWebsiteDataStore.allWebsiteDataTypes
                modifiedSince:NSDate.distantPast completionHandler:^{cookieWriteFinished();}];
        });
    });
}
static NSURL *assetURL(NSString *text,NSString *root) {
    NSString *relative=nil;
    if([text hasPrefix:@"file:///android_asset/"])relative=[text substringFromIndex:@"file:///android_asset/".length];
    else if([text hasPrefix:@"jar:file:"]){NSRange marker=[text rangeOfString:@"!/assets/"];if(marker.location!=NSNotFound)relative=[text substringFromIndex:NSMaxRange(marker)];}
    if(!relative||!root.length)return nil;
    NSURLComponents *parts=[NSURLComponents componentsWithString:relative];
    NSString *path=parts.percentEncodedPath.stringByRemovingPercentEncoding;
    if(!path.length||[path hasPrefix:@"/"])return nil;
    NSURL *base=[NSURL fileURLWithPath:root isDirectory:YES].URLByStandardizingPath;
    NSURL *file=[base URLByAppendingPathComponent:path].URLByStandardizingPath;
    if(![file.path hasPrefix:[base.path stringByAppendingString:@"/"]])return nil;
    NSURLComponents *mapped=[NSURLComponents componentsWithURL:file resolvingAgainstBaseURL:NO];
    mapped.percentEncodedQuery=parts.percentEncodedQuery;mapped.percentEncodedFragment=parts.percentEncodedFragment;
    return mapped.URL;
}
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
// Open shadow roots are used by embedded cloud-browser players. Bound both
// discovery and hit testing so capture/input cannot walk an unbounded tree.
"window.__kleptonDeepElements=tag=>{const found=[];let visited=0;const walk=(root,depth)=>{"
"if(depth>16||visited>=8192)return;const w=document.createTreeWalker(root,NodeFilter.SHOW_ELEMENT);"
"for(let e=w.nextNode();e&&visited<8192;e=w.nextNode()){visited++;if(e.localName===tag)found.push(e);"
"if(e.shadowRoot)walk(e.shadowRoot,depth+1);}};walk(document,0);return found;};"
"window.__kleptonHitTest=(x,y)=>{let e=document.elementFromPoint(x,y);for(let i=0;i<16&&e&&e.shadowRoot;i++){"
"const next=e.shadowRoot.elementFromPoint?.(x,y);if(!next||next===e)break;e=next;}return e;};"
"window.__kleptonWebErrors=[0,0];window.addEventListener('error',()=>window.__kleptonWebErrors[0]++);"
"window.addEventListener('unhandledrejection',()=>window.__kleptonWebErrors[1]++);"
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

// WKWebView snapshots can omit hardware-backed HTML video (WebKit 280824).
// An opt-in canvas sibling paints ordinary inline video without extracting
// stream URLs or altering the player's audio, input, controls, or media state.
// Called only at our snapshot cadence; no extra animation/timer loop.
static NSString *const videoCanvasScript = @"(()=>{"
"const mirrors=new Map();window.__kleptonVideoCanvasStats=[0,0,0];"
"window.__kleptonVideoCanvas=()=>{let drawn=0,failed=0;const active=new Set();"
"for(const v of window.__kleptonDeepElements('video')){if(active.size>=4)break;"
"const r=v.getBoundingClientRect(),s=getComputedStyle(v);"
"if(v.controls||v.mediaKeys||v.readyState<2||!v.videoWidth||!v.clientWidth||!v.clientHeight||"
"s.display==='none'||s.visibility!=='visible'||s.position==='fixed'||"
"r.right<=0||r.bottom<=0||r.left>=innerWidth||r.top>=innerHeight)continue;"
"let c=mirrors.get(v);if(!c){c=document.createElement('canvas');c.setAttribute('aria-hidden','true');mirrors.set(v,c);}"
"active.add(v);if(v.nextSibling!==c)v.after(c);"
"c.style.cssText='position:absolute;pointer-events:none;';"
"Object.assign(c.style,{left:(v.offsetLeft+v.clientLeft)+'px',top:(v.offsetTop+v.clientTop)+'px',"
"width:v.clientWidth+'px',height:v.clientHeight+'px',zIndex:s.zIndex,opacity:s.opacity,"
"transform:s.transform,transformOrigin:s.transformOrigin,borderRadius:s.borderRadius});"
"const scale=Math.min(1,1280/v.clientWidth,720/v.clientHeight);"
"const w=Math.max(1,Math.round(v.clientWidth*scale)),h=Math.max(1,Math.round(v.clientHeight*scale));"
"if(c.width!==w||c.height!==h){c.width=w;c.height=h;}"
"try{const x=c.getContext('2d');x.clearRect(0,0,w,h);"
"let dw=w,dh=h;if(s.objectFit==='contain'||s.objectFit==='cover'||s.objectFit==='scale-down'){"
"let k=s.objectFit==='cover'?Math.max(w/v.videoWidth,h/v.videoHeight):Math.min(w/v.videoWidth,h/v.videoHeight);"
"if(s.objectFit==='scale-down')k=Math.min(k,scale);dw=v.videoWidth*k;dh=v.videoHeight*k;}"
"x.drawImage(v,(w-dw)/2,(h-dh)/2,dw,dh);drawn++;}catch(_){c.remove();mirrors.delete(v);failed++;}"
"}for(const [v,c] of mirrors){if(!active.has(v)||!v.isConnected){c.remove();mirrors.delete(v);}}"
"window.__kleptonVideoCanvasStats=[mirrors.size,drawn,failed];"
"return [...active].some(v=>mirrors.has(v)&&!v.paused&&!v.ended)?drawn:0;};"
// A cloud browser sends its whole UI as an ordinary WebRTC video. Native
// snapshots can be entirely transparent after the hosting window closes.
// Export only a full-viewport, origin-clean, unprotected MediaStream canvas;
// ordinary HTML player pages retain native capture, including their controls.
"let streamCanvas;window.__kleptonVideoStreamStatus=0;window.__kleptonVideoStreamFrame=()=>{"
"window.__kleptonVideoStreamStatus=0;for(const [v,c] of mirrors){"
"const r=v.getBoundingClientRect(),s=getComputedStyle(v);"
"const host=v.getRootNode().host,hr=host?.getBoundingClientRect();"
"window.__kleptonVideoStreamGeometry=[Math.round(r.width),Math.round(r.height),Math.round(hr?.width||0),Math.round(hr?.height||0)];"
"let reason=0;if(typeof MediaStream==='undefined'||!(v.srcObject instanceof MediaStream))reason=4;"
"else if(v.paused||v.ended)reason=5;else if(v.mediaKeys||v.controls)reason=6;else if(s.opacity!=='1')reason=7;"
"else if(s.transform!=='none'){try{const m=new DOMMatrixReadOnly(s.transform);if(!m.is2D||m.b!==0||m.c!==0||m.a<=0||m.d<=0)reason=8;}catch(_){reason=8;}}"
// Cloud players commonly center a letterboxed video with CSS translation
// inside a full-page shadow host. Its screen bounds already include that
// translation and positive scaling; preserve black bars around the video.
"if(!reason&&(!(r.width*r.height>=innerWidth*innerHeight*.9||hr&&hr.width*hr.height>=innerWidth*innerHeight*.9)||innerWidth<1||innerHeight<1))reason=9;"
"if(reason){window.__kleptonVideoStreamStatus=reason;continue;}"
"if(!streamCanvas)streamCanvas=document.createElement('canvas');"
"const scale=Math.min(1,1280/innerWidth,720/innerHeight),w=Math.max(1,Math.round(innerWidth*scale)),h=Math.max(1,Math.round(innerHeight*scale));"
"if(streamCanvas.width!==w||streamCanvas.height!==h){streamCanvas.width=w;streamCanvas.height=h;}"
"try{const x=streamCanvas.getContext('2d',{alpha:false});x.fillStyle='black';x.fillRect(0,0,w,h);"
"x.drawImage(c,r.left*scale,r.top*scale,r.width*scale,r.height*scale);"
"const frame=streamCanvas.toDataURL('image/jpeg',.85);"
"if(frame.length>4194304){window.__kleptonVideoStreamStatus=3;return null;}"
"window.__kleptonVideoStreamStatus=1;return frame;"
"}catch(_){window.__kleptonVideoStreamStatus=2;return null;}}return null;};})();";

static CGImageRef videoStreamImage(id frame) {
    NSString *prefix=@"data:image/jpeg;base64,";
    if(![frame isKindOfClass:NSString.class]||![frame hasPrefix:prefix]||[frame length]>4194304)return NULL;
    NSData *data=[[NSData alloc] initWithBase64EncodedString:[frame substringFromIndex:prefix.length] options:0];
    if(!data.length){KL_RELEASE(data);return NULL;}
    CGImageSourceRef source=CGImageSourceCreateWithData((__bridge CFDataRef)data,(__bridge CFDictionaryRef)@{(id)kCGImageSourceShouldCache:@NO});
    KL_RELEASE(data);if(!source)return NULL;
    CFDictionaryRef properties=CGImageSourceCopyPropertiesAtIndex(source,0,NULL);
    int64_t width=0,height=0;
    if(properties){
        CFTypeRef w=CFDictionaryGetValue(properties,kCGImagePropertyPixelWidth),h=CFDictionaryGetValue(properties,kCGImagePropertyPixelHeight);
        if(w&&CFGetTypeID(w)==CFNumberGetTypeID())CFNumberGetValue(w,kCFNumberSInt64Type,&width);
        if(h&&CFGetTypeID(h)==CFNumberGetTypeID())CFNumberGetValue(h,kCFNumberSInt64Type,&height);
        CFRelease(properties);
    }
    CGImageRef image=width>0&&width<=1280&&height>0&&height<=720?CGImageSourceCreateImageAtIndex(source,0,NULL):NULL;
    CFRelease(source);return image;
}

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
    unsigned traceSamples;
    unsigned traceNavigations;
    uint64_t traceBaseSerial;
    BOOL videoCanvas;
    BOOL videoPlaying;
    unsigned videoFPS;
    unsigned videoCanvasFailures;
    unsigned videoStreamFrames,videoStreamFailures,transparentVideoFrames;
    double snapshotStarted, performanceStarted, performanceMillis;
    unsigned performanceFrames, performanceSamples;
}
@property(nonatomic,strong) WKWebView *web;
@property(nonatomic,strong) NSTimer *timer;
- (void)emit:(NSString *)event value:(NSString *)value request:(uint64_t)request;
- (void)snapshot;
- (void)scheduleSnapshot;
- (void)captureSnapshot;
- (void)consumeSnapshot:(id)image error:(NSError *)error;
- (void)consumeCGImage:(CGImageRef)image;
- (BOOL)hiddenHost;
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
    pthread_mutex_lock(&lock);
    traceSamples=0;traceNavigations++;traceBaseSerial=serial;
    pthread_mutex_unlock(&lock);
    videoPlaying=NO;performanceStarted=0;performanceMillis=0;performanceFrames=0;performanceSamples=0;
    videoStreamFrames=0;videoStreamFailures=0;transparentVideoFrames=0;
    [self emit:@"HandleLoadStarted" value:@"" request:0];
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationResponse:(WKNavigationResponse *)response decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler {
    if(kl_env_on("KL_WEBVIEW_TRACE",0)&&traceNavigations<=8&&response.forMainFrame&&
       [response.response isKindOfClass:NSHTTPURLResponse.class])
        fprintf(stderr,"[webview] navigation response view=%llu status=%ld\n",
                (unsigned long long)identity,(long)((NSHTTPURLResponse*)response.response).statusCode);
    decisionHandler(WKNavigationResponsePolicyAllow);
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
    fprintf(stderr,"[webview] navigation failed code=%ld origin=%s\n",(long)error.code,diagnosticOrigin(failedURL?:self.web.URL).UTF8String);
    [self emit:@"HandleLoadFailed" value:[NSString stringWithFormat:@"%ld,%@",(long)error.code,url] request:0];
}
- (void)webView:(WKWebView *)web didFailProvisionalNavigation:(WKNavigation *)nav withError:(NSError *)error { [self failed:error]; }
- (void)webView:(WKWebView *)web didFailNavigation:(WKNavigation *)nav withError:(NSError *)error { [self failed:error]; }
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web {
    fprintf(stderr,"[webview] WebKit content process terminated\n");
    [self emit:@"HandleLoadFailed" value:[@"-1," stringByAppendingString:web.URL.absoluteString ?: @""] request:0];
}
- (void)scheduleSnapshot {
    [self.timer invalidate];self.timer=nil;
    if(closed||paused||!rendering||snapshotPending||(!self.web.window&&!videoCanvas))return;
    // Schedule from the previous start, not completion: a slow snapshot applies
    // backpressure without adding another full frame interval or queuing work.
    double interval=videoPlaying?1.0/videoFPS:0.1;
    double delay=fmax(.001,interval-(NSProcessInfo.processInfo.systemUptime-snapshotStarted));
    self.timer=[NSTimer scheduledTimerWithTimeInterval:delay repeats:NO block:^(NSTimer *timer){[self snapshot];}];
}
- (BOOL)hiddenHost {
#if TARGET_OS_OSX
    return !self.web.window||!self.web.window.visible;
#else
    UIWindow *window=self.web.window;
    return !window||window.hidden||!window.windowScene||window.windowScene.activationState!=UISceneActivationStateForegroundActive;
#endif
}
- (void)snapshot {
    if (closed || paused || !rendering || snapshotPending || (!self.web.window&&!videoCanvas)) return;
    [self.timer invalidate];self.timer=nil;
    snapshotPending = YES;
    snapshotStarted=NSProcessInfo.processInfo.systemUptime;
    if(!performanceStarted)performanceStarted=snapshotStarted;
    if (videoCanvas) {
        BOOL hiddenHost=[self hiddenHost];
        NSString *script=[NSString stringWithFormat:@"(()=>{const playing=window.__kleptonVideoCanvas?window.__kleptonVideoCanvas():0;const frame=%@&&playing&&window.__kleptonVideoStreamFrame?window.__kleptonVideoStreamFrame():null;return {playing,frame,status:window.__kleptonVideoStreamStatus||0,geometry:window.__kleptonVideoStreamGeometry}})()",hiddenHost?@"true":@"false"];
        [self.web evaluateJavaScript:script completionHandler:^(id result,NSError *error) {
            if(self->closed||self->paused||!self->rendering){self->snapshotPending=NO;[self scheduleSnapshot];return;}
            NSDictionary *state=[result isKindOfClass:NSDictionary.class]?result:nil;
            self->videoPlaying=!error&&[state[@"playing"] isKindOfClass:NSNumber.class]&&[state[@"playing"] integerValue]>0;
            if(error && ++self->videoCanvasFailures<=3)
                fprintf(stderr,"[webview] video canvas preparation failed code=%ld\n",(long)error.code);
            CGImageRef frame=hiddenHost&&self->videoPlaying?videoStreamImage(state[@"frame"]):NULL;
            if(frame){
                if(++self->videoStreamFrames==1)
                    fprintf(stderr,"[webview] hidden-host stream capture view=%llu %zux%zu\n",(unsigned long long)self->identity,CGImageGetWidth(frame),CGImageGetHeight(frame));
                [self consumeCGImage:frame];CGImageRelease(frame);
                self->snapshotPending=NO;[self scheduleSnapshot];return;
            }
            if(hiddenHost&&self->videoPlaying&&++self->videoStreamFailures<=3){
                NSArray *geometry=[state[@"geometry"] isKindOfClass:NSArray.class]?state[@"geometry"]:nil;
                long bounds[4]={0};for(unsigned i=0;i<4&&i<geometry.count;i++)if([geometry[i] isKindOfClass:NSNumber.class])bounds[i]=[geometry[i] longValue];
                fprintf(stderr,"[webview] hidden-host stream unavailable view=%llu category=%ld video=%ldx%ld host=%ldx%ld\n",(unsigned long long)self->identity,[state[@"status"] isKindOfClass:NSNumber.class]?[state[@"status"] longValue]:-1L,bounds[0],bounds[1],bounds[2],bounds[3]);
                [self.web requestMediaPlaybackStateWithCompletionHandler:^(WKMediaPlaybackState playback){
                    if(!self->closed)fprintf(stderr,"[webview] hidden-host media state view=%llu state=%ld\n",(unsigned long long)self->identity,(long)playback);
                }];
            }
            [self captureSnapshot];
        }];
    } else [self captureSnapshot];
}
- (void)captureSnapshot {
    if(closed||paused||!rendering||!self.web.window){snapshotPending=NO;[self scheduleSnapshot];return;}
    WKSnapshotConfiguration *config = [[WKSnapshotConfiguration alloc] init];
    config.snapshotWidth = @(self.web.bounds.size.width);
    // The host may be offscreen or closed. Do not wait indefinitely for its
    // next onscreen update; our video canvas has already been prepared.
    config.afterScreenUpdates=NO;
    [self.web takeSnapshotWithConfiguration:config completionHandler:^(id image, NSError *error) {
        [self consumeSnapshot:image error:error];
        self->snapshotPending = NO;
        [self scheduleSnapshot];
    }];
    KL_RELEASE(config);
}
- (void)consumeSnapshot:(id)image error:(NSError *)error {
    if (closed || paused || !rendering) return;
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
    [self consumeCGImage:cg];
}
- (void)consumeCGImage:(CGImageRef)cg {
    if(closed||paused||!rendering||!cg)return;
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
    // An invisible WK host can return a successful, completely transparent
    // snapshot while its video is still playing. Keep the last usable frame
    // instead of publishing that empty surface into the theater's lighting.
    if(videoPlaying&&[self hiddenHost]){
        const uint8_t *bytes=data;BOOL opaquePixel=NO;
        for(size_t i=3;i<(size_t)w*h*4;i+=4)if(bytes[i]){opaquePixel=YES;break;}
        if(!opaquePixel){
            if(++transparentVideoFrames==1)fprintf(stderr,"[webview] discarded transparent hidden-video snapshot view=%llu\n",(unsigned long long)identity);
            free(data);return;
        }
    }
    if (!self->serial) fprintf(stderr,"[webview] first snapshot %llu %dx%d\n",(unsigned long long)self->identity,w,h);
    pthread_mutex_lock(&self->lock);
    free(self->pixels); self->pixels=data; self->pixelWidth=w; self->pixelHeight=h; self->serial++;
    pthread_mutex_unlock(&self->lock);
    if(kl_env_on("KL_WEBVIEW_TRACE",0)&&traceNavigations<=8&&performanceSamples<3){
        double now=NSProcessInfo.processInfo.systemUptime;
        if(!performanceStarted)performanceStarted=snapshotStarted;
        performanceFrames++;performanceMillis+=(now-snapshotStarted)*1000;
        if(now-performanceStarted>=5){
            fprintf(stderr,"[webview] capture cadence view=%llu target=%u fps=%.1f mean_ms=%.1f frames=%u\n",
                    (unsigned long long)identity,videoPlaying?videoFPS:10,
                    performanceFrames/(now-performanceStarted),performanceMillis/performanceFrames,performanceFrames);
            performanceSamples++;performanceStarted=now;performanceFrames=0;performanceMillis=0;
        }
    }
}
@end

void *kl_webview_create(uint64_t id, int width, int height, const char *agent, int autoplay, kl_webview_event callback) {
    @autoreleasepool {
        if (width < 1 || height < 1 || width > 4096 || height > 4096) return NULL;
        KLBrowser *browser = [[KLBrowser alloc] init];
        browser->identity=id; browser->callback=callback;
        browser->videoCanvas=kl_env_on("KL_WEBVIEW_VIDEO_CANVAS",0);
        int fps=kl_env_int("KL_WEBVIEW_VIDEO_FPS",30);
        browser->videoFPS=(unsigned)(fps<10?10:fps>60?60:fps);
        NSString *ua = string(agent);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (browser->closed) return;
            WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
            config.websiteDataStore=WKWebsiteDataStore.defaultDataStore;
    #if !TARGET_OS_OSX
            config.allowsInlineMediaPlayback = YES;
    #endif
            config.mediaTypesRequiringUserActionForPlayback = autoplay ? WKAudiovisualMediaTypeNone : WKAudiovisualMediaTypeAll;
            [config.userContentController addScriptMessageHandler:browser name:@"klepton"];
            WKUserScript *script = [[WKUserScript alloc] initWithSource:bridgeScript injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO];
            [config.userContentController addUserScript:script]; KL_RELEASE(script);
            if(browser->videoCanvas){
                WKUserScript *videoScript=[[WKUserScript alloc] initWithSource:videoCanvasScript injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES];
                [config.userContentController addUserScript:videoScript];KL_RELEASE(videoScript);
            }
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
            [browser scheduleSnapshot];
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
          afterCookies(^{
            if (browser->closed) return;
            if (html) [browser.web loadHTMLString:text baseURL:[NSURL URLWithString:baseURL]];
            else {
                NSURL *url=[NSURL URLWithString:text];
                BOOL asset=[text hasPrefix:@"file:///android_asset/"]||[text hasPrefix:@"jar:file:"];
                if(asset)url=assetURL(text,baseURL);
                fprintf(stderr,"[webview] load origin=%s\n",diagnosticOrigin(url).UTF8String);
                if (url.isFileURL) [browser.web loadFileURL:url allowingReadAccessToURL:asset?[NSURL fileURLWithPath:baseURL isDirectory:YES]:url.URLByDeletingLastPathComponent];
                else if (url) [browser.web loadRequest:[NSURLRequest requestWithURL:url]];
                else [browser emit:@"HandleLoadFailed" value:[@"-1000," stringByAppendingString:text] request:0];
            }
          });
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
            else if ([cmd isEqual:@"rendering"]) {
                browser->rendering=!!value;
                browser->performanceStarted=0;browser->performanceFrames=0;browser->performanceMillis=0;
                [browser scheduleSnapshot];
            }
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
                browser->performanceStarted=0;browser->performanceFrames=0;browser->performanceMillis=0;
                [browser.web setAllMediaPlaybackSuspended:browser->paused completionHandler:nil];
                [browser scheduleSnapshot];
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
        NSString *source=[NSString stringWithFormat:@"(()=>{const e=(window.__kleptonHitTest||document.elementFromPoint.bind(document))(%d,%d),kind=%@,button=%d;if(!e)return;const state=window.__kleptonPointer||(window.__kleptonPointer={target:null,buttons:0});if(kind==='pointerdown'){state.target=e;state.buttons=button===0?1:button===2?2:4;e.focus();}if(kind==='pointerup'||kind==='pointercancel')state.buttons=0;const o={bubbles:true,composed:true,cancelable:true,clientX:%d,clientY:%d,button,buttons:state.buttons,pointerId:1,pointerType:'mouse'};e.dispatchEvent(new PointerEvent(kind,o));e.dispatchEvent(new MouseEvent(kind.replace('pointer','mouse'),o));if(kind==='pointerup'){if(state.target===e){if(button===0)e.click();else if(button===2)e.dispatchEvent(new MouseEvent('contextmenu',o));}state.target=null;}if(kind==='pointercancel')state.target=null;})();",x,y,json(string(type)),button,x,y];
        kl_webview_eval(view,source.UTF8String,0);
    }
}
void kl_webview_scroll(void *view,int dx,int dy,int x,int y) {
    @autoreleasepool {
        NSString *source=[NSString stringWithFormat:@"(()=>{let e=(window.__kleptonHitTest||document.elementFromPoint.bind(document))(%d,%d);while(e&&e!==document.documentElement){if(e.scrollHeight>e.clientHeight||e.scrollWidth>e.clientWidth){e.scrollBy(%d,%d);return;}e=e.parentElement||e.getRootNode().host;}window.scrollBy(%d,%d);})();",x,y,dx,dy,dx,dy];
        kl_webview_eval(view,source.UTF8String,0);
    }
}
void kl_webview_key(void *view,const char *key) {
    @autoreleasepool {
        NSString *source=[NSString stringWithFormat:@"(()=>{let e=document.activeElement;const k=%@;while(e&&e.shadowRoot&&e.shadowRoot.activeElement)e=e.shadowRoot.activeElement;if(!e)return;e.dispatchEvent(new KeyboardEvent('keydown',{key:k,bubbles:true,composed:true}));if('value'in e){if(k==='BACKSPACE'||k==='Backspace')e.value=e.value.slice(0,-1);else if(k.length===1)e.value+=k;e.dispatchEvent(new Event('input',{bubbles:true,composed:true}));}e.dispatchEvent(new KeyboardEvent('keyup',{key:k,bubbles:true,composed:true}));})();",json(string(key))];
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
        static const unsigned thresholds[]={1,10,50,100,600,1800};
        unsigned sample=0;
        if(copy&&browser->traceNavigations<=8&&browser->traceSamples<6&&kl_env_on("KL_WEBVIEW_TRACE",0)&&
           *serial-browser->traceBaseSerial>=thresholds[browser->traceSamples])sample=++browser->traceSamples;
        pthread_mutex_unlock(&browser->lock);
        if(sample){
            // Bounded CPU-buffer statistics only: no page text, URLs, audio,
            // images on disk, GL readbacks, or debugger pauses.
            const unsigned char *p=copy;unsigned white=0,black=0,transparent=0,varied=0;
            unsigned char first[4];memcpy(first,p,4);
            for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
                const unsigned char *c=p+4*((size_t)((*h)*(2*y+1)/16)*(*w)+(*w)*(2*x+1)/16);
                white+=c[0]>=248&&c[1]>=248&&c[2]>=248;
                black+=c[0]<8&&c[1]<8&&c[2]<8;
                transparent+=c[3]<8;varied+=memcmp(c,first,4)!=0;
            }
            fprintf(stderr,"[webview] snapshot stats view=%llu serial=%llu %dx%d white=%u/64 black=%u/64 transparent=%u/64 varied=%u/64\n",
                    (unsigned long long)browser->identity,(unsigned long long)*serial,*w,*h,white,black,transparent,varied);
            if(sample>=3)dispatch_async(dispatch_get_main_queue(), ^{
                if(browser->closed)return;
                [browser.web evaluateJavaScript:@"(()=>{const find=window.__kleptonDeepElements||((tag)=>[...document.querySelectorAll(tag)]),videos=find('video'),v=videos[0],error=document.getElementById('errorDiv'),msg=error?.textContent||'';const category=!error?0:/auth|token|login/i.test(msg)?1:/timeout|timed.out/i.test(msg)?2:/network|connect|offline/i.test(msg)?3:/support|not.implemented/i.test(msg)?4:5;return [document.readyState==='complete'?1:0,document.hidden?1:0,document.body?document.body.children.length:0,find('canvas').length,videos.length,innerWidth,innerHeight,v?(v.paused?1:0):-1,v?v.readyState:-1,v&&v.error?v.error.code:0,v?v.videoWidth:0,v?v.videoHeight:0,v&&v.mediaKeys?1:0,...(window.__kleptonVideoCanvasStats||[0,0,0]),...(window.__kleptonWebErrors||[0,0]),category,find('iframe').length]})()" completionHandler:^(id result,NSError *error){
                    if(browser->closed)return;
                    if(error){fprintf(stderr,"[webview] DOM stats view=%llu error=%ld\n",(unsigned long long)browser->identity,(long)error.code);return;}
                    if(![result isKindOfClass:NSArray.class]||[result count]!=20)return;
                    long values[20];for(unsigned i=0;i<20;i++){id n=result[i];values[i]=[n isKindOfClass:NSNumber.class]?[n longValue]:-1;}
                    fprintf(stderr,"[webview] DOM stats view=%llu complete=%ld hidden=%ld children=%ld canvases=%ld videos=%ld viewport=%ldx%ld\n",
                            (unsigned long long)browser->identity,values[0],values[1],values[2],values[3],values[4],values[5],values[6]);
                    fprintf(stderr,"[webview] video stats view=%llu paused=%ld ready=%ld error=%ld size=%ldx%ld protected=%ld canvas=%ld drawn=%ld failed=%ld\n",
                            (unsigned long long)browser->identity,values[7],values[8],values[9],values[10],values[11],values[12],values[13],values[14],values[15]);
                    fprintf(stderr,"[webview] page errors view=%llu script=%ld rejection=%ld category=%ld frames=%ld\n",(unsigned long long)browser->identity,values[16],values[17],values[18],values[19]);
                }];
            });
        }
        return copy;
    }
}
