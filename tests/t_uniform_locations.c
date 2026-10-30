// A Unity-style pinned sampler must keep unit zero even after a 360-float array
// changes ANGLE's location assignment; queries and baked writes may be mixed.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "kl_glfb.h"
#define GL(ret,name,args) ret(*name)args=kl_glfb_sym(#name);assert(name)
int main(void){
    setenv("KL_GLFB_SIZE","32x32",1);setenv("KL_GLFB_VIRTUAL_UNIFORMS","1",1);assert(kl_glfb_init());kl_glfb_make_current();
    GL(uint32_t,glCreateShader,(uint32_t));GL(void,glShaderSource,(uint32_t,int,const char*const*,const int*));GL(void,glCompileShader,(uint32_t));
    GL(void,glGetShaderiv,(uint32_t,uint32_t,int*));GL(uint32_t,glCreateProgram,(void));GL(void,glAttachShader,(uint32_t,uint32_t));
    GL(void,glLinkProgram,(uint32_t));GL(void,glGetProgramiv,(uint32_t,uint32_t,int*));GL(void,glUseProgram,(uint32_t));
    GL(int,glGetUniformLocation,(uint32_t,const char*));GL(void,glUniform1i,(int,int));GL(void,glUniform1f,(int,float));
    GL(void,glUniform1fv,(int,int,const float*));GL(void,glGetUniformfv,(uint32_t,int,float*));GL(void,glGetUniformiv,(uint32_t,int,int*));
    GL(int,glGetProgramResourceLocation,(uint32_t,uint32_t,const char*));GL(uint32_t,glGetProgramResourceIndex,(uint32_t,uint32_t,const char*));
    GL(void,glGetProgramResourceiv,(uint32_t,uint32_t,uint32_t,int,const uint32_t*,int,int*,int*));
    GL(void,glGenTextures,(int,uint32_t*));GL(void,glActiveTexture,(uint32_t));GL(void,glBindTexture,(uint32_t,uint32_t));
    GL(void,glTexImage2D,(uint32_t,int,int,int,int,int,uint32_t,uint32_t,const void*));GL(void,glTexParameteri,(uint32_t,uint32_t,int));
    GL(void,glDrawArrays,(uint32_t,int,int));GL(void,glReadPixels,(int,int,int,int,uint32_t,uint32_t,void*));GL(uint32_t,glGetError,(void));
    const char *sources[]={
        "#version 300 es\nvoid main(){gl_Position=vec4(float((gl_VertexID<<1)&2)*2.-1.,float(gl_VertexID&2)*2.-1.,0,1);}",
        "#version 310 es\nprecision highp float;uniform float values[360];uniform float tone;layout(location = 0) uniform mediump sampler2D image;out vec4 color;void main(){color=texture(image,vec2(.5))*vec4(vec3(values[int(gl_FragCoord.x)]+tone),1);}"};
    uint32_t p=glCreateProgram();for(int i=0;i<2;i++){uint32_t s=glCreateShader(i?0x8B30:0x8B31);glShaderSource(s,1,sources+i,NULL);glCompileShader(s);int ok=0;glGetShaderiv(s,0x8B81,&ok);assert(ok);glAttachShader(p,s);}
    glLinkProgram(p);int ok=0;glGetProgramiv(p,0x8B82,&ok);assert(ok);glUseProgram(p);
    // Baked sampler write first, then named property writes, then baked again.
    glUniform1i(0,0);int image=glGetUniformLocation(p,"image");assert(image==0);
    int values=glGetUniformLocation(p,"values"),tone=glGetUniformLocation(p,"tone");assert(values>0&&tone>0&&values!=tone);
    assert(glGetProgramResourceLocation(p,0x92E1,"image")==image);
    uint32_t toneIndex=glGetProgramResourceIndex(p,0x92E1,"tone"),property=0x930E;int reflected=-1;
    glGetProgramResourceiv(p,0x92E1,toneIndex,1,&property,1,NULL,&reflected);assert(reflected==tone);
    assert(glGetUniformLocation(p,"values[16]")==values+16);
    float data[360];for(int i=0;i<360;i++)data[i]=.25f;glUniform1fv(values,360,data);glUniform1f(tone,.5f);glUniform1i(0,0);
    float actual=0;glGetUniformfv(p,values+16,&actual);assert(fabs(actual-.25)<.0001);
    glGetUniformfv(p,tone,&actual);assert(fabs(actual-.5)<.0001);int unit=-1;glGetUniformiv(p,image,&unit);assert(unit==0);
    uint32_t texture;glGenTextures(1,&texture);glActiveTexture(0x84C0+image);glBindTexture(0x0DE1,texture);
    uint8_t white[]={255,255,255,255};glTexImage2D(0x0DE1,0,0x8058,1,1,0,0x1908,0x1401,white);
    glTexParameteri(0x0DE1,0x2801,0x2600);glTexParameteri(0x0DE1,0x2800,0x2600);
    glDrawArrays(4,0,3);uint8_t pixel[4];glReadPixels(16,16,1,1,0x1908,0x1401,pixel);
    assert(abs(pixel[0]-191)<3&&abs(pixel[1]-191)<3&&abs(pixel[2]-191)<3&&pixel[3]==255);assert(!glGetError());
    puts("Uniform locations: mixed baked/named writes, compact sampler units, 360-element arrays, getter round trips and actual GPU color pass");
}
