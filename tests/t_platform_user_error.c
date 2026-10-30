// An unavailable account must complete the SDK's async request with an error,
// rather than strand the application's request-id-zero awaiter.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_ovrplat.h"
#define API(ret,name,args) ret (*name) args=kl_ovrplat_sym("ovr_" #name);assert(name)
int main(void) {
    setenv("KL_PLAT_USER","0",1);
    unsetenv("KL_NET_OFFLINE");
    API(uint64_t,User_GetLoggedInUser,(void));
    API(uint64_t,User_GetAccessToken,(void));
    API(uint64_t,User_GetUserProof,(void));
    API(uint64_t,User_GetOrgScopedID,(uint64_t));
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
                         User_GetLoggedInUser(),User_GetOrgScopedID(123)};
    const uint32_t types[]={0x436f345d,0x06a85abe,0x22810483,0x18f0b01b,
                            0x436f345d,0x18f0b01b};
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
    puts("Platform account absence: user, token, proof and organization requests complete with ordered, typed errors; no network credentials fabricated");
}
