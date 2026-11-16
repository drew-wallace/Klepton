// Synthetic localhost cookies only. Check the real shared WebKit store and
// network requests, including writes made before any browser exists.
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "kl_webview.h"
static atomic_int stopping,requests,headers;
static int listener,finished,evaluated;static char result[512];
static WKWebsiteDataStore *fixtureStore;
static id fixtureDefaultStore(id cls,SEL selector){return fixtureStore;}
static void pump(double seconds){
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
static void event(uint64_t view,const char *name,const char *value,uint64_t request){
    if(!strcmp(name,"HandleLoadFinished"))finished++;
    if(request){evaluated++;snprintf(result,sizeof result,"%s",value);}
}
static void *serve(void *unused){
    while(!atomic_load(&stopping)){
        int fd=accept(listener,NULL,NULL);if(fd<0)break;
        char input[8192]={0};size_t used=0;
        while(used<sizeof input-1&&!strstr(input,"\r\n\r\n")){
            ssize_t n=read(fd,input+used,sizeof input-1-used);if(n<=0)break;used+=(size_t)n;
        }
        int bits=0;
        if(strstr(input,"kl_readable=alpha"))bits|=1;
        if(strstr(input,"kl_hidden=fixture"))bits|=2;
        if(strstr(input,"kl_readable=beta"))bits|=4;
        atomic_store(&headers,bits);atomic_fetch_add(&requests,1);
        const char *response="HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 20\r\nConnection: close\r\n\r\n<body>fixture</body>";
        write(fd,response,strlen(response));close(fd);
    }
    return NULL;
}
static void cookie(const char *name,const char *value,int hidden,int secure,long long expires){
    NSDictionary *fields=@{@"Name":@(name),@"Value":@(value),@"Domain":@"127.0.0.1",@"Path":@"/allowed/",
        @"Secure":@(secure!=0),@"HttpOnly":@(hidden!=0),@"ExpirationDate":@(expires)};
    NSData *data=[NSJSONSerialization dataWithJSONObject:fields options:0 error:nil];
    NSString *json=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    kl_webview_set_cookie(json.UTF8String);
}
static void load(void *view,int port,const char *path,int count,int expected){
    char url[160];snprintf(url,sizeof url,"http://127.0.0.1:%d/%s",port,path);
    kl_webview_load(view,url,NULL,0);
    for(int i=0;i<500&&finished<count;i++)pump(.01);
    if(finished!=count||atomic_load(&headers)!=expected)
        fprintf(stderr,"Cookie fixture load=%d finished=%d received-bits=%d expected-bits=%d\n",count,finished,atomic_load(&headers),expected);
    assert(finished==count&&atomic_load(&headers)==expected);
}
int main(void){@autoreleasepool{
    // Confine clearAllData to an ephemeral store in this test process.
    fixtureStore=WKWebsiteDataStore.nonPersistentDataStore;
    Method storeMethod=class_getClassMethod(WKWebsiteDataStore.class,@selector(defaultDataStore));
    IMP originalStore=method_setImplementation(storeMethod,(IMP)fixtureDefaultStore);
    alarm(40);[NSApplication sharedApplication];
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,240)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;[window makeKeyAndOrderFront:nil];
    listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={.sin_len=sizeof address,.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(void*)&address,sizeof address));assert(!listen(listener,8));
    socklen_t size=sizeof address;assert(!getsockname(listener,(void*)&address,&size));int port=ntohs(address.sin_port);
    pthread_t thread;assert(!pthread_create(&thread,NULL,serve,NULL));
    cookie("kl_readable","alpha",0,0,0);cookie("kl_hidden","fixture",1,0,0);
    kl_webview_set_cookie("not JSON");kl_webview_set_cookie("{\"Name\":42}");
    kl_webview_set_cookie("\xff"); // Invalid UTF-8 cannot reach NSJSONSerialization.
    void *first=kl_webview_create(1,320,240,NULL,0,event);
    load(first,port,"allowed/one",1,3);
    kl_webview_eval(first,"document.cookie",1);
    for(int i=0;i<500&&!evaluated;i++)pump(.01);
    assert(evaluated==1&&strstr(result,"kl_readable=alpha")&&!strstr(result,"kl_hidden"));
    void *second=kl_webview_create(2,320,240,NULL,0,event);
    load(second,port,"allowed/two",2,3);
    load(first,port,"outside/",3,0);
    cookie("kl_readable","beta",0,0,0);
    load(first,port,"allowed/updated",4,6);
    cookie("kl_readable","gone",0,0,1);
    load(first,port,"allowed/deleted",5,2);
    cookie("kl_secure","value",1,1,(long long)NSDate.date.timeIntervalSince1970+3600);
    // Wait for the cookie barrier through a normal load, then inspect only our
    // synthetic cookie's attributes. Never print or inspect unrelated values.
    load(first,port,"outside/secure-check",6,0);
    __block BOOL checked=NO;
    [WKWebsiteDataStore.defaultDataStore.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie*> *cookies){
        BOOL found=NO;
        for(NSHTTPCookie *c in cookies)if([c.name isEqual:@"kl_secure"]){
            found=YES;assert(c.secure&&c.HTTPOnly&&c.expiresDate.timeIntervalSinceNow>3000);
        }
        assert(found);
        checked=YES;
    }];
    for(int i=0;i<500&&!checked;i++)pump(.01);assert(checked);
    kl_webview_eval(first,"localStorage.setItem('kl_fixture','present')",2);
    for(int i=0;i<500&&evaluated<2;i++)pump(.01);assert(evaluated==2);
    cookie("kl_readable","alpha",0,0,0);
    kl_webview_clear_data();
    cookie("kl_readable","beta",0,0,0);
    load(first,port,"allowed/after-clear",7,4); // New cookie survives; old HttpOnly cookie is gone.
    kl_webview_eval(first,"String(localStorage.getItem('kl_fixture'))",3);
    for(int i=0;i<500&&evaluated<3;i++)pump(.01);assert(evaluated==3&&!strcmp(result,"null"));
    kl_webview_close(first);kl_webview_close(second);pump(.1);
    atomic_store(&stopping,1);shutdown(listener,SHUT_RDWR);close(listener);pthread_join(thread,NULL);
    [window close];
    method_setImplementation(storeMethod,originalStore);fixtureStore=nil;
    puts("WebKit cookies: pre-create writes, navigation barrier, shared views, HTTP headers, HttpOnly, path scope, replacement, expiry and Secure attributes");
}return 0;}
