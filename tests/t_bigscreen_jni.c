#include <assert.h>
#include "../runtime/jni/kl_jni_vuplex.c"
#include "../runtime/jni/kl_jni_bigscreen.c"

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


static pthread_t guest_thread;
static int delivered;
static void unity_message(void *env,void *cls,void *target,void *method,void *bytes) {
    assert(pthread_equal(pthread_self(),guest_thread));
    assert(env==kl_jni_env());
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
static void browser_callback_contract(void) {
    guest_thread=pthread_self();
    void *env=kl_jni_env();void **table=*(void ***)env;
    int (*reg)(void*,void*,const kl_jni_method*,int)=table[215];
    kl_jni_method message={"nativeUnitySendMessage","(Ljava/lang/String;Ljava/lang/String;[B)V",unity_message};
    assert(!reg(env,kl_jni_class("com/unity3d/player/UnityPlayer"),&message,1));
    // Invalid dimensions must report failure rather than successful init. The
    // callback still needs the Unity destination even though no host view exists.
    klj_val args[9]={{.l=kl_jni_new_string("BrowserFixture")},{0},{.j=0},{.j=240}};
    void *obj=browser_create(env,NULL,args,9).l;browser_view *view=browser_of(obj);assert(view&&!view->host);
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
    browser_destroy(env,obj,NULL,0);
    call_view(obj,"pause"); call_view(obj,"resume");
    background_browser_event((void*)(uintptr_t)view->id);
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
    uint64_t java_message[]={(uintptr_t)args[0].l,(uintptr_t)kl_jni_new_string("HandleMessageEmitted"),(uintptr_t)kl_jni_new_string("hello π")};
    call("com/unity3d/player/UnityPlayer","UnitySendMessage","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",java_message,3,'V');
    assert(delivered==3);
}
int main(void) {
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
    puts("Bigscreen JNI: inherited input flags, isolated/replaced callbacks, context ownership, unavailable DRM");
}
