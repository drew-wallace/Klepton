// Actual ANGLE pixels from packed scalar storage reads, using Unity reflection.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kl_glfb.h"
#define GL(ret,name,args) ret (*name) args=kl_glfb_sym(#name);assert(name)
static uint32_t shader(uint32_t kind,const char *source){
    GL(uint32_t,glCreateShader,(uint32_t));GL(void,glShaderSource,(uint32_t,int,const char*const*,const int*));
    GL(void,glCompileShader,(uint32_t));GL(void,glGetShaderiv,(uint32_t,uint32_t,int*));
    uint32_t s=glCreateShader(kind);glShaderSource(s,1,&source,NULL);glCompileShader(s);int ok=0;glGetShaderiv(s,0x8B81,&ok);assert(ok);return s;
}
static uint32_t program(const char *fragment){
    const char *vertex="#version 300 es\nout highp vec4 vs_TEXCOORD0;out highp vec4 vs_TEXCOORD2;out highp vec3 vs_NORMAL1;void main(){vs_TEXCOORD0=vec4(0);vs_TEXCOORD2=vec4(0);vs_NORMAL1=vec3(0,1,0);gl_Position=vec4(float((gl_VertexID<<1)&2)*2.-1.,float(gl_VertexID&2)*2.-1.,0,1);}";
    GL(uint32_t,glCreateProgram,(void));GL(void,glAttachShader,(uint32_t,uint32_t));GL(void,glLinkProgram,(uint32_t));GL(void,glGetProgramiv,(uint32_t,uint32_t,int*));
    uint32_t p=glCreateProgram();glAttachShader(p,shader(0x8B31,vertex));glAttachShader(p,shader(0x8B30,fragment));glLinkProgram(p);int ok=0;glGetProgramiv(p,0x8B82,&ok);assert(ok);return p;
}
int main(int argc,char **argv){
    alarm(30);setenv("KL_GLFB_SIZE","64x64",1);assert(kl_glfb_init());kl_glfb_make_current();
    // The ignored captured shader is supplied locally; no proprietary shader
    // or online profile data is copied into the repository test fixture.
    if(argc>1){FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *s=calloc(1,n+1);assert(fread(s,1,n,f)==n);fclose(f);program(s);free(s);}
    const char *names[]={"_SkinTones","_CustomColors","_IrisOffsets","_MicAmplitudes","_Gradients","_HiddenAvatarFlags"};
    uint32_t bindings[]={5,3,1,4,0,2};
    char fragment[8192]="#version 310 es\nprecision highp float;precision highp int;\n";
    for(int i=0;i<6;i++){
        char declaration[512];snprintf(declaration,sizeof declaration,"struct %s_type { uint[1] value; };\nlayout(binding = %u, std430) readonly buffer %s { %s_type %s_buf[]; };\n",names[i],bindings[i],names[i],names[i],names[i]);strcat(fragment,declaration);
    }
    strcat(fragment,"layout(std140) uniform UnityTest { vec4 tint; };out vec4 color;void main(){\n");
    strcat(fragment,"float r=uintBitsToFloat(_SkinTones_buf[5].value[(0 >> 2) + 0])+uintBitsToFloat(_CustomColors_buf[5].value[(0 >> 2) + 0]);\n");
    strcat(fragment,"float g=uintBitsToFloat(_IrisOffsets_buf[5].value[(0 >> 2) + 0])+uintBitsToFloat(_MicAmplitudes_buf[5].value[(0 >> 2) + 0]);\n");
    strcat(fragment,"float b=uintBitsToFloat(_Gradients_buf[5].value[(0 >> 2) + 0])+uintBitsToFloat(_HiddenAvatarFlags_buf[5].value[(0 >> 2) + 0]);color=vec4(r,g,b,1)*tint;}\n");
    uint32_t p=program(fragment);
    GL(void,glGetProgramInterfaceiv,(uint32_t,uint32_t,uint32_t,int*));GL(uint32_t,glGetProgramResourceIndex,(uint32_t,uint32_t,const char*));
    GL(void,glGetProgramResourceName,(uint32_t,uint32_t,uint32_t,int,int*,char*));GL(void,glGetProgramResourceiv,(uint32_t,uint32_t,uint32_t,int,const uint32_t*,int,int*,int*));
    GL(void,glUniformBlockBinding,(uint32_t,uint32_t,uint32_t));
    GL(uint32_t,glGetUniformBlockIndex,(uint32_t,const char*));GL(void,glUseProgram,(uint32_t));
    GL(void,glGetProgramiv,(uint32_t,uint32_t,int*));GL(void,glGetActiveUniformBlockName,(uint32_t,uint32_t,int,int*,char*));
    GL(void,glGetActiveUniformBlockiv,(uint32_t,uint32_t,uint32_t,int*));GL(void,glGetActiveUniformsiv,(uint32_t,int,const uint32_t*,uint32_t,int*));
    GL(void,glGenBuffers,(int,uint32_t*));GL(void,glBindBuffer,(uint32_t,uint32_t));GL(void,glBufferData,(uint32_t,intptr_t,const void*,uint32_t));
    GL(void,glDeleteBuffers,(int,const uint32_t*));
    GL(void,glBufferSubData,(uint32_t,intptr_t,intptr_t,const void*));GL(void,glBindBufferBase,(uint32_t,uint32_t,uint32_t));GL(void,glBindBufferRange,(uint32_t,uint32_t,uint32_t,intptr_t,intptr_t));
    GL(void,glGetIntegerv,(uint32_t,int*));GL(uint32_t,glGetError,(void));GL(void,glGetBufferParameteriv,(uint32_t,uint32_t,int*));
    GL(void*,glMapBufferRange,(uint32_t,intptr_t,intptr_t,uint32_t));GL(uint8_t,glUnmapBuffer,(uint32_t));
    GL(void,glDrawArrays,(uint32_t,int,int));GL(void,glReadPixels,(int,int,int,int,uint32_t,uint32_t,void*));
    int count=0;glGetProgramInterfaceiv(p,0x92E6,0x92F5,&count);assert(count==6);
    glGetIntegerv(0x90DD,&count);assert(count==8);glGetIntegerv(0x90DA,&count);assert(count==6);
    glGetIntegerv(0x90DE,&count);assert(count==16384);glGetIntegerv(0x90DB,&count);assert(count==0);
    glGetProgramInterfaceiv(p,0x92E2,0x92F5,&count);assert(count==1); // No leaked emulation blocks.
    glGetProgramInterfaceiv(p,0x92E1,0x92F5,&count);assert(count==1); // Only tint is a uniform.
    glGetProgramiv(p,0x8A36,&count);assert(count==1);glGetProgramiv(p,0x8B86,&count);assert(count==1);
    char blockName[80];glGetActiveUniformBlockName(p,0,sizeof blockName,NULL,blockName);assert(!strcmp(blockName,"UnityTest"));
    int uniformIndex=-1,blockIndex=-1;glGetActiveUniformBlockiv(p,0,0x8A43,&uniformIndex);assert(uniformIndex==0);
    uint32_t uniformQuery=uniformIndex;glGetActiveUniformsiv(p,1,&uniformQuery,0x8A3A,&blockIndex);assert(blockIndex==0);
    uint32_t normal;glGenBuffers(1,&normal);glBindBuffer(0x8A11,normal);float tint[]={1,1,1,1};glBufferData(0x8A11,16,tint,0x88E8);
    uint32_t normalIndex=glGetUniformBlockIndex(p,"UnityTest");assert(normalIndex==0);glUniformBlockBinding(p,normalIndex,0);glBindBufferBase(0x8A11,0,normal);
    uint32_t copySentinel;glGenBuffers(1,&copySentinel);glBindBuffer(0x8F37,copySentinel);glBufferData(0x8F37,32,NULL,0x88E8);
    uint32_t buffers[6];glGenBuffers(6,buffers);
    for(int i=0;i<6;i++){
        uint32_t resource=glGetProgramResourceIndex(p,0x92E6,names[i]);assert(resource<6);
        char name[80];glGetProgramResourceName(p,0x92E6,resource,sizeof name,NULL,name);assert(!strcmp(name,names[i]));
        uint32_t prop=0x9302;int bound=-1,len=0;glGetProgramResourceiv(p,0x92E6,resource,1,&prop,1,&len,&bound);assert(len==1&&bound==bindings[i]);
        uint32_t properties[]={0x92FA,0x92FB,0x92FE,0x930C,0x930D};int valuesOut[5];
        glGetProgramResourceiv(p,0x92E5,resource,5,properties,5,&len,valuesOut);
        assert(len==5&&valuesOut[0]==0x1405&&valuesOut[1]==1&&valuesOut[2]==4&&valuesOut[3]==0&&valuesOut[4]==4);
        float values[12]={0};values[5]=.1f*(i/2+1);values[9]=.05f;
        glBindBuffer(0x90D2,buffers[i]);glBufferData(0x90D2,sizeof values,values,0x88E8);glBindBufferBase(0x90D2,bindings[i],buffers[i]);
        int size=0;glGetBufferParameteriv(0x90D2,0x8764,&size);assert(size==sizeof values);
    }
    int bound=0;glGetIntegerv(0x8A28,&bound);assert(bound==normal);glGetIntegerv(0x8F37,&bound);assert(bound==copySentinel);
    glUseProgram(p);glDrawArrays(4,0,3);uint8_t pixel[4];glReadPixels(32,32,1,1,0x1908,0x1401,pixel);
    assert(abs(pixel[0]-51)<3&&abs(pixel[1]-102)<3&&abs(pixel[2]-153)<3);
    glBindBuffer(0x90D2,buffers[0]);float value=.5f;glBufferSubData(0x90D2,5*4,4,&value);
    glDrawArrays(4,0,3);glReadPixels(32,32,1,1,0x1908,0x1401,pixel);assert(abs(pixel[0]-153)<3);
    float *mapped=glMapBufferRange(0x90D2,5*4,4,2|4);assert(mapped);*mapped=.2f;assert(glUnmapBuffer(0x90D2));
    glDrawArrays(4,0,3);glReadPixels(32,32,1,1,0x1908,0x1401,pixel);assert(abs(pixel[0]-77)<3);
    glBindBufferRange(0x90D2,bindings[0],buffers[0],16,32);
    glDrawArrays(4,0,3);glReadPixels(32,32,1,1,0x1908,0x1401,pixel);assert(abs(pixel[0]-38)<3);
    assert(!glGetError());
    glBindBuffer(0x8A11,normal);glDeleteBuffers(6,buffers);glGetIntegerv(0x8A28,&bound);assert(bound==normal);
    glGetIntegerv(0x90D3,&bound);assert(!bound);assert(!glGetError());
    puts("Avatar storage: captured shader compilation/link, real packed GPU colors, reflection isolation, binding permutation, subdata/map/range updates and GL state preservation pass");
}
