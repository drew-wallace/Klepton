// Read-only packed scalar StructuredBuffers used by Bigscreen's avatar shader.
// ES3 uniform blocks hold identical bytes as uvec4 words: no float conversion,
// std140 scalar-array padding or invented avatar colors. Writable/other layouts
// remain unsupported and retain their original compile errors.
#include <ctype.h>
#define KLSS_BYTES 16384
#define KLSS_SLOTS 8
typedef struct {
    uint32_t program,blocks[64],uniforms[1024];int nb,nu,ns;
    struct {char name[80];uint32_t block,binding;} storage[KLSS_SLOTS];
} klss_program;
static klss_program klss_programs[64];
static __thread uint32_t klss_bound;
static __thread struct {uint32_t source,mirror;intptr_t offset,size;uint64_t version;} klss_bindings[KLSS_SLOTS];
// Guest buffer names are usually small; larger names still work without the
// copy cache. Atomic generations also notice uploads from a shared context.
static uint64_t klss_versions[65536];
#define SSGL(ret,name,args) static ret (*name) args; if(!name)name=asym(#name)
static int klss_trace(void){static int enabled=-1;if(enabled<0)enabled=kl_env_on("KL_AVATAR_TRACE",0);return enabled;}
static int klss_base(void){
    SSGL(void,glGetIntegerv,(uint32_t,int32_t*));int32_t n=0;glGetIntegerv(0x8A2F,&n); // MAX_UNIFORM_BUFFER_BINDINGS
    return n>=KLSS_SLOTS?n-KLSS_SLOTS:-1;
}
static klss_program *klss_find(uint32_t p){
    for(int i=0;i<64;i++)if(klss_programs[i].program==p&&p)return &klss_programs[i];return NULL;
}
static int klss_replace(char *s,size_t cap,char *at,size_t old,const char *replacement){
    size_t n=strlen(replacement),len=strlen(s);if(len-old+n>=cap)return 0;
    memmove(at+n,at+old,strlen(at+old)+1);memcpy(at,replacement,n);return 1;
}
static char *klss_bracket(char *p){
    if(*p!='[')return NULL;int depth=1;while(*++p)if(*p=='[')depth++;else if(*p==']'&&!--depth)return p;return NULL;
}
static int klss_rewrite(char *buf,size_t cap){
    static const char *names[]={"_SkinTones","_CustomColors","_IrisOffsets","_MicAmplitudes","_Gradients","_HiddenAvatarFlags"};
    if(!strstr(buf,"readonly buffer")||!strstr(buf,"_HiddenAvatarFlags_buf"))return 0;
    char *copy=malloc(cap);if(!copy)return 0;strcpy(copy,buf);int count=0;
    for(int i=0;i<6;i++){
        char pattern[160],replacement[512];const char *name=names[i];
        snprintf(pattern,sizeof pattern,"struct %s_type {",name);
        char *structure=strstr(copy,pattern);if(!structure)goto reject;
        char *member=structure+strlen(pattern);while(*member&&isspace((unsigned char)*member))member++;
        if(strncmp(member,"uint[1] value;",14))goto reject;
        member+=14;while(*member&&isspace((unsigned char)*member))member++;
        if(strncmp(member,"};",2))goto reject;
        snprintf(pattern,sizeof pattern,"readonly buffer %s {",name);
        char *body=strstr(copy,pattern);if(!body)goto reject;
        char *block=NULL;for(char *q=copy;(q=strstr(q,"layout("))&&q<body;q++)block=q;
        if(!block||!strstr(block,"std430")||strstr(block,"std430")>body)goto reject;
        unsigned binding=0;char *b=strstr(block,"binding");if(b&&b<body){b=strchr(b,'=');if(!b||b>body)goto reject;binding=(unsigned)strtoul(b+1,NULL,10);if(binding>=KLSS_SLOTS)goto reject;}
        char *end=strstr(block,"};");if(!end)goto reject;
        snprintf(pattern,sizeof pattern,"%s_type %s_buf[];",name,name);
        if(!strstr(block,pattern)||strstr(block,pattern)>end)goto reject;
        snprintf(replacement,sizeof replacement,
            "// klssbinding%s %u\nlayout(std140) uniform klss%s { highp uvec4 klssData%s[1024]; };\n"
            "uint klssRead%s(int i) { return klssData%s[i >> 2][i & 3]; }",name,binding,name,name,name,name);
        if(!klss_replace(copy,cap,block,end+2-block,replacement))goto reject;
        snprintf(pattern,sizeof pattern,"%s_buf[",name);
        char *hit;
        while((hit=strstr(copy,pattern))){
            char *begin=hit+strlen(pattern)-1,*close=klss_bracket(begin);if(!close)goto reject;
            const char *suffix=".value[(0 >> 2) + 0]";
            if(strncmp(close+1,suffix,strlen(suffix)))goto reject;
            size_t n=close-begin-1;if(n>300)goto reject;char index[304];memcpy(index,begin+1,n);index[n]=0;
            snprintf(replacement,sizeof replacement,"klssRead%s(int(%s))",name,index);
            if(!klss_replace(copy,cap,hit,close+1+strlen(suffix)-hit,replacement))goto reject;
        }
        count++;
    }
    strcpy(buf,copy);free(copy);fprintf(stderr,"  [glfb] avatar shader: translated %d packed read-only storage blocks\n",count);return 1;
reject:free(copy);return 0;
}
static void klss_link(uint32_t program){
    SSGL(void,glGetProgramiv,(uint32_t,uint32_t,int32_t*));
    SSGL(void,glGetActiveUniformBlockName,(uint32_t,uint32_t,int32_t,int32_t*,char*));
    SSGL(void,glGetActiveUniformsiv,(uint32_t,int32_t,const uint32_t*,uint32_t,int32_t*));
    SSGL(void,glUniformBlockBinding,(uint32_t,uint32_t,uint32_t));
    klss_program *p=klss_find(program);if(p)memset(p,0,sizeof *p);
    int32_t nb=0,nu=0,ok=0;glGetProgramiv(program,0x8B82,&ok);if(!ok)return;
    glGetProgramiv(program,0x8A36,&nb);glGetProgramiv(program,0x8B86,&nu);
    if(nb>64||nu>1024||klss_base()<0)return;
    SSGL(void,glGetAttachedShaders,(uint32_t,int32_t,int32_t*,uint32_t*));
    SSGL(void,glGetShaderiv,(uint32_t,uint32_t,int32_t*));
    SSGL(void,glGetShaderSource,(uint32_t,int32_t,int32_t*,char*));
    uint32_t shaders[8];int32_t nsh=0;char *sources[8]={0};glGetAttachedShaders(program,8,&nsh,shaders);
    for(int i=0;i<nsh;i++){int32_t len=0;glGetShaderiv(shaders[i],0x8B88,&len);if(len>0){sources[i]=calloc(1,len+1);glGetShaderSource(shaders[i],len,NULL,sources[i]);}}
    klss_program temp={.program=program};
    for(uint32_t i=0;i<(uint32_t)nb;i++){
        char name[100];glGetActiveUniformBlockName(program,i,sizeof name,NULL,name);
        if(!strncmp(name,"klss_",5)&&temp.ns<KLSS_SLOTS){
            int j=temp.ns++;snprintf(temp.storage[j].name,80,"%s",name+4);temp.storage[j].block=i;
            char marker[100];snprintf(marker,sizeof marker,"// klssbinding%s ",name+4);
            for(int k=0;k<nsh;k++){char *b=sources[k]?strstr(sources[k],marker):NULL;if(b)temp.storage[j].binding=(unsigned)strtoul(b+strlen(marker),NULL,10);}
            glUniformBlockBinding(program,i,klss_base()+temp.storage[j].binding);
        }else temp.blocks[temp.nb++]=i;
    }
    for(int i=0;i<nsh;i++)free(sources[i]);
    if(!temp.ns)return;
    for(uint32_t i=0;i<(uint32_t)nu;i++){
        int32_t block=-1;glGetActiveUniformsiv(program,1,&i,0x8A3A,&block);int hidden=0;
        for(int j=0;j<temp.ns;j++)if((int32_t)temp.storage[j].block==block)hidden=1;
        if(!hidden)temp.uniforms[temp.nu++]=i;
    }
    if(!p)for(int i=0;i<64;i++)if(!klss_programs[i].program){p=&klss_programs[i];break;}
    if(p){*p=temp;fprintf(stderr,"  [glfb] program %u: %d avatar storage blocks with separate uniform-buffer bindings\n",program,temp.ns);}
}
static int klss_real_index(uint32_t program,uint32_t iface,int index){
    klss_program *p=klss_find(program);if(!p)return index;
    if(iface==0x92E1)return index>=0&&index<p->nu?p->uniforms[index]:-1;
    if(iface==0x92E2)return index>=0&&index<p->nb?p->blocks[index]:-1;
    return index;
}
static int klss_guest_index(uint32_t program,uint32_t iface,int index){
    klss_program *p=klss_find(program);if(!p||index<0)return index;
    uint32_t *map=iface==0x92E1?p->uniforms:p->blocks;int n=iface==0x92E1?p->nu:p->nb;
    for(int i=0;i<n;i++)if(map[i]==(uint32_t)index)return i;return -1;
}
static int klss_interface(uint32_t program,uint32_t iface,uint32_t pname,int32_t *out){
    klss_program *p=klss_find(program);if(!p)return 0;
    if(pname==0x92F5&&(iface==0x92E1||iface==0x92E2)){*out=iface==0x92E1?p->nu:p->nb;return 1;}
    if(iface!=0x92E6&&iface!=0x92E5)return 0;
    *out=pname==0x92F5?p->ns:pname==0x92F6?100:pname==0x92F7?1:0;return 1;
}
static int klss_resource(uint32_t program,uint32_t iface,uint32_t index,int32_t count,const uint32_t *props,int32_t size,int32_t *length,int32_t *out){
    if(iface!=0x92E6&&iface!=0x92E5)return 0;
    klss_program *p=klss_find(program);if(length)*length=0;if(!p||index>=(unsigned)p->ns||!props||!out)return 1;
    SSGL(void,glGetActiveUniformBlockiv,(uint32_t,uint32_t,uint32_t,int32_t*));
    int n=count<size?count:size;for(int i=0;i<n;i++){
        int v=0;switch(props[i]){
        case 0x92F9:v=(int)strlen(p->storage[index].name)+(iface==0x92E5?(int)strlen("_buf[0].value[0]")+1:1);break;
        case 0x9302:v=p->storage[index].binding;break;
        case 0x9303:v=4;break; // Minimum std430 scalar element, not the padded UBO size.
        case 0x9304:v=1;break;case 0x9305:v=index;break;
        case 0x9306:glGetActiveUniformBlockiv(program,p->storage[index].block,0x8A44,&v);break;
        case 0x930A:glGetActiveUniformBlockiv(program,p->storage[index].block,0x8A46,&v);break;
        case 0x92FA:v=0x1405;break;case 0x92FB:v=1;break;case 0x92FD:v=index;break;
        case 0x92FE:v=4;break;case 0x930C:v=0;break;case 0x930D:v=4;break;
        }out[i]=v;
    }if(length)*length=n;return 1;
}
static int klss_resource_name(uint32_t program,uint32_t iface,uint32_t index,int size,int *length,char *name){
    if(iface!=0x92E6&&iface!=0x92E5)return 0;
    klss_program *p=klss_find(program);int len=0;if(size>0&&name){name[0]=0;if(p&&index<(unsigned)p->ns){
        snprintf(name,size,"%s%s",p->storage[index].name,iface==0x92E5?"_buf[0].value[0]":"");len=(int)strlen(name);}}
    if(length)*length=len;return 1;
}
static void klss_UniformBlockBinding(uint32_t program,uint32_t index,uint32_t binding){
    SSGL(void,glUniformBlockBinding,(uint32_t,uint32_t,uint32_t));
    int real=klss_real_index(program,0x92E2,index);if(real>=0)glUniformBlockBinding(program,real,binding);
}
static uint32_t klss_GetUniformBlockIndex(uint32_t program,const char *name){
    SSGL(uint32_t,glGetUniformBlockIndex,(uint32_t,const char*));
    return klss_guest_index(program,0x92E2,(int)glGetUniformBlockIndex(program,name));
}
// Unity 2020 reflects ordinary material uniforms through ES3, then storage
// through ES3.1. Both views must use the same virtual indices and counts.
static void klss_GetProgramiv(uint32_t program,uint32_t pname,int32_t *out){
    SSGL(void,glGetProgramiv,(uint32_t,uint32_t,int32_t*));klss_program *p=klss_find(program);
    if(p&&out&&pname==0x8B86){*out=p->nu;return;}
    if(p&&out&&pname==0x8A36){*out=p->nb;return;}
    glGetProgramiv(program,pname,out);
}
static void klss_GetActiveUniform(uint32_t program,uint32_t index,int32_t size,int32_t *length,int32_t *count,uint32_t *type,char *name){
    SSGL(void,glGetActiveUniform,(uint32_t,uint32_t,int32_t,int32_t*,int32_t*,uint32_t*,char*));
    glGetActiveUniform(program,klss_real_index(program,0x92E1,index),size,length,count,type,name);
}
static void klss_GetUniformIndices(uint32_t program,int32_t count,const char *const *names,uint32_t *indices){
    SSGL(void,glGetUniformIndices,(uint32_t,int32_t,const char*const*,uint32_t*));
    glGetUniformIndices(program,count,names,indices);
    for(int i=0;indices&&i<count;i++)indices[i]=klss_guest_index(program,0x92E1,(int)indices[i]);
}
static void klss_GetActiveUniformsiv(uint32_t program,int32_t count,const uint32_t *indices,uint32_t pname,int32_t *out){
    SSGL(void,glGetActiveUniformsiv,(uint32_t,int32_t,const uint32_t*,uint32_t,int32_t*));
    if(!klss_find(program)){glGetActiveUniformsiv(program,count,indices,pname,out);return;}
    for(int i=0;indices&&out&&i<count;i++){
        uint32_t real=klss_real_index(program,0x92E1,indices[i]);glGetActiveUniformsiv(program,1,&real,pname,out+i);
        if(pname==0x8A3A)out[i]=klss_guest_index(program,0x92E2,out[i]);
    }
}
static void klss_GetActiveUniformBlockiv(uint32_t program,uint32_t index,uint32_t pname,int32_t *out){
    SSGL(void,glGetActiveUniformBlockiv,(uint32_t,uint32_t,uint32_t,int32_t*));
    uint32_t real=klss_real_index(program,0x92E2,index);glGetActiveUniformBlockiv(program,real,pname,out);
    if(out&&klss_find(program)&&pname==0x8A43){
        int32_t count=0;glGetActiveUniformBlockiv(program,real,0x8A42,&count);
        for(int i=0;i<count;i++)out[i]=klss_guest_index(program,0x92E1,out[i]);
    }
}
static void klss_GetActiveUniformBlockName(uint32_t program,uint32_t index,int32_t size,int32_t *length,char *name){
    SSGL(void,glGetActiveUniformBlockName,(uint32_t,uint32_t,int32_t,int32_t*,char*));
    glGetActiveUniformBlockName(program,klss_real_index(program,0x92E2,index),size,length,name);
}
// Borrow COPY_WRITE only for the duration of an operation; SSBO and copy-write
// generic bindings are independent in the guest's GL state.
static uint32_t klss_begin(void){
    SSGL(void,glGetIntegerv,(uint32_t,int32_t*));SSGL(void,glBindBuffer,(uint32_t,uint32_t));
    int32_t prev=0;glGetIntegerv(0x8F37,&prev);glBindBuffer(0x8F37,klss_bound);return prev;
}
static void klss_end(uint32_t prev){SSGL(void,glBindBuffer,(uint32_t,uint32_t));glBindBuffer(0x8F37,prev);}
static void klss_refresh(uint32_t buffer){
    if(!buffer)return;
    SSGL(void,glBindBuffer,(uint32_t,uint32_t));SSGL(void,glGetIntegerv,(uint32_t,int32_t*));
    SSGL(void,glGetBufferParameteriv,(uint32_t,uint32_t,int32_t*));SSGL(void,glGenBuffers,(int32_t,uint32_t*));
    SSGL(void,glBufferData,(uint32_t,intptr_t,const void*,uint32_t));SSGL(void,glCopyBufferSubData,(uint32_t,uint32_t,intptr_t,intptr_t,intptr_t));
    SSGL(void,glBindBufferBase,(uint32_t,uint32_t,uint32_t));
    int32_t read=0,write=0,uniform=0;glGetIntegerv(0x8A28,&uniform);glGetIntegerv(0x8F36,&read);glGetIntegerv(0x8F37,&write);
    for(int i=0;i<KLSS_SLOTS;i++)if(klss_bindings[i].source==buffer){
        uint64_t version=buffer<65536?__atomic_load_n(&klss_versions[buffer],__ATOMIC_ACQUIRE):UINT64_MAX;
        if(buffer<65536&&klss_bindings[i].mirror&&klss_bindings[i].version==version)continue;
        glBindBuffer(0x8F36,buffer);int32_t bytes=0;glGetBufferParameteriv(0x8F36,0x8764,&bytes);
        intptr_t size=klss_bindings[i].size?klss_bindings[i].size:bytes-klss_bindings[i].offset;
        if(size<=0||size>KLSS_BYTES||klss_bindings[i].offset<0||klss_bindings[i].offset>bytes-size){
            fprintf(stderr,"  [glfb] read-only storage buffer exceeds supported range (%ld bytes)\n",(long)size);continue;}
        if(!klss_bindings[i].mirror){
            glGenBuffers(1,&klss_bindings[i].mirror);glBindBuffer(0x8F37,klss_bindings[i].mirror);
            static const unsigned char zero[KLSS_BYTES];glBufferData(0x8F37,KLSS_BYTES,zero,0x88E8);
        }else glBindBuffer(0x8F37,klss_bindings[i].mirror);
        glCopyBufferSubData(0x8F36,0x8F37,klss_bindings[i].offset,0,size);
        glBindBufferBase(0x8A11,klss_base()+i,klss_bindings[i].mirror);
        klss_bindings[i].version=version;
    }
    glBindBuffer(0x8F36,read);glBindBuffer(0x8F37,write);glBindBuffer(0x8A11,uniform);
}
static void klss_changed(uint32_t buffer){
    if(buffer&&buffer<65536)__atomic_add_fetch(&klss_versions[buffer],1,__ATOMIC_RELEASE);
    klss_refresh(buffer);
}
static void klss_deleted(int32_t count,const uint32_t *buffers){
    SSGL(void,glDeleteBuffers,(int32_t,const uint32_t*));
    SSGL(void,glBindBufferBase,(uint32_t,uint32_t,uint32_t));
    SSGL(void,glGetIntegerv,(uint32_t,int32_t*));SSGL(void,glBindBuffer,(uint32_t,uint32_t));
    int32_t uniform=0;glGetIntegerv(0x8A28,&uniform);
    for(int j=0;buffers&&j<count;j++){
        if(klss_bound==buffers[j])klss_bound=0;
        for(int i=0;i<KLSS_SLOTS;i++)if(klss_bindings[i].source==buffers[j]){
            if(klss_bindings[i].mirror)glDeleteBuffers(1,&klss_bindings[i].mirror);
            memset(&klss_bindings[i],0,sizeof klss_bindings[i]);
            glBindBufferBase(0x8A11,klss_base()+i,0);
        }
        if(buffers[j]<65536)__atomic_add_fetch(&klss_versions[buffers[j]],1,__ATOMIC_RELEASE);
    }
    glBindBuffer(0x8A11,uniform);
}
static void klss_BindBufferRange(uint32_t target,uint32_t index,uint32_t buffer,intptr_t offset,intptr_t size){
    SSGL(void,glBindBufferRange,(uint32_t,uint32_t,uint32_t,intptr_t,intptr_t));
    if(target!=0x90D2){glBindBufferRange(target,index,buffer,offset,size);return;}
    if(index>=KLSS_SLOTS)return;
    int same=klss_bindings[index].source==buffer&&klss_bindings[index].offset==offset&&klss_bindings[index].size==size;
    klss_bound=buffer;klss_bindings[index].source=buffer;klss_bindings[index].offset=offset;klss_bindings[index].size=size;
    if(!same)klss_bindings[index].version=UINT64_MAX;
    if(buffer)klss_refresh(buffer);else{
        SSGL(void,glBindBufferBase,(uint32_t,uint32_t,uint32_t));
        SSGL(void,glGetIntegerv,(uint32_t,int32_t*));SSGL(void,glBindBuffer,(uint32_t,uint32_t));
        int32_t uniform=0;glGetIntegerv(0x8A28,&uniform);glBindBufferBase(0x8A11,klss_base()+index,0);glBindBuffer(0x8A11,uniform);
    }
}
static void klss_BindBufferBase(uint32_t target,uint32_t index,uint32_t buffer){
    SSGL(void,glBindBufferBase,(uint32_t,uint32_t,uint32_t));
    if(target==0x90D2)klss_BindBufferRange(target,index,buffer,0,0);else glBindBufferBase(target,index,buffer);
}
static void klss_trace_draw(uint32_t program){
    static int count;if(!klss_trace()||count>=3||!klss_find(program))return;count++;
    SSGL(void,glGetIntegeri_v,(uint32_t,uint32_t,int32_t*));
    SSGL(void,glGetBufferParameteriv,(uint32_t,uint32_t,int32_t*));
    SSGL(void,glGetIntegerv,(uint32_t,int32_t*));SSGL(void,glBindBuffer,(uint32_t,uint32_t));
    int32_t prev=0;glGetIntegerv(0x8F36,&prev);
    for(int i=0;i<6;i++){
        int32_t actual=0,bytes=0;glGetIntegeri_v(0x8A28,klss_base()+i,&actual);
        if(klss_bindings[i].source){glBindBuffer(0x8F36,klss_bindings[i].source);glGetBufferParameteriv(0x8F36,0x8764,&bytes);}
        fprintf(stderr,"  [avatar] draw %d program %u slot %d source %u mirror %u bound %d bytes %d\n",count,program,i,klss_bindings[i].source,klss_bindings[i].mirror,actual,bytes);
    }
    glBindBuffer(0x8F36,prev);
}
static void klss_GetBufferParameteriv(uint32_t target,uint32_t pname,int32_t *out){
    SSGL(void,glGetBufferParameteriv,(uint32_t,uint32_t,int32_t*));
    if(target==0x90D2){uint32_t prev=klss_begin();glGetBufferParameteriv(0x8F37,pname,out);klss_end(prev);}
    else glGetBufferParameteriv(target,pname,out);
}
#undef SSGL
