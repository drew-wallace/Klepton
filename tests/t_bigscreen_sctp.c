// Load the original APK's real SCTP implementation without an account or room.
// Exercise socket creation with a stale native errno, as at the crash site.
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_errno_bridge.h"

static int output(void *address,void *buffer,size_t length,uint8_t tos,uint8_t df){return 0;}
int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: %s /path/to/libBigSoup.so\n",argv[0]);return 2;}
    alarm(30);kl_thread_init();
    char dir[2048];snprintf(dir,sizeof dir,"%s",argv[1]);char *slash=strrchr(dir,'/');assert(slash);*slash=0;kl_set_library_path(dir);
    kl_image *image=kl_load_recursive(argv[1]);
    if(!image){fprintf(stderr,"load: %s\n",kl_error());return 1;}
    kl_run_init(image);
    void(*init)(uint16_t,void*,void*)=kl_sym(image,"usrsctp_init_nothreads");
    void *(*create)(int,int,int,void*,void*,uint32_t,void*)=kl_sym(image,"usrsctp_socket");
    int(*ulp)(void*,void*)=kl_sym(image,"usrsctp_set_ulpinfo");
    void(*close_socket)(void*)=kl_sym(image,"usrsctp_close");
    assert(init&&create&&ulp&&close_socket);
    int *(*location)(void)=kl_errno_bridge_import("libBigSoup.so","__errno",kl_shim_lookup("__errno"));
    init(0,(void*)output,NULL);
    // A valid AF_CONN socket must be independent of an unrelated old error.
    errno=EWOULDBLOCK;
    void *socket=create(123,1,132,NULL,NULL,0,NULL);
    int error=*location();
    printf("Real BigSoup SCTP: socket=%s guest_errno=%d\n",socket?"created":"NULL",error);fflush(stdout);
    if(!socket)return 1;
    assert(ulp(socket,(void*)0x1234)==1);
    close_socket(socket);
    // The bridge must preserve a genuinely invalid request rather than make
    // every socket request look successful.
    void *invalid=create(999,1,132,NULL,NULL,0,NULL);
    int invalid_error=*location();
    printf("Real BigSoup SCTP: invalid_socket=%s guest_errno=%d\n",invalid?"created":"NULL",invalid_error);fflush(stdout);
    assert(!invalid&&invalid_error==22); // Bundled socreate rejects this with EINVAL.
    puts("Real BigSoup SCTP creation and ULP registration pass with stale native errno");
    return 0;
}
