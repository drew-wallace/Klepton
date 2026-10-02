// Check guest JNI array arguments across both calling conventions. No Java VM,
// microphone or guest assets are required.
#include <assert.h>
#include "../runtime/kl_jni.c"
int main(void) {
    const char *sig="([S[F[D[[I[[Ljava/lang/Object;IFD)Z";
    // High bits distinguish a reference from its short/int/float element type.
    uintptr_t refs[]={0x123456781234,0x234567892345,0x3456789a3456,
                      0x456789ab4567,0x56789abc5678};
    klj_jvalue args[8];
    for(int i=0;i<5;i++) args[i].l=(void*)refs[i];
    args[5].i=-19; args[6].f=1.25f; args[7].d=-2.5;
    klj_val decoded[KLJ_MAX_ARGS];
    assert(klj_decode_args_a(sig,args,decoded)==8);
    for(int i=0;i<5;i++) assert(decoded[i].l==(void*)refs[i]);
    assert((int64_t)decoded[5].j==-19 && decoded[6].d==1.25 && decoded[7].d==-2.5);
    uint64_t gp[6]; for(int i=0;i<5;i++) gp[i]=refs[i]; gp[5]=(uint64_t)(int64_t)-19;
    struct {double d,pad;} fp[2]={{1.25,0},{-2.5,0}};
    kl_va va={.gr_top=gp+6,.gr_offs=-48,.vr_top=fp+2,.vr_offs=-32};
    assert(klj_decode_args(sig,&va,decoded)==8);
    for(int i=0;i<5;i++) assert(decoded[i].l==(void*)refs[i]);
    assert((int64_t)decoded[5].j==-19 && decoded[6].d==1.25 && decoded[7].d==-2.5);
    assert(va.gr_offs==0 && va.vr_offs==0);
    // Once register arguments spill, arrays still consume exactly one GP slot.
    uint64_t stack[4]={refs[0],refs[1],(uint64_t)(int64_t)-7,0};
    double scalar=3.5; memcpy(&stack[3],&scalar,8);
    va=(kl_va){.stack=stack};
    assert(klj_decode_args("([F[[SID)V",&va,decoded)==4);
    assert(decoded[0].l==(void*)refs[0] && decoded[1].l==(void*)refs[1]);
    assert((int64_t)decoded[2].j==-7 && decoded[3].d==3.5 && va.stack==stack+4);
    const char *bad[]={"([)V","([V)V","([[)V","([L;)V","([Ljava/lang/Object)V"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {
        va=(kl_va){.stack=stack};
        assert(klj_decode_args_a(bad[i],args,decoded)==-1);
        assert(klj_decode_args(bad[i],&va,decoded)==-1);
    }
    assert(klj_decode_args_a("()V",NULL,decoded)==0);
    puts("JNI arrays preserve reference width, GP/FP banks, following scalars and stack slots");
}
