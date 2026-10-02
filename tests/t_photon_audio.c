// Test the recorder contract with synthetic PCM; never opens a microphone.
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "kl_jni.h"
#include "../runtime/jni/kl_jni_int.h"
static int enabled, open_failure;
static atomic_int opens, closes, callbacks, stops;
static void *test_buffer;
static int use_real_proxy;
static int fixture_enabled(void) { return enabled; }
static int fixture_open(unsigned rate, unsigned channels) { atomic_fetch_add(&opens, 1); return open_failure; }
static void fixture_close(void) { atomic_fetch_add(&closes, 1); }
static unsigned fixture_rate(void) { return 48000; }
static unsigned fixture_channels(void) { return 2; }
static int fixture_read(int16_t *pcm, int frames) {
    int take = frames > 121 ? 121 : frames; // Partial reads must not submit stale PCM.
    for (int i = 0; i < take; i++) { pcm[i*2] = 1000; pcm[i*2+1] = 3000; }
    return take;
}
static void *fixture_callback(void *proxy, const char *iface, const char *name, const char *sig, void *args) {
    assert(!strcmp(iface,"com/exitgames/photon/audioinaec/AudioInAEC$DataCallback"));
    if (!strcmp(name,"OnData")) {
        klj_array *array = klj_arr(test_buffer);
        assert(array && array->kind=='S');
        for (int i=0;i<array->len;i++) assert(((int16_t*)array->data)[i] == 2000);
        atomic_fetch_add(&callbacks,1);
    } else { assert(!strcmp(name,"OnStop")); atomic_fetch_add(&stops,1); }
    if (use_real_proxy) klj_proxy_invoke(proxy, iface, name, sig, args);
    return NULL;
}
#define kl_audio_mic_enabled fixture_enabled
#define kl_audio_mic_open fixture_open
#define kl_audio_mic_close fixture_close
#define kl_audio_mic_rate fixture_rate
#define kl_audio_mic_channels fixture_channels
#define kl_audio_mic_read_i16 fixture_read
#define klj_proxy_invoke fixture_callback
#define klj_bind_photon fixture_bind_photon
#define klj_photon_reflected_signature fixture_reflected_signature
#include "../runtime/jni/kl_jni_photon.c"
#undef klj_proxy_invoke
#undef klj_photon_reflected_signature
static atomic_int modern_callbacks;
static int legacy_callbacks;
static void *modern_invoke(void *env, void *cls, int64_t handle, void *name, void *args) {
    assert(handle == 1234);
    assert(!strcmp(klj_str(name), "OnData") || !strcmp(klj_str(name), "OnStop"));
    assert(args == NULL); modern_callbacks++; return NULL;
}
static void *legacy_invoke(void *env, void *cls, int64_t handle, void *iface, void *method, void *args) {
    assert(handle == 5678);
    assert(!strcmp(klj_class_name(iface), PHOTON_CALLBACK));
    klj_object *o = klj_as_object(method); assert(o && !strcmp(o->cls, KLJ_CLASS_METHOD));
    legacy_callbacks++; return NULL;
}
static void *reflection_and_proxy_contract(void **table) {
    void *env = kl_jni_env();
    void *(*method)(void*,void*,const char*,const char*) = table[33];
    void *(*call_object)(void*,void*,void*,const void*) = table[116];
    void *(*from_reflected)(void*,void*) = table[7];
    int (*call_bool)(void*,void*,void*,const void*) = table[39];
    int (*register_natives)(void*,void*,const kl_jni_method*,int) = table[215];
    void *reflection = kl_jni_class("com/unity3d/player/ReflectionHelper");
    void *get = method(env, reflection, "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;");
    const char *requests[] = {
        PHOTON_START_SIG,
        "(Lcom/unity3d/player/UnityPlayerActivity;L" PHOTON_CALLBACK ";IIIZZZ)Z",
        "(Lcom/unity3d/player/UnityPlayerActivity;Ljava/lang/Object;IIIZZZ)Z",
        "(Lcom/unity3d/player/UnityPlayerActivity;L" KLJ_CLASS_PROXY ";IIIZZZ)Z",
        // Exact signature from the physical 16:13:21 unmute crash. Unity's
        // reflection request accepts dotted names, unlike a JNI descriptor.
        "(Lcom.unity3d.player.UnityPlayerActivity;Lcom.exitgames.photon.audioinaec.AudioInAEC$DataCallback;IIIZZZ)Z"};
    for (unsigned i=0;i<sizeof requests/sizeof requests[0];i++) {
        klj_jvalue a[4]={{.l=kl_jni_class(PHOTON_AUDIO)},{.l=kl_jni_new_string("Start")},
                        {.l=kl_jni_new_string(requests[i])},{.z=0}};
        void *resolved=call_object(env,reflection,get,a); assert(resolved);
        klj_method_obj *m=klj_as_object(resolved)->data; assert(!strcmp(m->sig,PHOTON_START_SIG));
        void *mid=from_reflected(env,resolved); assert(mid);
        // Capture is disabled. Dispatch must reach Start and return false,
        // rather than aborting on an unimplemented concrete-argument signature.
        klj_jvalue start[8]={{0}};
        void *recorder=photon_init(NULL,NULL,NULL,0).l;
        assert(!call_bool(env,recorder,mid,start));
    }
    const char *bad="(Landroid/app/Activity;Ljava/lang/Object;IIIZZZ)I";
    assert(fixture_reflected_signature(PHOTON_AUDIO,"Start",bad,0)==bad);
    assert(fixture_reflected_signature(PHOTON_AUDIO,"Start",requests[2],1)==requests[2]);
    // Normalize reference arrays and return types without changing primitive
    // descriptors, nested-class '$', staticness, or the guest's source string.
    const char *dotted="([Ljava.lang.String;I)Lcom.example.Nested$Thing;";
    void *signature=kl_jni_new_string(dotted);
    klj_jvalue request[4]={{.l=kl_jni_class("fixture/Reflection")},
        {.l=kl_jni_new_string("example")},{.l=signature},{.z=1}};
    void *resolved=call_object(env,reflection,get,request);
    klj_method_obj *description=klj_as_object(resolved)->data;
    assert(!strcmp(description->sig,"([Ljava/lang/String;I)Lcom/example/Nested$Thing;"));
    assert(description->is_static && !strcmp(klj_str(signature),dotted));
    void *constructor=method(env,reflection,"getConstructorID","(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;");
    klj_jvalue ctor[2]={{.l=request[0].l},{.l=kl_jni_new_string("(Landroid.app.Activity;[Ljava.lang.String;)V")}};
    resolved=call_object(env,reflection,constructor,ctor);
    description=klj_as_object(resolved)->data;
    assert(!strcmp(description->sig,"(Landroid/app/Activity;[Ljava/lang/String;)V"));
    void *field=method(env,reflection,"getFieldID","(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;");
    request[2].l=kl_jni_new_string("[Lcom.example.Nested$Thing;");
    resolved=call_object(env,reflection,field,request);
    void *field_signature=method(env,reflection,"getFieldSignature","(Ljava/lang/reflect/Field;)Ljava/lang/String;");
    klj_jvalue field_arg={.l=resolved};
    assert(!strcmp(klj_str(call_object(env,reflection,field_signature,&field_arg)),"[Lcom/example/Nested$Thing;"));
    // The existing Unity permission bindings must still dispatch after
    // normalization, and direct dotted JNI lookups remain supported.
    request[0].l=kl_jni_class("com/unity3d/player/UnityPermissions");
    request[1].l=kl_jni_new_string("hasUserAuthorizedPermission");
    request[2].l=kl_jni_new_string("(Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)Z");
    resolved=call_object(env,reflection,get,request);
    klj_jvalue permission[2]={{.l=kl_jni_activity()},{.l=kl_jni_new_string("android.permission.RECORD_AUDIO")}};
    int reflected_result=call_bool(env,request[0].l,from_reflected(env,resolved),permission);
    void *direct=method(env,request[0].l,"hasUserAuthorizedPermission",klj_str(request[2].l));
    assert(call_bool(env,request[0].l,direct,permission)==reflected_result);
    kl_jni_method modern={"nativeProxyInvoke","(JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;",modern_invoke};
    kl_jni_method legacy={"invoke","(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;",legacy_invoke};
    assert(!register_natives(env,reflection,&modern,1));
    assert(!register_natives(env,kl_jni_class("bitter/jnibridge/JNIBridge"),&legacy,1));
    void *create=method(env,reflection,"newProxyInstance","(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;");
    klj_jvalue proxy_args[3]={{.l=NULL},{.j=1234},{.l=kl_jni_class(PHOTON_CALLBACK)}};
    void *modern_proxy=call_object(env,reflection,create,proxy_args); assert(modern_proxy);
    klj_proxy_invoke(modern_proxy,PHOTON_CALLBACK,"OnData","()V",NULL);
    klj_proxy_invoke(modern_proxy,PHOTON_CALLBACK,"OnStop","()V",NULL);
    void *old_proxy=kl_jni_new_object(KLJ_CLASS_PROXY); klj_proxy p={.native_ptr=5678};
    klj_as_object(old_proxy)->data=&p;
    klj_proxy_invoke(old_proxy,PHOTON_CALLBACK,"OnData","()V",NULL);
    assert(modern_callbacks==2 && legacy_callbacks==1);
    ((klj_proxy*)klj_as_object(modern_proxy)->data)->disabled=1;
    klj_proxy_invoke(modern_proxy,PHOTON_CALLBACK,"OnData","()V",NULL);
    assert(modern_callbacks==2);
    ((klj_proxy*)klj_as_object(modern_proxy)->data)->disabled=0;
    return modern_proxy;
}
static void wait_callbacks(int count) {
    for(int i=0;i<2000 && atomic_load(&callbacks)<count;i++) usleep(1000);
    assert(atomic_load(&callbacks)>=count);
}
int main(void) {
    klj_val min_args[2]={{.j=24000},{.j=1}};
    assert(photon_min_buffer(NULL,NULL,min_args,2).j==960);
    min_args[1].j=3; assert((int)photon_min_buffer(NULL,NULL,min_args,2).j==-2);
    void *self=photon_init(NULL,NULL,NULL,0).l; assert(self);
    void *cb=kl_jni_new_object(KLJ_CLASS_PROXY);
    klj_val start[8]={{.l=kl_jni_activity()},{.l=cb},{.j=24000},{.j=1},{.j=960},{.j=1},{.j=1},{.j=1}};
    assert(!photon_start(NULL,self,start,8).j && !atomic_load(&opens));
    enabled=1; open_failure=1; assert(!photon_start(NULL,self,start,8).j); open_failure=0;
    kl_jni_local_frame_push(); test_buffer=klj_new_array('S',NULL,480);
    // Use Unity's actual CallBooleanMethodA path: a short[] is a jobject,
    // never a short scalar, and the full pointer must reach the recorder.
    void **jni=*(void***)kl_jni_env();
    void *(*lookup)(void*,void*,const char*,const char*)=jni[33];
    uint8_t (*call_bool_a)(void*,void*,void*,const klj_jvalue*)=jni[39];
    void *set=lookup(kl_jni_env(),kl_jni_class(PHOTON_AUDIO),"SetBuffer","([S)Z");
    klj_jvalue buffer={.l=test_buffer};
    assert(call_bool_a(kl_jni_env(),self,set,&buffer));
    assert(photon_state(self)->buffer==test_buffer);
    kl_jni_local_frame_pop(); assert(klj_arr(test_buffer)); // retained array survives
    assert(photon_start(NULL,self,start,8).j);
    assert(!photon_start(NULL,self,start,8).j); // no second ring reader
    assert(photon_rate(NULL,self,NULL,0).j==24000);
    wait_callbacks(2);
    assert(photon_stop(NULL,self,NULL,0).j); assert(atomic_load(&stops)==1);
    int before=atomic_load(&callbacks); usleep(15000); assert(atomic_load(&callbacks)==before);
    assert(!photon_state(self)->running && atomic_load(&closes)==1);
    assert(photon_start(NULL,self,start,8).j); wait_callbacks(before+2);
    assert(photon_stop(NULL,self,NULL,0).j); assert(atomic_load(&stops)==2 && atomic_load(&closes)==2);
    assert(photon_stop(NULL,self,NULL,0).j && atomic_load(&stops)==2);
    // The actual public JNI binding fixes the crashing method even with capture disabled.
    void **table=*(void***)kl_jni_env();
    void *(*method)(void*,void*,const char*,const char*)=table[33];
    int (*call)(void*,void*,void*,const void*)=table[51];
    void *mid=method(kl_jni_env(),kl_jni_class(PHOTON_AUDIO),"GetMinBufferSize","(II)I");
    klj_jvalue args[2]={{.i=48000},{.i=1}};
    assert(call(kl_jni_env(),self,mid,args)==1920);
    void *modern=reflection_and_proxy_contract(table);
    // Run real ReflectionHelper dispatch from the synthetic capture worker,
    // not just from the main thread or a mock callback seam.
    use_real_proxy=1; start[1].l=modern; before=atomic_load(&callbacks);
    assert(photon_start(NULL,self,start,8).j); wait_callbacks(before+2);
    assert(photon_stop(NULL,self,NULL,0).j);
    assert(atomic_load(&modern_callbacks)>=5);
    puts("Photon recorder JNI, opt-in refusal, PCM resampling, retained arrays, callback order, stop and restart passed");
}
