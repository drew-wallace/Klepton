// Vuplex Java platform adapter backed by Apple WebKit. Browser events are
// queued on the main thread and delivered only from the attached guest thread.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "kl_jni_int.h"
#include "kl_webview.h"
#include "kl_glfb.h"
#include "kl_egl.h"
#include "kl_jni_vuplex.h"

static atomic_int alternative_pointer_input;
static atomic_int alternative_keyboard_input;
static atomic_int autoplay_enabled;
static atomic_int drm_requested;
static pthread_mutex_t user_agent_lock = PTHREAD_MUTEX_INITIALIZER;
static char *global_user_agent;
static int global_user_agent_mode=-1;
static void apply_global_user_agent(void);

static klj_val klj_vuplex_user_agent(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    const char *value = klj_str(args[0].l);
    char *copy = value ? strdup(value) : NULL;
    pthread_mutex_lock(&user_agent_lock);
    free(global_user_agent);
    global_user_agent = copy;
    global_user_agent_mode=-1;
    pthread_mutex_unlock(&user_agent_lock);
    apply_global_user_agent();
    KLJ_LOG("Vuplex global user agent recorded");
    return (klj_val){0};
}

static klj_val klj_vuplex_user_agent_mode(void *env,void *self,const klj_val *args,int count) {
    pthread_mutex_lock(&user_agent_lock);
    free(global_user_agent);global_user_agent=NULL;
    global_user_agent_mode=!!args[0].j;
    pthread_mutex_unlock(&user_agent_lock);
    apply_global_user_agent();
    return (klj_val){0};
}

static klj_val klj_vuplex_pointer_input(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    atomic_store(&alternative_pointer_input, !!args[0].j);
    KLJ_LOG("Vuplex alternative pointer input = %d", atomic_load(&alternative_pointer_input));
    return (klj_val){0};
}

static klj_val klj_vuplex_keyboard_input(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    atomic_store(&alternative_keyboard_input, !!args[0].j);
    KLJ_LOG("Vuplex alternative keyboard input = %d", atomic_load(&alternative_keyboard_input));
    return (klj_val){0};
}

static klj_val klj_vuplex_autoplay(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    atomic_store(&autoplay_enabled, !!args[0].j);
    // Consumed when a native view is configured, before its first navigation.
    return (klj_val){0};
}

static klj_val klj_vuplex_drm(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)count;
    // This setter requests a browser policy. It does not provision Widevine.
    atomic_store(&drm_requested, !!args[0].j);
    return (klj_val){0};
}

static klj_val klj_vuplex_available(void *env, void *self, const klj_val *args, int count) {
    (void)env; (void)self; (void)args; (void)count;
    return (klj_val){.j=1};
}


typedef struct browser_view {
    uint64_t id, serial;
    unsigned texture, refs;
    int destroyed, uploaded_width, uploaded_height, rendered;
    char *target;
    void *host;
    struct browser_view *next;
} browser_view;
typedef struct browser_event {
    uint64_t id, request;
    char *method, *value;
    struct browser_event *next;
} browser_event;
typedef struct browser_request {
    uint64_t id, view;
    void *proxy;
    struct browser_request *next;
} browser_request;
static pthread_mutex_t browser_lock = PTHREAD_MUTEX_INITIALIZER;
static browser_view *browsers;
static browser_event *events_head, *events_tail;
static browser_request *requests;
static uint64_t next_browser_id, next_request_id;
static size_t event_bytes;
static unsigned event_count;

static void apply_global_user_agent(void) {
    pthread_mutex_lock(&user_agent_lock);
    pthread_mutex_lock(&browser_lock);
    for(browser_view *v=browsers;v;v=v->next) {
        if(global_user_agent_mode>=0)kl_webview_user_agent_mode(v->host,global_user_agent_mode);
        else kl_webview_user_agent(v->host,global_user_agent);
    }
    pthread_mutex_unlock(&browser_lock);
    pthread_mutex_unlock(&user_agent_lock);
}
static void browser_event_received(uint64_t id,const char *method,const char *value,uint64_t request) {
    size_t bytes=strnlen(value,1024*1024+1);
    pthread_mutex_lock(&browser_lock);
    if(bytes>1024*1024 || event_count>=256 || event_bytes+bytes>4*1024*1024) {
        pthread_mutex_unlock(&browser_lock);
        KLJ_LOG_ONCE("WebKit event queue limit reached; dropping excess page messages");
        return;
    }
    browser_event *e=calloc(1,sizeof *e);
    if (!e) { pthread_mutex_unlock(&browser_lock); return; }
    e->id=id; e->request=request; e->method=strdup(method); e->value=strdup(value);
    if(!e->method||!e->value) {free(e->method);free(e->value);free(e);pthread_mutex_unlock(&browser_lock);return;}
    if (events_tail) events_tail->next=e; else events_head=e;
    events_tail=e; event_count++; event_bytes+=bytes;
    pthread_mutex_unlock(&browser_lock);
}
static void browser_release(void *data);
static browser_view *browser_of(void *obj) {
    klj_object *o=klj_as_object(obj);
    if (!o || !o->data || o->destroy!=browser_release || (strcmp(o->cls,"com/vuplex/webview/WebView3D") && strcmp(o->cls,"android/webkit/WebView"))) return NULL;
    return o->data;
}
int klj_vuplex_is_view(void *obj) { return browser_of(obj)!=NULL; }
static void browser_release(void *data) {
    browser_view *v=data;
    pthread_mutex_lock(&browser_lock);
    if (--v->refs) { pthread_mutex_unlock(&browser_lock); return; }
    browser_view **slot=&browsers;
    while (*slot && *slot!=v) slot=&(*slot)->next;
    if (*slot) *slot=v->next;
    pthread_mutex_unlock(&browser_lock);
    kl_webview_close(v->host); free(v->target); free(v);
}
void klj_vuplex_drain_events(void) {
    pthread_mutex_lock(&browser_lock);
    browser_event *events=events_head; events_head=events_tail=NULL; event_count=0;event_bytes=0;
    pthread_mutex_unlock(&browser_lock);
    while (events) {
        browser_event *e=events; events=e->next;
        browser_view *v=NULL;
        pthread_mutex_lock(&browser_lock);
        for (browser_view *p=browsers;p;p=p->next) if(p->id==e->id) {v=p;break;}
        char *target=v&&!v->destroyed?strdup(v->target):NULL;
        pthread_mutex_unlock(&browser_lock);
        kl_jni_local_frame_push();
        if (e->request) {
            pthread_mutex_lock(&browser_lock);
            browser_request **slot=&requests;
            while (*slot && (*slot)->id!=e->request) slot=&(*slot)->next;
            browser_request *r=*slot;
            if(r)*slot=r->next;
            pthread_mutex_unlock(&browser_lock);
            if (r) {
                if (target) {
                    void *array=klj_new_array('L',"java/lang/Object",1);
                    ((void **)klj_arr(array)->data)[0]=kl_jni_new_string(e->value);
                    klj_proxy_invoke(r->proxy,"com/vuplex/webview/Consumer","accept","(Ljava/lang/Object;)V",array);
                }
                kl_jni_unpin_object(r->proxy); free(r);
            }
        } else if (target) {
            void (*send)(void *,void *,void *,void *,void *)=kl_jni_native("com/unity3d/player/UnityPlayer","nativeUnitySendMessage","(Ljava/lang/String;Ljava/lang/String;[B)V");
            if (send) {
                size_t len=strlen(e->value);
                void *bytes=klj_new_array('B',NULL,(int)len);
                memcpy(klj_arr(bytes)->data,e->value,len);
                send(kl_jni_env(),kl_jni_class("com/unity3d/player/UnityPlayer"),kl_jni_new_string(target),kl_jni_new_string(e->method),bytes);
            } else KLJ_LOG("WebView event %s: Unity native message entry point unavailable",e->method);
        }
        kl_jni_local_frame_pop();
        free(target); free(e->method); free(e->value); free(e);
    }
    // Cancel callbacks belonging to closed/collected views on the guest thread.
    browser_request *cancelled=NULL;
    pthread_mutex_lock(&browser_lock);
    browser_request **slot=&requests;
    while (*slot) {
        browser_request *r=*slot; int alive=0;
        for(browser_view *v=browsers;v;v=v->next) if(v->id==r->view&&v->host) alive=1;
        if(alive) slot=&r->next;
        else { *slot=r->next; r->next=cancelled; cancelled=r; }
    }
    pthread_mutex_unlock(&browser_lock);
    while(cancelled) {browser_request *r=cancelled;cancelled=r->next;kl_jni_unpin_object(r->proxy);free(r);}
}
static klj_val browser_create(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=calloc(1,sizeof *v);
    if (!v) return (klj_val){0};
    v->target=strdup(klj_str(a[0].l)?:""); v->texture=(unsigned)a[1].j; v->refs=1;
    pthread_mutex_lock(&browser_lock);
    v->id=++next_browser_id; v->next=browsers; browsers=v;
    pthread_mutex_unlock(&browser_lock);
    pthread_mutex_lock(&user_agent_lock);
    pthread_mutex_lock(&browser_lock);
    v->host=kl_webview_create(v->id,(int)a[2].j,(int)a[3].j,global_user_agent,atomic_load(&autoplay_enabled),browser_event_received);
    if(global_user_agent_mode>=0)kl_webview_user_agent_mode(v->host,global_user_agent_mode);
    pthread_mutex_unlock(&browser_lock);
    pthread_mutex_unlock(&user_agent_lock);
    KLJ_LOG("Vuplex WebView %llu target=%s texture=%u size=%dx%d",(unsigned long long)v->id,v->target,v->texture,(int)a[2].j,(int)a[3].j);
    if (!v->host) browser_event_received(v->id,"HandleLoadFailed","-1,invalid WebView dimensions",0);
    return (klj_val){.l=klj_own(klj_new_object_data("com/vuplex/webview/WebView3D",v),browser_release)};
}
static klj_val browser_native(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=browser_of(self); if(!v) return (klj_val){0};
    pthread_mutex_lock(&browser_lock); v->refs++; pthread_mutex_unlock(&browser_lock);
    return (klj_val){.l=klj_own(klj_new_object_data("android/webkit/WebView",v),browser_release)};
}
static klj_val browser_destroy(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=browser_of(self);
    if(v) {
        pthread_mutex_lock(&browser_lock);
        void *host=v->host; v->host=NULL; v->destroyed=1;
        pthread_mutex_unlock(&browser_lock);
        kl_webview_close(host);
    }
    return (klj_val){0};
}
// Serialize handle capture with destroy. WebKit commands only enqueue main
// queue work, so none can call back into JNI while this lock is held.
#define WITH_BROWSER(v,code) do { pthread_mutex_lock(&browser_lock); if(v) {code;} pthread_mutex_unlock(&browser_lock); } while(0)
static klj_val browser_url(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=browser_of(self); WITH_BROWSER(v,kl_webview_load(v->host,klj_str(a[0].l),kl_jni_assets_dir(),0)); return (klj_val){0};
}
static klj_val browser_html(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=browser_of(self); WITH_BROWSER(v,kl_webview_load(v->host,klj_str(a[0].l),n>1?klj_str(a[1].l):NULL,1)); return (klj_val){0};
}
static klj_val browser_js(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=browser_of(self); if(!v) return (klj_val){0};
    browser_request *r=a[1].l?calloc(1,sizeof *r):NULL;
    if(r) {r->proxy=a[1].l;kl_jni_pin_object(r->proxy);}
    pthread_mutex_lock(&browser_lock);
    if(v->host) {
        if(r) {r->id=++next_request_id;r->view=v->id;r->next=requests;requests=r;}
        kl_webview_eval(v->host,klj_str(a[0].l),r?r->id:0);
        r=NULL;
    }
    pthread_mutex_unlock(&browser_lock);
    if(r) {kl_jni_unpin_object(r->proxy);free(r);}
    return (klj_val){0};
}
static klj_val browser_resize(void *env,void *self,const klj_val *a,int n) {
    browser_view *v=browser_of(self); WITH_BROWSER(v,kl_webview_resize(v->host,(int)a[0].j,(int)a[1].j)); return (klj_val){0};
}
void klj_vuplex_render(void *object) {
    browser_view *v=browser_of(object);
    pthread_mutex_lock(&browser_lock);
    if(!v||!v->host||!v->texture) {pthread_mutex_unlock(&browser_lock);return;}
    if(!v->rendered)KLJ_LOG("WebKit render view=%llu context=%p",(unsigned long long)v->id,kl_egl_current_context());
    v->rendered=1;
    uint64_t serial=v->serial; int w=0,h=0;
    void *pixels=kl_webview_copy_frame(v->host,&serial,&w,&h);
    if(pixels) {
        if(kl_glfb_upload_webview_bgra(v->texture,w,h,pixels,w!=v->uploaded_width||h!=v->uploaded_height)) {
            if(!v->serial)KLJ_LOG("WebKit first texture upload view=%llu texture=%u %dx%d",(unsigned long long)v->id,v->texture,w,h);
            v->serial=serial; v->uploaded_width=w; v->uploaded_height=h;
        }
        free(pixels);
    }
    pthread_mutex_unlock(&browser_lock);
}

static klj_val browser_render(void *env,void *self,const klj_val *a,int n) { klj_vuplex_render(self);return (klj_val){0}; }
#define COMMAND(name,cmd,flag) static klj_val name(void *env,void *self,const klj_val *a,int n) {browser_view *v=browser_of(self);WITH_BROWSER(v,kl_webview_command(v->host,cmd,flag));return (klj_val){0};}
COMMAND(browser_back,"back",0)
COMMAND(browser_forward,"forward",0)
COMMAND(browser_reload,"reload",0)
COMMAND(browser_pause,"paused",1)
COMMAND(browser_resume,"paused",0)
COMMAND(browser_focus,"focus",(int)a[0].j)
COMMAND(browser_rendering,"rendering",(int)a[0].j)
COMMAND(browser_focus_events,"focusedInputFieldEvents",(int)a[0].j)
int klj_vuplex_is_fullscreen(void *object) {
    int result=0;browser_view *v=browser_of(object);
    WITH_BROWSER(v,result=kl_webview_is_fullscreen(v->host));return result;
}
void klj_vuplex_exit_fullscreen(void *object) {
    browser_view *v=browser_of(object);WITH_BROWSER(v,kl_webview_command(v->host,"exitFullscreen",0));
}
static klj_val browser_agent(void *env,void *self,const klj_val *a,int n) {browser_view *v=browser_of(self);WITH_BROWSER(v,kl_webview_user_agent(v->host,klj_str(a[0].l)));return (klj_val){0};}
static klj_val browser_agent_mode(void *env,void *self,const klj_val *a,int n) {browser_view *v=browser_of(self);WITH_BROWSER(v,kl_webview_user_agent_mode(v->host,!!a[0].j));return (klj_val){0};}
void klj_vuplex_pointer(void *object,const char *type,int x,int y,int button) { browser_view *v=browser_of(object);WITH_BROWSER(v,kl_webview_pointer(v->host,type,x,y,button==1?2:button==2?1:0)); }
void klj_vuplex_scroll(void *object,int dx,int dy,int x,int y) {browser_view *v=browser_of(object);WITH_BROWSER(v,kl_webview_scroll(v->host,dx,dy,x,y));}
#define POINTER(name,type) static klj_val name(void *env,void *self,const klj_val *a,int n) {klj_vuplex_pointer(self,type,(int)a[0].j,(int)a[1].j,n>3?(int)a[2].j:0);return (klj_val){0};}
POINTER(browser_down,"pointerdown")
POINTER(browser_up,"pointerup")
POINTER(browser_move,"pointermove")
static klj_val browser_click(void *env,void *self,const klj_val *a,int n) { browser_down(env,self,a,2);browser_up(env,self,a,2);return (klj_val){0}; }
static klj_val browser_scroll(void *env,void *self,const klj_val *a,int n) {klj_vuplex_scroll(self,(int)a[0].j,(int)a[1].j,n>2?(int)a[2].j:0,n>2?(int)a[3].j:0);return (klj_val){0};}
static klj_val browser_key(void *env,void *self,const klj_val *a,int n) {browser_view *v=browser_of(self);WITH_BROWSER(v,kl_webview_key(v->host,klj_str(a[0].l)));return (klj_val){0};}

const klj_binding klj_bind_vuplex[] = {
    {"com/vuplex/webview/WebView3D", "<init>", "(Ljava/lang/String;IIIZZZLcom/vuplex/webview/BiConsumer;Ljava/lang/Object;)V", browser_create},

    {"com/vuplex/webview/WebView", "globallySetUserAgent", "(Ljava/lang/String;)V", klj_vuplex_user_agent},
    {"com/vuplex/webview/WebView", "globallySetUserAgent", "(Z)V", klj_vuplex_user_agent_mode},
    {"com/vuplex/webview/WebView", "setAutoplayEnabled", "(Z)V", klj_vuplex_autoplay},
    {"com/vuplex/webview/WebView", "setDrmEnabled", "(Z)V", klj_vuplex_drm},
    {"com/vuplex/webview/WebView", "isWebViewAvailable", "()Z", klj_vuplex_available},
    {"com/vuplex/webview/WebView", "setAlternativePointerInputSystemEnabled", "(Z)V", klj_vuplex_pointer_input},
    {"com/vuplex/webview/WebView", "setAlternativeKeyboardInputSystemEnabled", "(Z)V", klj_vuplex_keyboard_input},
    {"com/vuplex/webview/WebView3D", "<init>", "(Ljava/lang/String;IIIZZZLcom/vuplex/webview/BiConsumer;Landroid/os/Message;)V", browser_create},
    {"com/vuplex/webview/WebView", "getNativeWebView", "()Landroid/webkit/WebView;", browser_native},
    {"com/vuplex/webview/WebView", "destroy", "()V", browser_destroy},
    {"com/vuplex/webview/WebView", "loadUrl", "(Ljava/lang/String;)V", browser_url},
    {"com/vuplex/webview/WebView", "loadHtml", "(Ljava/lang/String;)V", browser_html},
    {"com/vuplex/webview/WebView", "loadHtml", "(Ljava/lang/String;Ljava/lang/String;)V", browser_html},
    {"com/vuplex/webview/WebView", "executeJavaScript", "(Ljava/lang/String;Lcom/vuplex/webview/Consumer;)V", browser_js},
    {"com/vuplex/webview/WebView", "resize", "(II)V", browser_resize},
    {"com/vuplex/webview/WebView", "render", "()V", browser_render},
    {"com/vuplex/webview/WebView", "goBack", "()V", browser_back},
    {"com/vuplex/webview/WebView", "goForward", "()V", browser_forward},
    {"com/vuplex/webview/WebView", "reload", "()V", browser_reload},
    {"com/vuplex/webview/WebView", "pause", "()V", browser_pause},
    {"com/vuplex/webview/WebView", "resume", "()V", browser_resume},
    {"com/vuplex/webview/WebView", "setFocused", "(Z)V", browser_focus},
    {"com/vuplex/webview/WebView", "setFocusedInputFieldEventsEnabled", "(Z)V", browser_focus_events},
    {"com/vuplex/webview/WebView", "setRenderingEnabled", "(Z)V", browser_rendering},
    {"com/vuplex/webview/WebView", "setUserAgent", "(Ljava/lang/String;)V", browser_agent},
    {"com/vuplex/webview/WebView", "setUserAgent", "(Z)V", browser_agent_mode},
    {"com/vuplex/webview/WebView", "click", "(IIZ)V", browser_click},
    {"com/vuplex/webview/WebView", "movePointer", "(IIZ)V", browser_move},
    {"com/vuplex/webview/WebView", "pointerDown", "(IIIIZ)V", browser_down},
    {"com/vuplex/webview/WebView", "pointerUp", "(IIII)V", browser_up},
    {"com/vuplex/webview/WebView", "scroll", "(II)V", browser_scroll},
    {"com/vuplex/webview/WebView", "scroll", "(IIII)V", browser_scroll},
    {"com/vuplex/webview/WebView", "sendKey", "(Ljava/lang/String;)V", browser_key},
    {NULL, NULL, NULL, NULL},
};
