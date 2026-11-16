// A nativeLowMemory callback must execute on the polling guest frame thread,
// never on the dispatch worker that reports the OS event.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_jni_slots.h"

#define JNI_SLOT_ENUM(n,name) SLOT_##name=n,
enum {KL_JNI_SLOTS(JNI_SLOT_ENUM)};
static pthread_t frame_thread;
static atomic_int callbacks;
static void low_memory(void *env,void *cls){
    assert(pthread_equal(pthread_self(),frame_thread));
    assert(env==kl_jni_env()&&cls==kl_jni_class("com/unity3d/player/UnityPlayer"));
    atomic_fetch_add(&callbacks,1);
}
static void *request(void *unused){
    for(int i=0;i<100;i++)kl_mem_pressure_request_system(i&1);
    assert(atomic_load(&callbacks)==0);
    return NULL;
}
int main(void){
    setenv("KL_LOWMEM_AT","0",1);
    frame_thread=pthread_self();
    void *env=kl_jni_env();void **table=*(void ***)env;
    int32_t (*reg)(void*,void*,const kl_jni_method*,int32_t)=table[SLOT_RegisterNatives];
    kl_jni_method method={"nativeLowMemory","()V",(void*)low_memory};
    assert(reg(env,kl_jni_class("com/unity3d/player/UnityPlayer"),&method,1)==0);
    pthread_t worker;assert(!pthread_create(&worker,NULL,request,NULL));
    assert(!pthread_join(worker,NULL));assert(atomic_load(&callbacks)==0);
    kl_mem_pressure_poll();assert(atomic_load(&callbacks)==1);
    kl_mem_pressure_poll();assert(atomic_load(&callbacks)==1);
    kl_mem_pressure_request_system(0);assert(atomic_load(&callbacks)==1);
    kl_mem_pressure_poll();assert(atomic_load(&callbacks)==2);
    puts("Low-memory notifications: deferred from worker, coalesced, delivered on guest frame thread with footprint polling disabled");
    return 0;
}
