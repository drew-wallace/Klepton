// External video images promise sampling, not framebuffer attachment semantics.
// In particular YCbCr cannot be attached at all. Sample both native and BGRA
// images into ordinary RGBA layers to retain the same colours and orientation.
// Raw ANGLE calls keep this internal program out of guest uniform bookkeeping.
static uint32_t klfb_copy_video_layer(uint32_t source,uint32_t destination,
                                     int width,int height,uint32_t format) {
#define VF(ret,name,args) ret (*name) args
    static struct {
        VF(uint32_t,CreateShader,(uint32_t));VF(void,ShaderSource,(uint32_t,int,const char*const*,const int*));
        VF(void,CompileShader,(uint32_t));VF(void,GetShaderiv,(uint32_t,uint32_t,int*));
        VF(uint32_t,CreateProgram,(void));VF(void,AttachShader,(uint32_t,uint32_t));
        VF(void,LinkProgram,(uint32_t));VF(void,GetProgramiv,(uint32_t,uint32_t,int*));
        VF(void,DeleteShader,(uint32_t));VF(void,UseProgram,(uint32_t));
        VF(int,GetUniformLocation,(uint32_t,const char*));VF(void,Uniform1i,(int,int));
        VF(void,GenVertexArrays,(int,uint32_t*));VF(void,BindVertexArray,(uint32_t));
        VF(void,GenSamplers,(int,uint32_t*));VF(void,BindSampler,(uint32_t,uint32_t));
        VF(void,SamplerParameteri,(uint32_t,uint32_t,int));
        VF(void,ActiveTexture,(uint32_t));VF(void,BindTexture,(uint32_t,uint32_t));
        VF(void,GetBooleanv,(uint32_t,uint8_t*));VF(void,ColorMask,(uint8_t,uint8_t,uint8_t,uint8_t));
        VF(void,Viewport,(int,int,int,int));VF(void,DrawArrays,(uint32_t,int,int));
        VF(const uint8_t*,GetString,(uint32_t));
        VF(void,PauseTransformFeedback,(void));VF(void,ResumeTransformFeedback,(void));
    } v;
#undef VF
    static int initialized,ready,decode_location;
    static uint32_t program,vao,sampler,framebuffer;
    if(!initialized) {
        initialized=1;
#define VR(name) v.name=asym("gl" #name)
        VR(CreateShader);VR(ShaderSource);VR(CompileShader);VR(GetShaderiv);
        VR(CreateProgram);VR(AttachShader);VR(LinkProgram);VR(GetProgramiv);
        VR(DeleteShader);VR(UseProgram);VR(GetUniformLocation);VR(Uniform1i);
        VR(GenVertexArrays);VR(BindVertexArray);VR(GenSamplers);VR(BindSampler);
        VR(SamplerParameteri);VR(ActiveTexture);VR(BindTexture);VR(GetBooleanv);
        VR(ColorMask);VR(Viewport);VR(DrawArrays);VR(GetString);
        VR(PauseTransformFeedback);VR(ResumeTransformFeedback);
#undef VR
        if(!v.CreateShader||!v.ShaderSource||!v.CompileShader||!v.GetShaderiv||
           !v.CreateProgram||!v.AttachShader||!v.LinkProgram||!v.GetProgramiv||
           !v.DeleteShader||!v.UseProgram||!v.GetUniformLocation||!v.Uniform1i||
           !v.GenVertexArrays||!v.BindVertexArray||!v.GenSamplers||!v.BindSampler||
           !v.SamplerParameteri||!v.ActiveTexture||!v.BindTexture||!v.GetBooleanv||
           !v.ColorMask||!v.Viewport||!v.DrawArrays||!r_IsEnabled||!r_Enable||!r_Disable)
            return 0x502;
        const char *vertex="#version 300 es\nout vec2 uv;void main(){"
            "vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
            "uv=p;gl_Position=vec4(p*2.-1.,0.,1.);}";
        const char *fragment="#version 300 es\nprecision highp float;"
            "uniform sampler2D video;uniform int decodeRGB;in vec2 uv;out vec4 color;"
            "void main(){color=texture(video,uv);if(decodeRGB!=0){"
            "vec3 lo=color.rgb/12.92,hi=pow((color.rgb+.055)/1.055,vec3(2.4));"
            "color.rgb=mix(hi,lo,lessThanEqual(color.rgb,vec3(.04045)));}}";
        uint32_t shaders[]={v.CreateShader(0x8b31),v.CreateShader(0x8b30)};
        const char *sources[]={vertex,fragment};program=v.CreateProgram();
        int success=1;
        for(int i=0;i<2;i++){
            int compiled=0;v.ShaderSource(shaders[i],1,&sources[i],NULL);
            v.CompileShader(shaders[i]);v.GetShaderiv(shaders[i],0x8b81,&compiled);
            success&=compiled;v.AttachShader(program,shaders[i]);
        }
        int linked=0;v.LinkProgram(program);v.GetProgramiv(program,0x8b82,&linked);
        for(int i=0;i<2;i++)v.DeleteShader(shaders[i]);
        if(!success||!linked){fprintf(stderr,"  [glfb] video layer copy shader failed\n");return 0x502;}
        decode_location=v.GetUniformLocation(program,"decodeRGB");
        v.GenVertexArrays(1,&vao);v.GenSamplers(1,&sampler);r_GenFramebuffers(1,&framebuffer);
        v.SamplerParameteri(sampler,0x2801,0x2601);v.SamplerParameteri(sampler,0x2800,0x2601);
        v.SamplerParameteri(sampler,0x2802,0x812f);v.SamplerParameteri(sampler,0x2803,0x812f);
        ready=program&&vao&&sampler&&framebuffer;
    }
    if(!ready)return 0x502;
    for(int i=0;i<16&&a_glGetError&&a_glGetError();i++){}
    int previous_program=0,previous_vao=0,previous_active=0,previous_texture=0;
    int previous_sampler=0,previous_framebuffer=0,viewport[4];uint8_t mask[4];
    a_glGetIntegerv(0x8b8d,&previous_program);a_glGetIntegerv(0x85b5,&previous_vao);
    // Switching away would destroy a delete-pending current program, making
    // restoration impossible. Hold this layer's previous frame until the
    // guest binds its replacement instead of changing its object lifetime.
    int deleting=0;
    if(previous_program)v.GetProgramiv(previous_program,0x8b80,&deleting);
    if(deleting)return 0x502;
    a_glGetIntegerv(0x84e0,&previous_active);a_glGetIntegerv(0x8ca6,&previous_framebuffer);
    a_glGetIntegerv(0xba2,viewport);v.GetBooleanv(0xc23,mask);
    v.ActiveTexture(0x84c0);
    a_glGetIntegerv(0x8069,&previous_texture);a_glGetIntegerv(0x8919,&previous_sampler);
    uint8_t feedback=0,paused=0;
    v.GetBooleanv(0x8e24,&feedback);v.GetBooleanv(0x8e23,&paused);
    if(feedback&&!paused){if(!v.PauseTransformFeedback||!v.ResumeTransformFeedback){v.ActiveTexture(previous_active);return 0x502;}v.PauseTransformFeedback();}
    const uint32_t caps[]={0xbe2,0xb71,0xb90,0xc11,0xb44,0x8c89,0x809e,0x80a0,0xbd0};
    uint8_t enabled[sizeof caps/sizeof *caps];
    for(unsigned i=0;i<sizeof caps/sizeof *caps;i++){enabled[i]=r_IsEnabled(caps[i]);if(enabled[i])r_Disable(caps[i]);}
    const char *extensions=v.GetString?(const char*)v.GetString(0x1f03):NULL;
    int srgb_control=extensions&&strstr(extensions,"GL_EXT_sRGB_write_control");
    int srgb_enabled=srgb_control?r_IsEnabled(0x8db9):0;
    if(srgb_enabled)r_Disable(0x8db9);
    v.ColorMask(1,1,1,1);v.Viewport(0,0,width,height);
    r_BindFramebuffer(0x8ca9,framebuffer);
    r_FramebufferTexture2D(0x8ca9,0x8ce0,0xde1,destination,0);
    uint32_t error=0;
    if(r_CheckFramebufferStatus&&r_CheckFramebufferStatus(0x8ca9)!=0x8cd5)error=0x506;
    if(!error){
        v.UseProgram(program);v.BindVertexArray(vao);v.BindTexture(0xde1,source);v.BindSampler(0,sampler);
        // When write control is unavailable, ES always encodes an sRGB target.
        // Decode first so the stored bytes retain the video's encoded values.
        v.Uniform1i(decode_location,format==0x8c43&&!srgb_control);
        error=a_glGetError?a_glGetError():0;
        if(!error){v.DrawArrays(4,0,3);error=a_glGetError?a_glGetError():0;}
    }
    r_FramebufferTexture2D(0x8ca9,0x8ce0,0xde1,0,0);
    r_BindFramebuffer(0x8ca9,previous_framebuffer);
    v.UseProgram(previous_program);v.BindVertexArray(previous_vao);
    v.BindTexture(0xde1,previous_texture);v.BindSampler(0,previous_sampler);v.ActiveTexture(previous_active);
    v.Viewport(viewport[0],viewport[1],viewport[2],viewport[3]);v.ColorMask(mask[0],mask[1],mask[2],mask[3]);
    if(srgb_enabled)r_Enable(0x8db9);
    for(unsigned i=0;i<sizeof caps/sizeof *caps;i++)if(enabled[i])r_Enable(caps[i]);
    if(feedback&&!paused)v.ResumeTransformFeedback();
    return error;
}
