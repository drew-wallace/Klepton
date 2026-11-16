#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <TargetConditionals.h>
#include <errno.h>
#include <stdlib.h>
#include "kl_meta_account.h"
#include "kl_ovrplat.h"
#include "kl_env.h"

// Meta session imports contain a game-scoped access token, never a password or
// a general account/profile token. They are transferred through CoreDevice and
// consumed into this app's device-only Keychain before guest initialization.
static NSString *klmeta_service(void) {
    return [[NSBundle mainBundle].bundleIdentifier stringByAppendingString:@".meta-account"];
}
static NSDictionary *klmeta_query(NSString *app) {
    NSMutableDictionary *q=[@{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService:klmeta_service()} mutableCopy];
    if (app) q[(__bridge id)kSecAttrAccount]=app;
    return q;
}
static BOOL klmeta_app(NSString *app) {
    return [app isKindOfClass:NSString.class] && app.length>0 && app.length<64 &&
        [app rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location==NSNotFound;
}
static NSDictionary *klmeta_load(NSString *app) {
    NSMutableDictionary *q=[klmeta_query(app) mutableCopy];
    q[(__bridge id)kSecReturnData]=@YES; q[(__bridge id)kSecMatchLimit]=(__bridge id)kSecMatchLimitOne;
    CFTypeRef value=NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q,&value)!=errSecSuccess) return nil;
    NSData *data=CFBridgingRelease(value);
    id object=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![object isKindOfClass:NSDictionary.class] || ![object[@"app_id"] isEqual:app] ||
        ![object[@"access_token"] isKindOfClass:NSString.class] || ![object[@"access_token"] length]) return nil;
    return object;
}
static void klmeta_error(uint64_t request,const char *message,int http,int code) {
    kl_ovrplat_account_result r={.error=message,.error_code=code?code:-1,.http_code=http};
    kl_ovrplat_complete_account(request,&r);
}
static uint64_t klmeta_id(id value) {
    NSString *s=[value isKindOfClass:NSNumber.class] ? [value stringValue] : value;
    if (!klmeta_app(s)) return 0;
    const char *text=s.UTF8String; char *end=NULL; errno=0;
    unsigned long long n=strtoull(text,&end,10);
    return errno || !end || *end ? 0 : n;
}
static NSString *klmeta_string(id value) { return [value isKindOfClass:NSString.class] ? value : nil; }

// Do not forward arbitrary Meta error text to guest logs: remote error strings
// may echo submitted values. Keep the actual HTTP/API error codes for diagnosis.
static void klmeta_decode(uint64_t request,int kind,int http,NSData *data) {
    if (data.length>1024*1024) { klmeta_error(request,"Meta returned an oversized account response.",http,-1);return; }
    id raw=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![raw isKindOfClass:NSDictionary.class]) { klmeta_error(request,"Meta returned an invalid account response.",http,-1);return; }
    NSDictionary *json=raw;
    if (http<200 || http>=300 || json[@"error"]) {
        id e=json[@"error"];int code=[e isKindOfClass:NSDictionary.class] && [e[@"code"] respondsToSelector:@selector(intValue)] ? [e[@"code"] intValue] : -1;
        if (code==190) dispatch_async(dispatch_get_main_queue(), ^{
            [NSNotificationCenter.defaultCenter postNotificationName:@"KleptonMetaSessionExpired" object:nil];
        });
        klmeta_error(request,code==190 ? "The Meta session expired. Sign in again." : "Meta rejected the account request.",http,code);return;
    }
    kl_ovrplat_account_result r={0};
    if (kind==KL_OVRPLAT_ACCOUNT_USER) {
        r.user_id=klmeta_id(json[@"id"]);
        r.oculus_id=klmeta_string(json[@"alias"]).UTF8String;
        r.display_name=klmeta_string(json[@"display_name"]).UTF8String;
        r.image_url=klmeta_string(json[@"profile_url"]).UTF8String;
        r.small_image_url=klmeta_string(json[@"profile_url_small"]).UTF8String;
    } else if (kind==KL_OVRPLAT_ACCOUNT_PROOF) {
        r.string=klmeta_string(json[@"nonce"]).UTF8String;
    } else if (kind==KL_OVRPLAT_ACCOUNT_ORG_ID) {
        id org=json[@"org_scoped_id"];
        r.user_id=klmeta_id([org isKindOfClass:NSDictionary.class] ? org[@"id"] : org);
    }
    kl_ovrplat_complete_account(request,&r);
}

@interface KLMetaHTTP : NSObject <NSURLSessionTaskDelegate>
@end
@implementation KLMetaHTTP
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
 willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
 completionHandler:(void (^)(NSURLRequest *))completionHandler { completionHandler(nil); }
@end

static void klmeta_request(uint64_t request,int kind,const char *app_id,uint64_t user_id,void *context) {
    @autoreleasepool {
        NSString *app=[NSString stringWithUTF8String:app_id];
        NSDictionary *record=klmeta_load(app);
        NSString *token=record[@"access_token"];
        if (!token) { klmeta_error(request,"No Meta session is linked for this game.",0,-1);return; }
        if (kind==KL_OVRPLAT_ACCOUNT_TOKEN) {
            kl_ovrplat_account_result r={.string=token.UTF8String};kl_ovrplat_complete_account(request,&r);return;
        }
        NSString *path;
        if (kind==KL_OVRPLAT_ACCOUNT_USER) path=@"me?fields=id,alias,display_name,profile_url,profile_url_small";
        else if (kind==KL_OVRPLAT_ACCOUNT_PROOF) path=@"user_nonce_generate";
        else if (kind==KL_OVRPLAT_ACCOUNT_ORG_ID && user_id) path=[NSString stringWithFormat:@"%llu?fields=org_scoped_id",(unsigned long long)user_id];
        else { klmeta_error(request,"Unsupported Meta account request.",0,-1);return; }
        NSMutableURLRequest *r=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[@"https://graph.oculus.com/" stringByAppendingString:path]]];
        r.HTTPMethod=kind==KL_OVRPLAT_ACCOUNT_PROOF ? @"POST" : @"GET";
        [r setValue:[@"Bearer " stringByAppendingString:token] forHTTPHeaderField:@"Authorization"];
        r.timeoutInterval=30;
        static NSURLSession *session;static dispatch_once_t once;
        dispatch_once(&once,^{
            NSURLSessionConfiguration *c=NSURLSessionConfiguration.ephemeralSessionConfiguration;
            c.URLCache=nil;c.HTTPCookieStorage=nil;
            session=[NSURLSession sessionWithConfiguration:c delegate:[KLMetaHTTP new] delegateQueue:nil];
        });
        [[session dataTaskWithRequest:r completionHandler:^(NSData *data,NSURLResponse *response,NSError *error) {
            if (error) { klmeta_error(request,"Could not reach Meta account services.",0,-1);return; }
            int status=[response isKindOfClass:NSHTTPURLResponse.class] ? (int)((NSHTTPURLResponse *)response).statusCode : 0;
            klmeta_decode(request,kind,status,data ?: [NSData data]);
        }] resume];
    }
}

void kl_meta_account_install(void) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        if (kl_env_on("KL_NET_OFFLINE",0)) return;
        NSURL *documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL *file=[documents URLByAppendingPathComponent:@"klepton-meta-session.json"];
        NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:file.path error:nil];
        if ([attrs[NSFileType] isEqual:NSFileTypeRegular] && [attrs[NSFileSize] unsignedLongLongValue]<=32768) {
            NSData *data=[NSData dataWithContentsOfURL:file];
            id object=data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            if ([object isKindOfClass:NSDictionary.class] && klmeta_app(object[@"app_id"]) &&
                [object[@"access_token"] isKindOfClass:NSString.class] && [object[@"access_token"] length]>0 &&
                [object[@"access_token"] length]<16384 && [object count]==2) {
                NSDictionary *query=klmeta_query(object[@"app_id"]);
                NSDictionary *values=@{(__bridge id)kSecValueData:data,(__bridge id)kSecAttrAccessible:(__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly};
                OSStatus status=SecItemUpdate((__bridge CFDictionaryRef)query,(__bridge CFDictionaryRef)values);
                if (status==errSecItemNotFound) {
                    NSMutableDictionary *add=[query mutableCopy];[add addEntriesFromDictionary:values];
                    status=SecItemAdd((__bridge CFDictionaryRef)add,NULL);
                }
                if (status==errSecSuccess) [NSFileManager.defaultManager removeItemAtURL:file error:nil];
            }
        }
        if (SecItemCopyMatching((__bridge CFDictionaryRef)klmeta_query(nil),NULL)==errSecSuccess)
            kl_ovrplat_set_account_provider(klmeta_request,NULL);
    }
#endif
}
