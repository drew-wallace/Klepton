#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "klepton.h"
#include "kl_errno_bridge.h"

static int fail_again(void){errno=EWOULDBLOCK;return -1;}
static int read_host_errno(void){return errno;}
static double mixed(long a,long b,long c,long d,long e,long f,long g,long h,long i,long j,
                    double x0,double x1,double x2,double x3,double x4,double x5,double x6,double x7,double x8,double x9){
    assert(a==1&&b==2&&c==3&&d==4&&e==5&&f==6&&g==7&&h==8&&i==9&&j==10);
    return x0+2*x1+3*x2+4*x3+5*x4+6*x5+7*x6+8*x7+9*x8+10*x9;
}
typedef struct {uint64_t values[6];} large;
static large big_return(uint64_t a,uint64_t b){return (large){{a,b,a+b,a*b,a^b,a-b}};}
static int *(*location)(void);
static void *worker(void *unused){
    errno=ETIMEDOUT;assert(*location()==110);*location()=93;assert(*location()==93);return NULL;
}
int main(void){
    void *original=kl_shim_lookup("__errno");assert(original);
    assert(kl_errno_bridge_import("libOther.so","__errno",original)==original);
    location=kl_errno_bridge_import("/fixture/libBigSoup.so","__errno",original);
    assert((void*)location!=original);
    assert(kl_errno_bridge_import("/fixture/libBigSoup.framework/libBigSoup","__errno",original)==(void*)location);
    assert(kl_errno_bridge_import("/fixture/libBigSoup.dylib","__errno",original)==(void*)location);
    assert(kl_errno_bridge_import("libBigSoupOther.so","__errno",original)==original);
    errno=EAFNOSUPPORT;assert(*location()==97);
    // The exact usrsctp failure: a real success written into guest errno must
    // survive the next access despite a stale native error.
    *location()=0;assert(*location()==0&&errno==0);
    *location()=93;assert(*location()==93&&errno==EPROTONOSUPPORT);
    int(*fail)(void)=kl_errno_bridge_import("libBigSoup.so","fixture_fail",fail_again);
    int(*host)(void)=kl_errno_bridge_import("libBigSoup.so","fixture_read",read_host_errno);
    for(int i=0;i<3;i++){*location()=0;assert(fail()==-1);assert(*location()==11);}
    *location()=110;assert(host()==ETIMEDOUT&&*location()==110);
    double(*call)(long,long,long,long,long,long,long,long,long,long,double,double,double,double,double,double,double,double,double,double)=
        kl_errno_bridge_import("libBigSoup.so","fixture_mixed",mixed);
    assert(call(1,2,3,4,5,6,7,8,9,10,1,2,3,4,5,6,7,8,9,10)==385);
    large(*ret)(uint64_t,uint64_t)=kl_errno_bridge_import("libBigSoup.so","fixture_sret",big_return);
    large value=ret(11,7);assert(value.values[0]==11&&value.values[1]==7&&value.values[2]==18&&value.values[3]==77&&value.values[4]==12&&value.values[5]==4);
    *location()=98;pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));assert(!pthread_join(thread,NULL));assert(*location()==98);
    puts("BigSoup errno: guest writes, repeated native failures, Linux mapping, thread isolation, integer/vector/stack arguments and sret pass");
    return 0;
}
