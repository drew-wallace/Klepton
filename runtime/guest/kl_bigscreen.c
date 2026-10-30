// The Quest build retries Oculus login from "Go Online", even when no Meta
// provider exists. Resume its saved session, or offer its real email login.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_jni.h"
#include "kl_bigscreen.h"

static atomic_uint requested;
static unsigned observed, handled, since;
static unsigned popup_closed_for;
static int resume_attempted;
static int resume_finishing;
static uint32_t resume_task;
const char *kl_bigscreen_unity_arguments(void) {
    const char *package=kl_jni_guest_package();long version=0;
    kl_jni_guest_version(&version,NULL);
    if(package&&!strcmp(package,"com.bigscreenvr.bigscreen")&&version==4688857&&
       kl_env_on("KL_BIGSCREEN_CPU_SKINNING",1))return "-disable-gpu-skinning";
    return "";
}
static int enabled(void) {
    if (!kl_env_on("KL_BIGSCREEN_EMAIL_LOGIN",1)) return 0;
    const char *package=kl_jni_guest_package();
    if (!package || strcmp(package,"com.bigscreenvr.bigscreen")) return 0;
    long version=0;kl_jni_guest_version(&version,NULL);
    return version==4688857; // Validated Quest 0.950.2; other builds stay untouched.
}
void kl_bigscreen_account_unavailable(void) {
    if (enabled()) atomic_fetch_add_explicit(&requested,1,memory_order_release);
}

static struct {
    void *(*domain_get)(void);
    void **(*domain_get_assemblies)(void *,size_t *);
    void *(*assembly_get_image)(void *);
    void *(*class_from_name)(void *,const char *,const char *);
    void *(*class_get_method_from_name)(void *,const char *,int);
    void *(*class_get_type)(void *);
    void *(*type_get_object)(void *);
    void *(*class_get_field_from_name)(void *,const char *);
    void (*field_static_get_value)(void *,void *);
    void *(*runtime_invoke)(void *,void *,void **,void **);
    void *(*object_unbox)(void *);
    uint32_t (*array_length)(void *);
    void *(*object_get_class)(void *);
    void *(*class_get_parent)(void *);
    int32_t (*string_length)(void *);
    uint32_t (*gchandle_new)(void *,int);
    void *(*gchandle_get_target)(uint32_t);
    void (*gchandle_free)(uint32_t);
} api;
static int bind_api(void) {
    kl_image *image=kl_find_image("libil2cpp.so");
    if (!image) return 0;
#define BIND(n) do { *(void**)&api.n=kl_sym(image,"il2cpp_" #n);if(!api.n)return 0; } while(0)
    BIND(domain_get);BIND(domain_get_assemblies);BIND(assembly_get_image);
    BIND(class_from_name);BIND(class_get_method_from_name);BIND(class_get_type);
    BIND(type_get_object);BIND(class_get_field_from_name);BIND(field_static_get_value);
    BIND(runtime_invoke);BIND(object_unbox);BIND(array_length);
#undef BIND
    return 1;
}
static void drop_resume_task(void) {
    if(resume_task){api.gchandle_free(resume_task);resume_task=0;}
}
static void *find_class(const char *ns,const char *name);
static void *task_method(void *task,const char *name) {
    for(void *cls=api.object_get_class(task);cls;cls=api.class_get_parent(cls)) {
        void *method=api.class_get_method_from_name(cls,name,0);
        if(method)return method;
    }
    return NULL;
}
static int task_bool(void *task,const char *name,int *value) {
    void *method=task_method(task,name),*exception=NULL;
    if(!method)return 0;
    void *boxed=api.runtime_invoke(method,task,NULL,&exception);
    if(exception||!boxed)return 0;
    uint8_t *result=api.object_unbox(boxed);
    if(!result)return 0;
    *value=*result!=0;return 1;
}
// Use only the app's existing server-issued refresh token and authentication
// task. No password or token text is copied into host storage or diagnostics.
// 0 = use email form, 2 = waiting for authentication/connection.
static int resume_session(void) {
    if(!kl_env_on("KL_BIGSCREEN_RESUME_SESSION",1))return 0;
    if(resume_finishing)return 2;
    if(resume_attempted&&!resume_task)return 0;
    void *image=kl_find_image("libil2cpp.so");
#define OPTIONAL(n) do { *(void**)&api.n=kl_sym(image,"il2cpp_" #n);if(!api.n)return 0; } while(0)
    OPTIONAL(object_get_class);OPTIONAL(class_get_parent);OPTIONAL(string_length);
    OPTIONAL(gchandle_new);OPTIONAL(gchandle_get_target);OPTIONAL(gchandle_free);
#undef OPTIONAL
    void *loader=find_class("Bigscreen","BIG_APP_LOADER");
    void *singletons=find_class("Bigscreen","BIG_STATIC_SINGLETONS");
    void *accounts=find_class("Bigscreen.Cloud","Accounts");
    if(!loader||!singletons||!accounts)return 0;
    void *field=api.class_get_field_from_name(singletons,"appStateLoader");
    void *success=api.class_get_method_from_name(loader,"OnLogInSuccess",0);
    void *get=api.class_get_method_from_name(accounts,"GetRefreshToken",0);
    void *check=api.class_get_method_from_name(accounts,"CheckAccessTokenStatus",0);
    if(!field||!success||!get||!check)return 0;
    void *instance=NULL,*exception=NULL;
    api.field_static_get_value(field,&instance);
    if(!instance)return 0;
    if(!resume_attempted) {
        resume_attempted=1;
        void *token=api.runtime_invoke(get,NULL,NULL,&exception);
        if(exception||!token||api.string_length(token)<=0)return 0;
        void *task=api.runtime_invoke(check,NULL,NULL,&exception);
        if(exception||!task)return 0;
        resume_task=api.gchandle_new(task,0);
        if(!resume_task)return 0;
        fprintf(stderr,"  [compat] Bigscreen saved session: checking with the server\n");
    }
    void *task=api.gchandle_get_target(resume_task);
    int complete=0,faulted=0,canceled=0,valid=0;
    if(!task||!task_bool(task,"get_IsCompleted",&complete))goto failed;
    if(!complete)return 2;
    if(!task_bool(task,"get_IsFaulted",&faulted)||faulted||
       !task_bool(task,"get_IsCanceled",&canceled)||canceled||
       !task_bool(task,"get_Result",&valid)||!valid)goto failed;
    drop_resume_task();
    api.runtime_invoke(success,instance,NULL,&exception);
    if(exception)goto failed;
    resume_finishing=1;
    fprintf(stderr,"  [compat] Bigscreen saved session accepted; continuing normal sign-in\n");
    return 2;
failed:
    drop_resume_task();
    fprintf(stderr,"  [compat] Bigscreen saved session unavailable; email sign-in required\n");
    return 0;
}
static void *find_class(const char *ns,const char *name) {
    size_t count=0;void **assemblies=api.domain_get_assemblies(api.domain_get(),&count);
    for(size_t i=0;i<count;i++) {
        void *image=api.assembly_get_image(assemblies[i]);
        void *cls=image?api.class_from_name(image,ns,name):NULL;
        if(cls)return cls;
    }
    return NULL;
}
// 0 = UI not ready, 1 = handled, 2 = authentication pending, -1 = unsupported.
static int open_login(unsigned request) {
    if(!bind_api())return 0;
    void *tablet=find_class("Bigscreen.UI","TabletUI");
    void *states=find_class("Bigscreen.UI","BigUIState");
    void *object=find_class("UnityEngine","Object");
    void *websocket=find_class("Bigscreen.Cloud","Websocket");
    if(!tablet||!states||!object||!websocket)return 0;
    void *go=api.class_get_method_from_name(tablet,"GoToPage",4);
    void *end=api.class_get_method_from_name(tablet,"EndState",3);
    void *find=api.class_get_method_from_name(object,"FindObjectsOfType",1);
    void *connected=api.class_get_method_from_name(websocket,"get_IsConnected",0);
    void *login_field=api.class_get_field_from_name(states,"Login");
    void *popup_field=api.class_get_field_from_name(states,"PopupMessage");
    if(!go||!end||!find||!connected||!login_field||!popup_field)return -1;
    void *exception=NULL;
    void *boxed=api.runtime_invoke(connected,NULL,NULL,&exception);
    if(exception||!boxed)return -1;
    // Respect a successful Bigscreen session; a later Meta request must not
    // navigate away from the room or replace a signed-in account's UI.
    uint8_t *online=api.object_unbox(boxed);
    if(!online)return -1;
    if(*online&&!resume_finishing){drop_resume_task();return 1;}
    void *type=api.type_get_object(api.class_get_type(tablet));
    void *find_args[]={type};
    void *objects=api.runtime_invoke(find,NULL,find_args,&exception);
    if(exception)return -1;
    if(!objects||api.array_length(objects)!=1)return 0;
    // Unity's 64-bit Il2CppArray header, as in the existing managed probes.
    void *instance=*(void **)((uint8_t*)objects+32);
    int32_t login=0,popup=0;uint8_t no=0,yes=1;
    api.field_static_get_value(login_field,&login);
    api.field_static_get_value(popup_field,&popup);
    if(*online&&resume_finishing) {
        void *ui=find_class("Bigscreen.UI","BigUI");
        void *state=ui?api.class_get_method_from_name(ui,"get_CurrentScreenState",0):NULL;
        void *home=api.class_get_method_from_name(tablet,"GoHome",0);
        if(!state||!home)return -1;
        void *current=api.runtime_invoke(state,instance,NULL,&exception);
        if(exception||!current)return -1;
        int32_t *value=api.object_unbox(current);
        if(!value)return -1;
        // The normal connection flow can leave the earlier login panel open.
        // Close it only after online connection is real; preserve other pages.
        if(*value==login) {
            api.runtime_invoke(home,instance,NULL,&exception);
            if(exception)return -1;
        }
        resume_finishing=0;
        fprintf(stderr,"  [compat] Bigscreen saved session connected\n");
        return 1;
    }
    if(popup_closed_for!=request) {
        void *end_args[]={&popup,&no,&no};
        api.runtime_invoke(end,instance,end_args,&exception);
        if(exception)return -1;
        popup_closed_for=request;
    }
    int resumed=resume_session();
    if(resumed)return resumed;
    void *go_args[]={&login,&no,&yes,&no};
    api.runtime_invoke(go,instance,go_args,&exception);
    if(exception)return -1;
    fprintf(stderr,"  [compat] Bigscreen email sign-in opened after Meta account service failure\n");
    return 1;
}
void kl_bigscreen_tick(unsigned frame) {
    unsigned next=atomic_load_explicit(&requested,memory_order_acquire);
    if(next==handled||!enabled())return;
    if(next!=observed){observed=next;since=frame;}
    unsigned age=frame-since;
    // Let the SDK finish its failure UI, then retry briefly while Unity builds
    // the tablet. No arbitrary startup timer and no repeated prompt every frame.
    if(age<30||age%30)return;
    int result=open_login(next);
    // A real server task keeps its GC root until completion and uses the app's
    // own HTTP timeout. Do not open a competing password request while pending.
    if(result==2)return;
    if(result||age>=600) {
        drop_resume_task();
        handled=next;
        if(result<=0)fprintf(stderr,"  [compat] Bigscreen email sign-in UI unavailable; see runtime log\n");
    }
}
