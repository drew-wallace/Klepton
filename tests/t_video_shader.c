// Unity's multipass video variant must compile without OVR multiview. Exercise
// actual GL pixels, two per-eye draws and a side-by-side video texture. Optional
// source arguments exercise locally captured guest shaders without redistributing them.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_glfb.h"
#define GL(ret,name,args) ret(*name)args=kl_glfb_sym(#name);assert(name)
static char *read_source(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    char *s=calloc((size_t)size+1,1);assert(s&&fread(s,1,(size_t)size,f)==(size_t)size);fclose(f);return s;
}
int main(int argc,char **argv) {
    setenv("KL_GLFB","1",1);setenv("KL_GLFB_SIZE","32x16",1);setenv("KL_GLFB_VIRTUAL_UNIFORMS","1",1);
    assert(kl_glfb_init());kl_glfb_make_current();
    GL(unsigned,glCreateShader,(unsigned));GL(void,glShaderSource,(unsigned,int,const char*const*,const int*));
    GL(void,glCompileShader,(unsigned));GL(void,glGetShaderiv,(unsigned,unsigned,int*));
    GL(void,glGetShaderSource,(unsigned,int,int*,char*));GL(void,glGetShaderInfoLog,(unsigned,int,int*,char*));
    GL(unsigned,glCreateProgram,(void));GL(void,glAttachShader,(unsigned,unsigned));GL(void,glLinkProgram,(unsigned));
    GL(void,glGetProgramiv,(unsigned,unsigned,int*));GL(void,glUseProgram,(unsigned));
    GL(int,glGetUniformLocation,(unsigned,const char*));GL(void,glUniformMatrix4fv,(int,int,unsigned char,const float*));
    GL(void,glUniform1i,(int,int));GL(int,glGetAttribLocation,(unsigned,const char*));
    GL(void,glGenBuffers,(int,unsigned*));GL(void,glBindBuffer,(unsigned,unsigned));
    GL(void,glBufferData,(unsigned,long,const void*,unsigned));GL(void,glEnableVertexAttribArray,(unsigned));
    GL(void,glVertexAttribPointer,(unsigned,int,unsigned,unsigned char,int,const void*));
    GL(void,glGenTextures,(int,unsigned*));GL(void,glBindTexture,(unsigned,unsigned));
    GL(void,glTexImage2D,(unsigned,int,int,int,int,int,unsigned,unsigned,const void*));
    GL(void,glTexParameteri,(unsigned,unsigned,int));GL(void,glViewport,(int,int,int,int));
    GL(void,glDrawArrays,(unsigned,int,int));GL(void,glReadPixels,(int,int,int,int,unsigned,unsigned,void*));
    const char *vertex="#version 300 es\n#extension GL_OVR_multiview2 : require\nprecision highp float;\n"
        "#if defined(STEREO_MULTIVIEW_ON)\nlayout(num_views=2) in;\n#endif\n"
        "uniform mat4 unity_MatrixVP;uniform mat4 unity_ObjectToWorld;"
        "in vec3 in_POSITION0;in vec3 in_TEXCOORD0;out mediump vec2 uvs;"
        "void main(){gl_Position=unity_MatrixVP*unity_ObjectToWorld*vec4(in_POSITION0,1);uvs=in_TEXCOORD0.xy;}";
    const char *fragment="#version 300 es\n#extension GL_OVR_multiview2 : require\nprecision highp float;\n"
        "#if defined(STEREO_MULTIVIEW_ON)\n// Another separately compiled variant.\n#endif\n"
        "uniform sampler2D _YPlane;in mediump vec2 uvs;out vec4 color;void main(){color=texture(_YPlane,uvs);}";
    char *local[2]={0};if(argc==3){vertex=local[0]=read_source(argv[1]);fragment=local[1]=read_source(argv[2]);}
    unsigned program=glCreateProgram();const char *sources[]={vertex,fragment};
    for(int i=0;i<2;i++) {
        unsigned s=glCreateShader(i?0x8b30:0x8b31);glShaderSource(s,1,sources+i,NULL);glCompileShader(s);
        int compiled=0;glGetShaderiv(s,0x8b81,&compiled);
        if(!compiled){char log[1024]={0};glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"%s\n",log);}
        assert(compiled);glAttachShader(program,s);
    }
    glLinkProgram(program);int linked=0;glGetProgramiv(program,0x8b82,&linked);assert(linked);glUseProgram(program);
    float identity[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    glUniformMatrix4fv(glGetUniformLocation(program,"unity_MatrixVP"),1,0,identity);
    glUniformMatrix4fv(glGetUniformLocation(program,"unity_ObjectToWorld"),1,0,identity);
    glUniform1i(glGetUniformLocation(program,"_YPlane"),0);
    unsigned texture;glGenTextures(1,&texture);glBindTexture(0xde1,texture);
    unsigned char packed[]={255,0,0,255,0,255,0,255};
    glTexImage2D(0xde1,0,0x8058,2,1,0,0x1908,0x1401,packed);
    glTexParameteri(0xde1,0x2801,0x2600);glTexParameteri(0xde1,0x2800,0x2600);
    int position=glGetAttribLocation(program,"in_POSITION0"),uv=glGetAttribLocation(program,"in_TEXCOORD0");
    assert(position>=0&&uv>=0);
    unsigned buffer;glGenBuffers(1,&buffer);glBindBuffer(0x8892,buffer);
    glEnableVertexAttribArray(position);glEnableVertexAttribArray(uv);
    glVertexAttribPointer(position,3,0x1406,0,6*sizeof(float),NULL);
    glVertexAttribPointer(uv,3,0x1406,0,6*sizeof(float),(void*)(3*sizeof(float)));
    for(int eye=0;eye<2;eye++) {
        float start=eye*.5f,end=start+.5f;
        float vertices[]={-1,-1,0,start,0,0, 1,-1,0,end,0,0, -1,1,0,start,1,0, 1,1,0,end,1,0};
        glBufferData(0x8892,sizeof vertices,vertices,0x88e4);glViewport(eye*16,0,16,16);glDrawArrays(5,0,4);
        unsigned char p[4]={0};glReadPixels(eye*16+8,8,1,1,0x1908,0x1401,p);
        assert(p[eye]>220&&p[!eye]<32&&p[2]<32&&p[3]==255);
    }
    // Do not weaken a genuine multiview variant's requirement.
    const char *required="#version 300 es\n#extension GL_OVR_multiview2 : require\n#define STEREO_MULTIVIEW_ON 1\n#if defined(STEREO_MULTIVIEW_ON)\n#endif\nvoid main(){gl_Position=vec4(0);}";
    unsigned s=glCreateShader(0x8b31);glShaderSource(s,1,&required,NULL);
    char unchanged[1024]={0};glGetShaderSource(s,sizeof unchanged,NULL,unchanged);assert(strstr(unchanged,": require"));
    free(local[0]);free(local[1]);
    puts("Unity video shader: multipass compile/link, left/right SBS pixels and preserved active-multiview requirement");
}
