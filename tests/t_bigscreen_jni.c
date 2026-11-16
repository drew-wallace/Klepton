#include <assert.h>
#include <unistd.h>
#include "kl_driver.h"
#define kl_webview_command observed_webview_command
#define kl_webview_clear_data observed_webview_clear_data
#include "../runtime/jni/kl_jni_vuplex.c"
#undef kl_webview_command
#undef kl_webview_clear_data
#include "../runtime/jni/kl_jni_bigscreen.c"

static int global_pause_calls, expected_pause;
static int clear_data_calls;
void observed_webview_clear_data(void) {clear_data_calls++;}
void observed_webview_command(void *host,const char *command,int value) {
    if(!host)return;
    assert(host==(void *)1||host==(void *)2);
    assert(!strcmp(command,"paused")&&value==expected_pause);
    global_pause_calls++;
}

static klj_val call(const char *cls, const char *name, const char *signature,
                   uint64_t *args, int count, char result) {
    void *clazz = kl_jni_class(cls);
    void *method = klj_want(clazz, name, signature, 'M');
    kl_va va = {.gr_top = args ? args + count : NULL, .gr_offs = -8 * count};
    return klj_call(kl_jni_env(), clazz, method, &va, result);
}

static void call_view(void *object, const char *name) {
    void *method=klj_want(kl_jni_class("com/vuplex/webview/WebView3D"),name,"()V",'m');
    kl_va va={0};
    klj_call(kl_jni_env(),object,method,&va,'V');
}

static klj_val instance(void *object,const char *cls,const char *name,const char *signature,
                        uint64_t *args,int count,char result) {
    void *method=klj_want(kl_jni_class(cls),name,signature,'m');
    kl_va va={.gr_top=args?args+count:NULL,.gr_offs=-8*count};
    return klj_call(kl_jni_env(),object,method,&va,result);
}
static void audio_focus_contract(void) {
    const char *ab="android/media/AudioAttributes$Builder",*ac="android/media/AudioAttributes";
    const char *fb="android/media/AudioFocusRequest$Builder",*fc="android/media/AudioFocusRequest";
    void *builder=kl_jni_new_object(ab);
    uint64_t usage[]={14},content[]={2},gain[]={1},none[]={0};
    assert(instance(builder,ab,"setUsage","(I)Landroid/media/AudioAttributes$Builder;",usage,1,'L').l==builder);
    instance(builder,ab,"setContentType","(I)Landroid/media/AudioAttributes$Builder;",content,1,'L');
    void *attributes=instance(builder,ab,"build","()Landroid/media/AudioAttributes;",NULL,0,'L').l;
    instance(builder,ab,"setUsage","(I)Landroid/media/AudioAttributes$Builder;",none,1,'L');
    assert(instance(attributes,ac,"getUsage","()I",NULL,0,'I').j==14);
    assert(instance(attributes,ac,"getContentType","()I",NULL,0,'I').j==2);
    void *env=kl_jni_env();void **table=*(void ***)env;
    void *(*construct)(void *,void *,void *,const klj_jvalue *)=table[30]; // NewObjectA
    void *focus_class=kl_jni_class(fb);
    void *ctor=klj_want(focus_class,"<init>","(I)V",'m');
    klj_jvalue constructor_args[]={{.i=1}};
    void *focus=construct(env,focus_class,ctor,constructor_args);
    assert(focus&&focus!=focus_class&&!strcmp(klj_class_name(focus_class),fb));
    uint64_t attr_arg[]={(uintptr_t)attributes};
    instance(focus,fb,"setAudioAttributes","(Landroid/media/AudioAttributes;)Landroid/media/AudioFocusRequest$Builder;",attr_arg,1,'L');
    instance(focus,fb,"setAcceptsDelayedFocusGain","(Z)Landroid/media/AudioFocusRequest$Builder;",gain,1,'L');
    void *request=instance(focus,fb,"build","()Landroid/media/AudioFocusRequest;",NULL,0,'L').l;
    instance(focus,fb,"setAudioAttributes","(Landroid/media/AudioAttributes;)Landroid/media/AudioFocusRequest$Builder;",none,1,'L');
    assert(instance(request,fc,"getAudioAttributes","()Landroid/media/AudioAttributes;",NULL,0,'L').l==attributes);
    assert(instance(request,fc,"getFocusGain","()I",NULL,0,'I').j==1);
    assert(instance(request,fc,"acceptsDelayedFocusGain","()Z",NULL,0,'Z').j==1);
    void *manager=kl_jni_new_object("android/media/AudioManager");
    uint64_t request_arg[]={(uintptr_t)request};
    assert(instance(manager,"android/media/AudioManager","requestAudioFocus","(Landroid/media/AudioFocusRequest;)I",request_arg,1,'I').j==0);
    assert(instance(manager,"android/media/AudioManager","abandonAudioFocusRequest","(Landroid/media/AudioFocusRequest;)I",request_arg,1,'I').j==1);
    // Cross the JNI quarantine and recycle constructor payloads that own refs.
    // Their destructors run under the allocator lock and must not re-lock it.
    for(int i=0;i<10000;i++) {
        kl_jni_local_frame_push();
        void *temporary=construct(env,focus_class,ctor,constructor_args);
        instance(temporary,fb,"setAudioAttributes","(Landroid/media/AudioAttributes;)Landroid/media/AudioFocusRequest$Builder;",attr_arg,1,'L');
        kl_jni_local_frame_pop();
    }
    assert(instance(attributes,ac,"getUsage","()I",NULL,0,'I').j==14);
}
static void abi_fields_contract(void) {
    void *env=kl_jni_env();void **table=*(void ***)env;
    void *(*field)(void *,void *,const char *,const char *)=table[144];
    void *(*read)(void *,void *,void *)=table[145];
    void (*drop)(void *,void *)=table[23];
    void *cls=kl_jni_class("android/os/Build");
    const char *names[]={"SUPPORTED_ABIS","SUPPORTED_64_BIT_ABIS","SUPPORTED_32_BIT_ABIS"};
    for(int i=0;i<3;i++) {
        void *fid=field(env,cls,names[i],"[Ljava/lang/String;");
        kl_jni_local_frame_push();void *value=read(env,cls,fid);
        kl_jni_local_frame_pop();drop(env,value);
        assert(read(env,cls,fid)==value);
        klj_array *array=klj_arr(value);assert(array&&array->len==(i==2?0:1));
        if(i!=2)assert(!strcmp(klj_str(((void **)array->data)[0]),"arm64-v8a"));
    }
}


static pthread_t guest_thread;
static int delivered,script_results;
static int backtrace_reported;
static void *script_result(void *env,void *cls,int64_t handle,void *method,void *args) {
    assert(pthread_equal(pthread_self(),guest_thread));
    assert(handle==5678&&!strcmp(klj_str(method),"accept"));
    klj_array *array=klj_arr(args);assert(array&&array->kind=='L'&&array->len==1);
    void *string=((void**)array->data)[0];
    // Unity's _AndroidJNIHelper.Unbox converts the callback argument this way.
    void *mid=klj_want(kl_jni_class("java/lang/String"),"toString","()Ljava/lang/String;",'m');
    kl_va va={0};void *result=klj_call(env,string,mid,&va,'L').l;
    assert(result==string&&!strcmp(klj_str(result),"{\"playing\":true}"));
    script_results++;return NULL;
}
static void unity_message(void *env,void *cls,void *target,void *method,void *bytes) {
    assert(pthread_equal(pthread_self(),guest_thread));
    assert(env==kl_jni_env());
    if(!strcmp(klj_str(target),"BacktraceFixture")) {
        assert(!strcmp(klj_str(method),"OnException"));
        klj_array *data=klj_arr(bytes);
        assert(data&&data->len==strlen("error\nstack")&&!memcmp(data->data,"error\nstack",data->len));
        backtrace_reported++;return;
    }
    assert(!strcmp(klj_str(target),"BrowserFixture"));
    klj_array *array=klj_arr(bytes);assert(array&&array->kind=='B');
    if(delivered==0)assert(!strcmp(klj_str(method),"HandleLoadFailed"));
    else {
        assert(!strcmp(klj_str(method),"HandleMessageEmitted"));
        assert(array->len==strlen("hello π")&&!memcmp(array->data,"hello π",array->len));
    }
    delivered++;
}
static void *background_browser_event(void *id) {
    browser_event_received((uintptr_t)id,"HandleMessageEmitted","hello π",0);
    return NULL;
}
static void backtrace_contract(void) {
    const char *cls="backtraceio/unity/BacktraceAndroidBackgroundUnhandledExceptionHandler";
    void *env=kl_jni_env();void **table=*(void ***)env;
    void *(*construct)(void *,void *,void *,const klj_jvalue *)=table[30];
    void *clazz=kl_jni_class(cls),*ctor=klj_want(clazz,"<init>","(Ljava/lang/String;Ljava/lang/String;)V",'m');
    kl_jni_local_frame_push();
    klj_jvalue args[]={{.l=kl_jni_new_string("BacktraceFixture")},{.l=kl_jni_new_string("OnException")}};
    void *handler=construct(env,clazz,ctor,args);kl_jni_pin_object(handler);
    kl_jni_local_frame_pop();
    instance(handler,cls,"stopMonitoring","()V",NULL,0,'V');
    uint64_t report[]={(uintptr_t)kl_jni_new_string("error"),(uintptr_t)kl_jni_new_string("stack")};
    instance(handler,cls,"ReportThreadException","(Ljava/lang/String;Ljava/lang/String;)V",report,2,'V');
    instance(handler,cls,"finish","()V",NULL,0,'V');
    assert(backtrace_reported==1);
    kl_jni_unpin_object(handler);
}
static void browser_callback_contract(void) {
    guest_thread=pthread_self();
    void *env=kl_jni_env();void **table=*(void ***)env;
    int (*reg)(void*,void*,const kl_jni_method*,int)=table[215];
    kl_jni_method message={"nativeUnitySendMessage","(Ljava/lang/String;Ljava/lang/String;[B)V",unity_message};
    assert(!reg(env,kl_jni_class("com/unity3d/player/UnityPlayer"),&message,1));
    // Invalid dimensions must report failure rather than successful init. The
    // callback still needs the Unity destination even though no host view exists.
    // Vegas CP178.7 requests the two-boolean constructor with an Object
    // message parameter when its player suite creates the browser surface.
    // Exercise NewObjectA so an absent exact JNI binding cannot go unnoticed.
    void *clazz=kl_jni_class("com/vuplex/webview/WebView3D");
    void *ctor=klj_want(clazz,"<init>","(Ljava/lang/String;IIIZZLcom/vuplex/webview/BiConsumer;Ljava/lang/Object;)V",'m');
    void *(*construct)(void *,void *,void *,const klj_jvalue *)=table[30];
    klj_jvalue args[8]={{.l=kl_jni_new_string("BrowserFixture")},{.i=42},{.i=0},{.i=240},{.z=1}};
    void *obj=construct(env,clazz,ctor,args);
    browser_view *view=browser_of(obj);assert(view&&!view->host);
    assert(view->texture==42&&view->vulkan&&!strcmp(view->target,"BrowserFixture"));
    uint64_t keyboard_on[]={1},keyboard_off[]={0};
    instance(obj,"com/vuplex/webview/WebView3D","setNativeOnScreenKeyboardEnabled","(Z)V",keyboard_on,1,'V');
    assert(view->native_keyboard_requested==1);
    instance(obj,"com/vuplex/webview/WebView3D","setNativeOnScreenKeyboardEnabled","(Z)V",keyboard_off,1,'V');
    assert(view->native_keyboard_requested==0);
    // The post-login transition invokes inherited instance methods, including
    // on a view whose initialization failed. These must never abort the guest.
    call_view(obj,"pause"); call_view(obj,"resume");
    void *agent_mode=klj_want(kl_jni_class("com/vuplex/webview/WebView3D"),"setUserAgent","(Z)V",'m');
    uint64_t mobile_arg[]={1};
    kl_va agent_va={.gr_top=mobile_arg+1,.gr_offs=-8};
    klj_call(env,obj,agent_mode,&agent_va,'V');
    assert(global_user_agent_mode==-1); // Per-view policy must remain local.
    void *focus_events=klj_want(kl_jni_class("com/vuplex/webview/WebView3D"),"setFocusedInputFieldEventsEnabled","(Z)V",'m');
    kl_va focus_va={.gr_top=mobile_arg+1,.gr_offs=-8};
    klj_call(env,obj,focus_events,&focus_va,'V');
    pthread_t worker;assert(!pthread_create(&worker,NULL,background_browser_event,(void*)(uintptr_t)view->id));
    pthread_join(worker,NULL);assert(delivered==0);
    klj_vuplex_drain_events();assert(delivered==2);
    // Both Unity generations create the same proxy representation.
    const char *helper="com/unity3d/player/ReflectionHelper";
    uint64_t older[]={1234,(uintptr_t)kl_jni_class("com/vuplex/webview/Consumer")};
    void *proxy=call(helper,"newProxyInstance","(JLjava/lang/Class;)Ljava/lang/Object;",older,2,'L').l;
    klj_proxy *p=klj_as_object(proxy)->data;assert(p->native_ptr==1234&&p->reflection_helper);
    assert(!strcmp(klj_class_name(((void**)klj_arr(p->classes)->data)[0]),"com/vuplex/webview/Consumer"));
    uint64_t newer[]={0,5678,older[1]};
    proxy=call(helper,"newProxyInstance","(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;",newer,3,'L').l;
    p=klj_as_object(proxy)->data;assert(p->native_ptr==5678&&p->reflection_helper);
    kl_jni_method invoke={"nativeProxyInvoke","(JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;",script_result};
    assert(!reg(env,kl_jni_class(helper),&invoke,1));
    // Deliver an actual queued JavaScript completion through the proxy bridge.
    browser_request *request=calloc(1,sizeof *request);assert(request);
    request->id=++next_request_id;request->view=view->id;request->proxy=proxy;
    kl_jni_pin_object(proxy);unsigned pin_count=klj_as_object(proxy)->pinned;
    request->next=requests;requests=request;
    browser_event_received(view->id,"","{\"playing\":true}",request->id);
    klj_vuplex_drain_events();assert(script_results==1&&!requests);
    assert(klj_as_object(proxy)->pinned==pin_count-1);
    browser_destroy(env,obj,NULL,0);
    call_view(obj,"pause"); call_view(obj,"resume");
    background_browser_event((void*)(uintptr_t)view->id);
    klj_vuplex_drain_events();assert(delivered==2);
    uint64_t java_message[]={(uintptr_t)args[0].l,(uintptr_t)kl_jni_new_string("HandleMessageEmitted"),(uintptr_t)kl_jni_new_string("hello π")};
    call("com/unity3d/player/UnityPlayer","UnitySendMessage","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",java_message,3,'V');
    assert(delivered==3);
}
int main(void) {
    alarm(30);
    abi_fields_contract();
    audio_focus_contract();
    call("com/vuplex/webview/WebView3D","clearAllData","()V",NULL,0,'V');
    assert(clear_data_calls==1);
    setenv("KL_UNITY_ARGS", "-force-gles30", 1);
    assert(!strcmp(kl_driver_unity_arguments(), "-force-gles30"));
    setenv("KL_UNITY_ARGS", "", 1);
    assert(!*kl_driver_unity_arguments());
    unsetenv("KL_UNITY_ARGS");
    browser_view live2={.host=(void *)2}, dead={.host=(void *)3,.destroyed=1,.next=&live2};
    browser_view empty={.next=&dead}, live1={.host=(void *)1,.next=&empty};
    browsers=&live1;
    expected_pause=1;
    call("com/vuplex/webview/WebView3D","pauseAll","()V",NULL,0,'V');
    assert(global_pause_calls==2);
    expected_pause=0;
    call("com/vuplex/webview/WebView3D","resumeAll","()V",NULL,0,'V');
    assert(global_pause_calls==4);
    browsers=NULL;
    call("com/vuplex/webview/WebView3D","resumeAll","()V",NULL,0,'V');
    assert(global_pause_calls==4);
    uint64_t cookie[]={(uintptr_t)kl_jni_new_string("not valid JSON")};
    call("com/vuplex/webview/WebView3D","setCookie","(Ljava/lang/String;)V",cookie,1,'V');
    uint64_t yes[] = {1}, no[] = {0};
    call("com/vuplex/webview/WebView3D", "setAlternativePointerInputSystemEnabled", "(Z)V", yes, 1, 'V');
    assert(atomic_load(&alternative_pointer_input) == 1);
    call("com/vuplex/webview/WebView", "setAlternativePointerInputSystemEnabled", "(Z)V", no, 1, 'V');
    assert(atomic_load(&alternative_pointer_input) == 0);
    call("com/vuplex/webview/WebView3D", "setAlternativeKeyboardInputSystemEnabled", "(Z)V", yes, 1, 'V');
    assert(atomic_load(&alternative_keyboard_input) == 1);

    call("com/vuplex/webview/WebView3D", "setAutoplayEnabled", "(Z)V", yes, 1, 'V');
    call("com/vuplex/webview/WebView3D", "setDrmEnabled", "(Z)V", yes, 1, 'V');
    assert(atomic_load(&autoplay_enabled) == 1 && atomic_load(&drm_requested) == 1);
    assert(call("com/vuplex/webview/WebView3D", "isWebViewAvailable", "()Z", NULL, 0, 'Z').j == 1);
    uint64_t agent[] = {(uintptr_t)kl_jni_new_string("First agent")};
    call("com/vuplex/webview/WebView3D", "globallySetUserAgent", "(Ljava/lang/String;)V", agent, 1, 'V');
    assert(!strcmp(global_user_agent, "First agent"));
    uint64_t clear_agent[] = {0};
    call("com/vuplex/webview/WebView3D", "globallySetUserAgent", "(Ljava/lang/String;)V", clear_agent, 1, 'V');
    assert(global_user_agent == NULL);
    call("com/vuplex/webview/WebView3D", "globallySetUserAgent", "(Z)V", no, 1, 'V');
    assert(global_user_agent_mode==0 && global_user_agent==NULL);
    call("com/vuplex/webview/WebView3D", "globallySetUserAgent", "(Z)V", yes, 1, 'V');
    assert(global_user_agent_mode==1 && global_user_agent==NULL);
    call("com/vuplex/webview/WebView3D", "globallySetUserAgent", "(Ljava/lang/String;)V", agent, 1, 'V');
    assert(global_user_agent_mode==-1 && !strcmp(global_user_agent,"First agent"));
    call("com/vuplex/webview/WebView3D", "globallySetUserAgent", "(Ljava/lang/String;)V", clear_agent, 1, 'V');

    const char *callbacks = "com/bigscreenvr/bigmediaplayer/BigMediaPlayerCallbacks";
    uint64_t first[] = {(uintptr_t)kl_jni_new_string("FirstObject"), (uintptr_t)kl_jni_new_string("OnError")};
    uint64_t second[] = {(uintptr_t)kl_jni_new_string("SecondObject"), (uintptr_t)kl_jni_new_string("OnReady")};
    call(callbacks, "RegisterDRMLicenseErrorCallback", "(Ljava/lang/String;Ljava/lang/String;)V", first, 2, 'V');
    call(callbacks, "RegisterPlaybackReadyCallback", "(Ljava/lang/String;Ljava/lang/String;)V", second, 2, 'V');
    assert(!strcmp(media_callbacks[1].object, "FirstObject"));
    assert(!strcmp(media_callbacks[8].object, "SecondObject"));
    call(callbacks, "RegisterDRMLicenseErrorCallback", "(Ljava/lang/String;Ljava/lang/String;)V", second, 2, 'V');
    assert(!strcmp(media_callbacks[1].method, "OnReady"));
    // Callback registration only saves destinations; it produces no events.

    const char *api = "com/bigscreenvr/bigmediaplayer/BigMediaPlayerAPI";
    void *context = kl_jni_new_object("com/unity3d/player/UnityPlayerActivity");
    uint64_t activity[] = {(uintptr_t)context}, clear[] = {0};
    unsigned pinned = klj_as_object(context)->pinned;
    call(api, "SetMediaPlayerContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", activity, 1, 'V');
    assert(media_context == context && klj_as_object(context)->pinned == pinned + 1);
    call(api, "SetMediaPlayerContext", "(Landroid/content/Context;)V", clear, 1, 'V');
    assert(media_context == NULL && klj_as_object(context)->pinned == pinned);
    assert(call(api, "CreateBigMediaPlayer", "()I", NULL, 0, 'I').j == 0);
    assert(call(api, "CreateBigMediaPlayer", "()I", NULL, 0, 'I').j == 1);
    assert(find_player(0) && find_player(1) && !find_player(2));
    assert(find_player(0)->texture == 0 && find_player(1)->texture == 0);
    const char *web = "com/bigscreenvr/webview/BigWebView";
    assert((int)call(web, "GetSharedTextureHandle", "()I", NULL, 0, 'I').j == -1);
    assert(call(web, "CreateBigWebClient", "()I", NULL, 0, 'I').j == 0);
    assert(call(web, "CreateBigWebClient", "()I", NULL, 0, 'I').j == 1);
    uint64_t web_id[]={0};
    assert(!call(web,"IsFullScreen","(I)Z",web_id,1,'Z').j);
    call(web,"ExitFullScreen","(I)V",web_id,1,'V');
    call(web, "SetContext", "(Lcom/unity3d/player/UnityPlayerActivity;)V", activity, 1, 'V');
    assert(web_context == context && klj_as_object(context)->pinned == pinned + 1);
    call(web, "SetContext", "(Landroid/content/Context;)V", clear, 1, 'V');
    assert(web_context == NULL && klj_as_object(context)->pinned == pinned);
    klj_val drm = call(api, "GetDRMInfo", "()Ljava/lang/String;", NULL, 0, 'L');
    assert(!strcmp(klj_str(drm.l), "Not available."));
    browser_callback_contract();
    backtrace_contract();
    puts("Bigscreen JNI: inherited input flags, isolated/replaced callbacks, context ownership, unavailable DRM");
}
