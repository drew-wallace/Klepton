// An unavailable account must complete the SDK's async request with an error,
// rather than strand the application's request-id-zero awaiter.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "kl_ovrplat.h"
#define API(ret,name,args) ret (*name) args=kl_ovrplat_sym("ovr_" #name);assert(name)

static struct { uint64_t id; int kind; uint64_t user; char app[64]; } issued[64];
static unsigned issued_count;
static void provider(uint64_t request,int kind,const char *app,uint64_t user,void *context) {
    assert(context==issued);
    assert(issued_count<64);
    unsigned i=issued_count++;
    issued[i].id=request;issued[i].kind=kind;issued[i].user=user;
    snprintf(issued[i].app,sizeof issued[i].app,"%s",app);
}
static void *worker(void *opaque) {
    uint64_t request=*(uint64_t*)opaque;
    kl_ovrplat_account_result result={.string="test-proof-not-a-network-credential"};
    assert(kl_ovrplat_complete_account(request,&result));return NULL;
}
static void provider_contract(void) {
    API(uint64_t,UnityInitWrapperAsynchronous,(const char*));
    API(uint64_t,User_GetLoggedInUser,(void));
    API(uint64_t,User_GetAccessToken,(void));
    API(uint64_t,User_GetUserProof,(void));
    API(uint64_t,User_GetOrgScopedID,(uint64_t));
    API(uint64_t,GetLoggedInUserID,(void));
    API(void*,PopMessage,(void));
    API(uint64_t,Message_GetRequestID,(const void*));
    API(uint64_t,Message_GetType,(const void*));
    API(uint64_t,Message_IsError,(const void*));
    API(const void*,Message_GetUser,(const void*));
    API(const void*,Message_GetOrgScopedID,(const void*));
    API(uint64_t,User_GetID,(const void*));
    API(uint64_t,OrgScopedID_GetID,(const void*));
    API(const char*,User_GetOculusID,(const void*));
    API(const char*,User_GetDisplayName,(const void*));
    API(const char*,User_GetImageUrl,(const void*));
    API(const char*,User_GetSmallImageUrl,(const void*));
    API(const char*,Message_GetString,(const void*));
    API(const void*,Message_GetError,(const void*));
    API(int32_t,Error_GetCode,(const void*));
    API(int32_t,Error_GetHttpCode,(const void*));
    API(void,FreeMessage,(void*));
    assert(UnityInitWrapperAsynchronous("2370815932930055"));
    void *m=PopMessage();assert(m);FreeMessage(m);
    kl_ovrplat_set_account_provider(provider,issued);
    uint64_t user=User_GetLoggedInUser(),proof=User_GetUserProof(),token=User_GetAccessToken(),org=User_GetOrgScopedID(123);
    assert(user&&proof&&token&&org&&issued_count==4&&!PopMessage());
    for(unsigned i=0;i<4;i++)assert(!strcmp(issued[i].app,"2370815932930055"));
    assert(issued[0].kind==KL_OVRPLAT_ACCOUNT_USER&&issued[1].kind==KL_OVRPLAT_ACCOUNT_PROOF);
    assert(issued[3].kind==KL_OVRPLAT_ACCOUNT_ORG_ID&&issued[3].user==123);
    assert(!GetLoggedInUserID());
    const char *absent[]={"User_GetPresence","User_GetPresenceDeeplinkMessage",
        "User_GetPresenceDestinationApiName","User_GetPresenceLobbySessionId","User_GetPresenceMatchSessionId",
        "ManagedInfo_GetDepartment","ManagedInfo_GetEmail","ManagedInfo_GetEmployeeNumber","ManagedInfo_GetExternalId",
        "ManagedInfo_GetLocation","ManagedInfo_GetManager","ManagedInfo_GetName","ManagedInfo_GetOrganizationId",
        "ManagedInfo_GetOrganizationName","ManagedInfo_GetPosition"};
    for(unsigned i=0;i<sizeof absent/sizeof *absent;i++) {
        char symbol[128];snprintf(symbol,sizeof symbol,"ovr_%s",absent[i]);
        const char *(*get)(const void*)=kl_ovrplat_sym(symbol);assert(get&&!strcmp(get(NULL),""));
    }
    char nonce[]="fixture-fresh-proof";
    kl_ovrplat_account_result result={.string=nonce};
    assert(kl_ovrplat_complete_account(proof,&result));nonce[0]='X';
    m=PopMessage();assert(m&&!Message_IsError(m)&&Message_GetRequestID(m)==proof);
    assert(Message_GetType(m)==0x22810483&&!strcmp(Message_GetString(m),"fixture-fresh-proof"));
    assert(!kl_ovrplat_complete_account(proof,&result));FreeMessage(m);
    char alias[]="FixtureAlias",name[]="Fixture Player";
    result=(kl_ovrplat_account_result){.user_id=123,.oculus_id=alias,.display_name=name,.image_url="https://example.invalid/avatar",.small_image_url="https://example.invalid/small"};
    assert(kl_ovrplat_complete_account(user,&result));alias[0]=name[0]='X';
    void *user_message=PopMessage();assert(user_message&&!Message_IsError(user_message));
    const void *handle=Message_GetUser(user_message);
    assert(User_GetID(handle)==123&&GetLoggedInUserID()==123);
    assert(!strcmp(User_GetOculusID(handle),"FixtureAlias")&&!strcmp(User_GetDisplayName(handle),"Fixture Player"));
    assert(!strcmp(User_GetImageUrl(handle),"https://example.invalid/avatar"));
    assert(!strcmp(User_GetSmallImageUrl(handle),"https://example.invalid/small"));
    result=(kl_ovrplat_account_result){.user_id=456};assert(kl_ovrplat_complete_account(org,&result));
    m=PopMessage();assert(m&&OrgScopedID_GetID(Message_GetOrgScopedID(m))==456);FreeMessage(m);
    result=(kl_ovrplat_account_result){.error="The session expired.",.error_code=190,.http_code=401};
    assert(kl_ovrplat_complete_account(token,&result));m=PopMessage();assert(m&&Message_IsError(m));
    const void *error=Message_GetError(m);assert(Error_GetCode(error)==190&&Error_GetHttpCode(error)==401);FreeMessage(m);
    // Sign-out cancels pending requests, rejects late results, and leaves a
    // previously popped SDK user snapshot valid until its owner frees it.
    proof=User_GetUserProof();assert(proof);
    kl_ovrplat_set_account_provider(NULL,NULL);
    assert(!GetLoggedInUserID());assert(User_GetID(handle)==123);
    assert(!strcmp(User_GetDisplayName(handle),"Fixture Player"));FreeMessage(user_message);
    result=(kl_ovrplat_account_result){.string="stale"};assert(!kl_ovrplat_complete_account(proof,&result));
    m=PopMessage();assert(m&&Message_IsError(m)&&Message_GetRequestID(m)==proof);FreeMessage(m);
    kl_ovrplat_set_account_provider(provider,issued);
    // A game change invalidates pending work from the preceding app.
    proof=User_GetUserProof();assert(UnityInitWrapperAsynchronous("777"));
    assert(!kl_ovrplat_complete_account(proof,&result));
    m=PopMessage();assert(m&&Message_IsError(m));FreeMessage(m);
    m=PopMessage();assert(m&&!Message_IsError(m));FreeMessage(m);
    // Reserve the entire queue, then complete requests concurrently. Each
    // response must remain owned and arrive exactly once without overwrite.
    uint64_t requests[16];pthread_t threads[16];
    for(unsigned i=0;i<16;i++)requests[i]=User_GetUserProof();
    for(unsigned i=0;i<16;i++){assert(requests[i]);assert(!pthread_create(&threads[i],NULL,worker,&requests[i]));}
    for(unsigned i=0;i<16;i++)assert(!pthread_join(threads[i],NULL));
    unsigned seen=0;
    for(unsigned i=0;i<16;i++){
        m=PopMessage();assert(m&&!Message_IsError(m));uint64_t id=Message_GetRequestID(m);
        unsigned j=0;while(j<16&&requests[j]!=id)j++;assert(j<16&&!(seen&(1u<<j)));seen|=1u<<j;FreeMessage(m);
    }
    assert(seen==0xffff&&!PopMessage());
    // Malformed successful responses must not create an authenticated user.
    user=User_GetLoggedInUser();result=(kl_ovrplat_account_result){0};
    assert(kl_ovrplat_complete_account(user,&result));m=PopMessage();assert(m&&Message_IsError(m)&&!GetLoggedInUserID());FreeMessage(m);
    kl_ovrplat_set_account_provider(NULL,NULL);
    puts("Platform provider: app-scoped async identity/proof, owned payloads, sign-out, stale response rejection and concurrent completions");
}

int main(void) {
    setenv("KL_PLAT_USER","0",1);
    unsetenv("KL_NET_OFFLINE");
    API(uint64_t,User_GetLoggedInUser,(void));
    API(uint64_t,User_GetAccessToken,(void));
    API(uint64_t,User_GetUserProof,(void));
    API(uint64_t,User_GetOrgScopedID,(uint64_t));
    API(uint64_t,User_GetLoggedInUserFriends,(void));
    API(uint64_t,DeviceApplicationIntegrity_GetIntegrityToken,(const char*));
    API(uint64_t,IAP_GetViewerPurchases,(void));
    API(void*,PopMessage,(void));
    API(uint64_t,Message_GetRequestID,(const void*));
    API(uint64_t,Message_GetType,(const void*));
    API(uint64_t,Message_IsError,(const void*));
    API(const void*,Message_GetError,(const void*));
    API(const char*,Error_GetMessage,(const void*));
    API(int32_t,Error_GetCode,(const void*));
    API(int32_t,Error_GetHttpCode,(const void*));
    API(void,FreeMessage,(void*));
    uint64_t requests[]={User_GetLoggedInUser(),User_GetAccessToken(),
                         User_GetUserProof(),User_GetOrgScopedID(0),
                         User_GetLoggedInUser(),User_GetOrgScopedID(123),
                         User_GetLoggedInUserFriends(),
                         DeviceApplicationIntegrity_GetIntegrityToken("private-test-challenge"),
                         IAP_GetViewerPurchases()};
    const uint32_t types[]={0x436f345d,0x06a85abe,0x22810483,0x18f0b01b,
                            0x436f345d,0x18f0b01b,0x587c2a8d,0x3271abda,0x3a0f8419};
    for(unsigned i=0;i<sizeof requests/sizeof *requests;i++) {
        assert(requests[i]);
        for(unsigned j=0;j<i;j++) assert(requests[i]!=requests[j]);
        void *message=PopMessage();assert(message);
        assert(Message_GetRequestID(message)==requests[i]);
        assert(Message_GetType(message)==types[i]);
        assert(Message_IsError(message));
        const void *error=Message_GetError(message);assert(error);
        assert(strstr(Error_GetMessage(error),"unavailable"));
        assert(Error_GetCode(error)==-1&&Error_GetHttpCode(error)==0);
        FreeMessage(message);
    }
    assert(!PopMessage());
    provider_contract();
    puts("Platform account absence: user, token, proof and organization requests complete with ordered, typed errors; no network credentials fabricated");
}
