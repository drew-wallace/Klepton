// Decode actual Graph response shapes without accessing a real account,
// Keychain, or network. The SDK's result-copy/queue contract is tested in
// t_platform_user_error through its public guest entry points.
#import <Foundation/Foundation.h>
#include <assert.h>
#include <string.h>
#include "kl_ovrplat.h"

static kl_ovrplat_account_result captured;
static char text[512],error_text[512],alias[128],name[128];
int kl_ovrplat_complete_account(uint64_t request,const kl_ovrplat_account_result *result) {
    assert(request==123);
    captured=*result;
    snprintf(text,sizeof text,"%s",result->string?result->string:"");
    snprintf(error_text,sizeof error_text,"%s",result->error?result->error:"");
    snprintf(alias,sizeof alias,"%s",result->oculus_id?result->oculus_id:"");
    snprintf(name,sizeof name,"%s",result->display_name?result->display_name:"");
    return 1;
}
void kl_ovrplat_set_account_provider(kl_ovrplat_account_provider p,void *context) { assert(!"No test should register a live provider"); }
#include "../runtime/xr/kl_meta_account.m"

static void decode(int kind,int http,NSString *json) {
    memset(&captured,0,sizeof captured);
    klmeta_decode(123,kind,http,[json dataUsingEncoding:NSUTF8StringEncoding]);
}
int main(void) {
    @autoreleasepool {
        decode(KL_OVRPLAT_ACCOUNT_USER,200,@"{\"id\":\"9876543210123456\",\"alias\":\"Fixture\",\"display_name\":\"Jó Fixture\"}");
        assert(!captured.error_code&&captured.user_id==9876543210123456ull);
        assert(!strcmp(alias,"Fixture")&&!strcmp(name,"Jó Fixture"));
        decode(KL_OVRPLAT_ACCOUNT_PROOF,200,@"{\"nonce\":\"fixture-proof\"}");
        assert(!captured.error_code&&!strcmp(text,"fixture-proof"));
        decode(KL_OVRPLAT_ACCOUNT_ORG_ID,200,@"{\"id\":\"9\",\"org_scoped_id\":{\"id\":\"123456\"}}");
        assert(captured.user_id==123456);
        // A game-scoped ID must never be substituted for a missing org ID.
        decode(KL_OVRPLAT_ACCOUNT_ORG_ID,200,@"{\"id\":\"9\"}");assert(!captured.user_id);
        decode(KL_OVRPLAT_ACCOUNT_USER,200,@"{\"id\":\"18446744073709551616\"}");assert(!captured.user_id);
        decode(KL_OVRPLAT_ACCOUNT_USER,200,@"{\"id\":-1}");assert(!captured.user_id);
        decode(KL_OVRPLAT_ACCOUNT_USER,200,@"{\"id\":1.5}");assert(!captured.user_id);
        decode(KL_OVRPLAT_ACCOUNT_PROOF,401,@"{\"error\":{\"code\":190,\"message\":\"private-token-must-not-appear\"}}");
        assert(captured.error_code==190&&captured.http_code==401);
        assert(strstr(error_text,"expired")&&!strstr(error_text,"private-token"));
        decode(KL_OVRPLAT_ACCOUNT_PROOF,200,@"{\"error\":{\"code\":10}}");assert(captured.error_code==10);
        decode(KL_OVRPLAT_ACCOUNT_PROOF,200,@"[]");assert(captured.error_code);
        decode(KL_OVRPLAT_ACCOUNT_PROOF,503,@"not JSON");assert(captured.error_code&&captured.http_code==503);
        puts("Meta Graph decoding: exact IDs, profile/nonce shapes, scoped identity and redacted service failures");
    }
}
