// Account failure may check a saved session; only server success may continue.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "klepton.h"
#include "kl_bigscreen.h"

static const char *package="com.bigscreenvr.bigscreen";
static long version=4688857;
static int lookups,opened,ended,online,unready,throws,missing;
static uint8_t connected;
static int saved,complete=1,valid=1,faulted,canceled,check_throws,success_throws;
static int checked,resumed,handles,freed,token_length=64,task;
static uint8_t boxed_bool;
static int32_t current_state=25;
static int home_opened;
static struct {void *klass,*monitor,*bounds;uint64_t length;void *item;} array;
const char *kl_jni_guest_package(void){return package;}
void kl_jni_guest_version(long *code,const char **name){if(code)*code=version;if(name)*name="fixture";}
kl_image *kl_find_image(const char *name){assert(!strcmp(name,"libil2cpp.so"));lookups++;return (void*)1;}
static void *domain_get(void){return (void*)1;}
static void **domain_get_assemblies(void *d,size_t *n){static void *assemblies[]={(void*)1};*n=1;return assemblies;}
static void *assembly_get_image(void *a){return a;}
static void *class_from_name(void *image,const char *ns,const char *name){return (void*)name;}
static void *class_get_method_from_name(void *cls,const char *name,int argc){
    if(!strcmp(name,"get_IsConnected")){assert(argc==0);return (void*)1;}
    if(!strcmp(name,"FindObjectsOfType")){assert(argc==1);return (void*)2;}
    if(!strcmp(name,"EndState")){assert(argc==3);return (void*)3;}
    if(!strcmp(name,"GoToPage")){assert(argc==4);return (void*)4;}
    if(!strcmp(name,"GetRefreshToken")){assert(argc==0);return (void*)5;}
    if(!strcmp(name,"CheckAccessTokenStatus")){assert(argc==0);return (void*)6;}
    if(!strcmp(name,"OnLogInSuccess")){assert(argc==0);return (void*)7;}
    if(!strcmp(name,"get_IsCompleted"))return !strcmp(cls,"TaskBase")?(void*)8:NULL;
    if(!strcmp(name,"get_IsFaulted"))return !strcmp(cls,"TaskBase")?(void*)9:NULL;
    if(!strcmp(name,"get_IsCanceled"))return !strcmp(cls,"TaskBase")?(void*)10:NULL;
    if(!strcmp(name,"get_Result"))return (void*)11;
    if(!strcmp(name,"get_CurrentScreenState"))return (void*)12;
    if(!strcmp(name,"GoHome"))return (void*)13;
    abort();
}
static void *identity(void *p){return p;}
static void *class_get_field_from_name(void *cls,const char *name){return missing?NULL:(void*)name;}
static void field_static_get_value(void *field,void *out){
    if(!strcmp(field,"Login"))*(int32_t*)out=25;
    else if(!strcmp(field,"appStateLoader"))*(void**)out=(void*)100;
    else {assert(!strcmp(field,"PopupMessage"));*(int32_t*)out=31;}
}
static uint32_t array_length(void *p){return (uint32_t)array.length;}
static void *runtime_invoke(void *method,void *instance,void **args,void **exception){
    if(method==(void*)1){connected=online;return &connected;}
    if(method==(void*)2){array.length=unready?0:1;array.item=(void*)99;return &array;}
    if(method==(void*)5){assert(!instance);return saved?&token_length:NULL;}
    if(method==(void*)6){assert(!instance);checked++;if(check_throws)*exception=(void*)1;return &task;}
    if(method==(void*)7){assert(instance==(void*)100&&valid&&!faulted&&!canceled);resumed++;if(success_throws)*exception=(void*)1;return NULL;}
    if(method>=(void*)8&&method<=(void*)11){
        assert(instance==&task);
        if(method==(void*)8)boxed_bool=complete;
        if(method==(void*)9)boxed_bool=faulted;
        if(method==(void*)10)boxed_bool=canceled;
        if(method==(void*)11){assert(complete&&!faulted&&!canceled);boxed_bool=valid;}
        return &boxed_bool;
    }
    if(method==(void*)12){assert(instance==(void*)99);return &current_state;}
    if(method==(void*)13){assert(instance==(void*)99&&online);home_opened++;return NULL;}
    assert(instance==(void*)99);
    if(method==(void*)3){assert(*(int32_t*)args[0]==31&&!*(uint8_t*)args[1]&&!*(uint8_t*)args[2]);ended++;if(throws)*exception=(void*)1;}
    else {assert(method==(void*)4&&*(int32_t*)args[0]==25&&!*(uint8_t*)args[1]&&*(uint8_t*)args[2]&&!*(uint8_t*)args[3]);opened++;}
    return NULL;
}
static void *object_get_class(void *obj){assert(obj==&task);return "TaskBool";}
static void *class_get_parent(void *cls){return !strcmp(cls,"TaskBool")?"TaskBase":NULL;}
static int32_t string_length(void *s){assert(s==&token_length);return token_length;}
static uint32_t gchandle_new(void *obj,int pinned){assert(obj==&task&&!pinned);handles++;return 1;}
static void *gchandle_get_target(uint32_t id){assert(id==1&&handles>freed);return &task;}
static void gchandle_free(uint32_t id){assert(id==1&&handles>freed);freed++;}
void *kl_sym(kl_image *image,const char *name){
#define MAP(n) if(!strcmp(name,"il2cpp_" #n))return (void*)n
    MAP(domain_get);MAP(domain_get_assemblies);MAP(assembly_get_image);
    MAP(class_from_name);MAP(class_get_method_from_name);MAP(class_get_field_from_name);
    MAP(field_static_get_value);MAP(runtime_invoke);MAP(array_length);
    MAP(object_get_class);MAP(class_get_parent);MAP(string_length);
    MAP(gchandle_new);MAP(gchandle_get_target);MAP(gchandle_free);
#undef MAP
    if(!strcmp(name,"il2cpp_class_get_type")||!strcmp(name,"il2cpp_type_get_object")||!strcmp(name,"il2cpp_object_unbox"))return (void*)identity;
    abort();
}
static void ticks(unsigned from,unsigned to){for(unsigned frame=from;frame<=to;frame++)kl_bigscreen_tick(frame);}
static void check(int mode){
    pid_t child=fork();assert(child>=0);
    if(!child){
        unsetenv("KL_BIGSCREEN_EMAIL_LOGIN");
        unsetenv("KL_BIGSCREEN_RESUME_SESSION");
        if(mode>=11)saved=1;
        if(mode==12)valid=0;
        if(mode==13)faulted=1;
        if(mode==14)complete=0;
        if(mode==15)canceled=1;
        if(mode==16)setenv("KL_BIGSCREEN_RESUME_SESSION","0",1);
        if(mode==17)check_throws=1;
        if(mode==18)success_throws=1;
        if(mode==19)token_length=0;
        if(mode==1)package="com.example.other";
        if(mode==2)version++;
        if(mode==3)setenv("KL_BIGSCREEN_EMAIL_LOGIN","0",1);
        if(mode==5)online=1;
        if(mode==6||mode==10)unready=1;
        if(mode==7)throws=1;
        if(mode==8)missing=1;
        if(mode)kl_bigscreen_account_unavailable();
        if(mode==9)kl_bigscreen_account_unavailable();
        ticks(0,29);assert(!lookups&&!opened);
        ticks(30,100);
        if(mode==0||mode==1||mode==2||mode==3)assert(!lookups&&!opened&&!ended);
        if(mode==4||mode==9){assert(opened==1&&ended==1);ticks(101,200);assert(opened==1);kl_bigscreen_account_unavailable();ticks(201,300);assert(opened==2);}
        if(mode==5)assert(lookups&&!opened&&!ended);
        if(mode==6){assert(!opened);unready=0;ticks(101,200);assert(opened==1&&ended==1);}
        if(mode==7)assert(!opened&&ended==1);
        if(mode==8)assert(!opened&&!ended);
        if(mode==10){ticks(101,1000);int before=lookups;ticks(1001,2000);assert(before==lookups&&!opened);}
        if(mode==11){assert(checked==1&&resumed==1&&!opened&&freed==1);online=1;ticks(101,200);assert(resumed==1&&home_opened==1);ticks(201,300);assert(home_opened==1);}
        if(mode==12||mode==13||mode==15){assert(checked==1&&!resumed&&opened==1&&handles==1&&freed==1);}
        if(mode==14){assert(checked==1&&!resumed&&!opened&&handles==1&&!freed);ticks(101,1000);assert(checked==1&&!opened&&!freed);complete=1;ticks(1001,1100);assert(resumed==1&&!opened&&freed==1&&ended==1);online=1;current_state=7;ticks(1101,1200);assert(!home_opened);}
        if(mode==16||mode==19)assert(!checked&&!resumed&&opened==1&&!handles);
        if(mode==17)assert(checked==1&&!resumed&&opened==1&&!handles);
        if(mode==18)assert(checked==1&&resumed==1&&opened==1&&freed==1);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(void){
    unsetenv("KL_BIGSCREEN_CPU_SKINNING");
    assert(!strcmp(kl_bigscreen_unity_arguments(),"-disable-gpu-skinning"));
    package="com.example.other";assert(!*kl_bigscreen_unity_arguments());
    package="com.bigscreenvr.bigscreen";version++;assert(!*kl_bigscreen_unity_arguments());version--;
    setenv("KL_BIGSCREEN_CPU_SKINNING","0",1);assert(!*kl_bigscreen_unity_arguments());unsetenv("KL_BIGSCREEN_CPU_SKINNING");
    for(int mode=0;mode<=19;mode++)check(mode);return 0;
}
