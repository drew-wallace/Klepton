// A simulator aim ray must survive the same frame and node queries Unity uses.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kl_ovrp.h"
#include "kl_jni.h"

static void compatibility_scope(void) {
    char root[]="/tmp/klepton-adapter.XXXXXX";
    assert(mkdtemp(root));
    char assets[1024],manifest[1024],yaml[1024];
    snprintf(assets,sizeof assets,"%s/assets",root);assert(!mkdir(assets,0700));
    snprintf(manifest,sizeof manifest,"%s/AndroidManifest.xml",root);
    snprintf(yaml,sizeof yaml,"%s/apktool.yml",root);
    const char *original=kl_jni_assets_dir();
    uint64_t (*query)(const void **)=kl_ovrp_sym("ovrp_GetDisplayAdapterId2");
    const struct {const char *package,*mode;long version;int no_preference;
                  const char *frustum_mode;int unified,frame_samples;} cases[]={
        {"com.LuckyVR.PokerStarsVR",NULL,1102036,1,NULL,1,1},
        {"com.LuckyVR.PokerStarsVR",NULL,1102037,0,NULL,0,0},
        {"com.example.other",NULL,1102036,0,NULL,0,0},
        {"com.LuckyVR.PokerStarsVR","legacy",1102036,0,"0",0,1},
        {"com.example.other","none",1102036,1,"1",1,0},
    };
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        FILE *f=fopen(manifest,"w");assert(f);
        fprintf(f,"<manifest package=\"%s\"/>",cases[i].package);fclose(f);
        f=fopen(yaml,"w");assert(f);fprintf(f,"versionCode: '%ld'\n",cases[i].version);fclose(f);
        if(cases[i].mode)setenv("KL_OVRP_ADAPTER_ID",cases[i].mode,1);else unsetenv("KL_OVRP_ADAPTER_ID");
        kl_jni_set_assets_dir(assets);
        const void *value=(void *)1;assert(!query(&value));
        assert((value==NULL)==cases[i].no_preference);
        // Each child starts with an unread frustum policy, just like an app
        // launch. Exercise a canted display: symmetric host FOVs hide this bug.
        pid_t child=fork();assert(child>=0);
        if(!child) {
            setenv("KL_OVRP_EYE_TAN","vision",1);
            if(cases[i].frustum_mode)setenv("KL_OVRP_UNIFY_FRUSTUM",cases[i].frustum_mode,1);
            else unsetenv("KL_OVRP_UNIFY_FRUSTUM");
            uint64_t (*frustum)(int,void*)=kl_ovrp_sym("ovrp_GetNodeFrustum2");assert(frustum);
            for(int eye=0;eye<2;eye++) {
                float f[6]={0},t[4];assert(!frustum(eye,f));
                kl_ovrp_eye_tangents(eye,t);
                const float left=cases[i].unified||eye==0?1.73205f:1.0f;
                const float right=cases[i].unified||eye==1?1.73205f:1.0f;
                assert(fabsf(f[4]-left)<.00001f && fabsf(f[5]-right)<.00001f);
                assert(fabsf(t[0]-f[4])<.00001f && fabsf(t[1]-f[5])<.00001f);
                assert(fabsf(t[2]-f[2])<.00001f && fabsf(t[3]-f[3])<.00001f);
            }
            unsetenv("KL_OVRP_FRAME_SAMPLES");
            uint64_t (*update)(int,int,double)=kl_ovrp_sym("ovrp_Update3");
            uint64_t (*pose)(int,int,int,void*)=kl_ovrp_sym("ovrp_GetNodePoseState3");
            kl_ovrp_set_head_pose(1,1.6f,0,0,0,0,1);assert(!update(-1,7,0));
            kl_ovrp_set_head_pose(2,1.6f,0,0,0,0,1);assert(!update(-1,7,0));
            float state[22]={0};assert(!pose(-1,7,9,state));
            assert(state[4]==(cases[i].frame_samples?1:2));
            _exit(0);
        }
        int status;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    unsetenv("KL_OVRP_ADAPTER_ID");kl_jni_set_assets_dir(original);
    unlink(manifest);unlink(yaml);rmdir(assets);rmdir(root);
}

static void check(int correction) {
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        setenv("KL_OVRP_TOUCH_FRAME",correction?"1":"0",1);
        unsetenv("KL_OVRP_HANDS_IN_VIEW");
        uint64_t (*updates[])(int,int,double)={kl_ovrp_sym("ovrp_Update2"),kl_ovrp_sym("ovrp_Update3")};
        uint64_t (*query)(int,int,int,void*)=kl_ovrp_sym("ovrp_GetNodePoseState3");
        assert(updates[0]&&updates[1]&&query);
        const float samples[][7]={
            {0,0,0,1, .1f,1.6f,-.4f},
            {.1825742f,.3651484f,-.5477226f,.7302967f, -.3f,1.4f,-1.2f},
            {-.5f,.5f,.5f,.5f, .4f,.8f,.2f},
        };
        for (unsigned i=0;i<sizeof samples/sizeof *samples;i++) {
            const float *s=samples[i];
            for (int hand=0;hand<2;hand++)
                kl_ovrp_set_touch_pose(hand,s[4],s[5],s[6],s[0],s[1],s[2],s[3]);
            for (int step=-1;step<=0;step++) {
              for (int api=0;api<2;api++) {
                // Update2 returns ovrpBool; Update3 returns ovrpResult.
                // The APK's Update2 converts a nonnegative Update3 result to 1.
                assert(updates[api](step,(int)i,0)==(api==0?1:0));
                for (int node=3;node<=4;node++) {
                    float pose[22]={0}; query(-1,step,node,pose);
                    for (int j=0;j<7;j++) assert(fabsf(pose[j]-s[j])<.00001f);
                    for (int j=7;j<22;j++) assert(pose[j]==0);
                }
              }
            }
        }
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(void) {
    compatibility_scope();
    int32_t (*hands_enabled)(void) = kl_ovrp_sym("ovrp_GetHandTrackingEnabled");
    uint64_t (*adapter)(const void **) = kl_ovrp_sym("ovrp_GetDisplayAdapterId2");
    assert(hands_enabled && adapter && hands_enabled() == 0);
    const char *hand_queries[]={"ovrp_GetHandState2","ovrp_GetHandState3"};
    for(unsigned i=0;i<2;i++) {
        uint64_t (*query)(int,int,void*)=kl_ovrp_sym(hand_queries[i]);assert(query);
        unsigned char state[1024],original[1024];memset(state,0xa5,sizeof state);memcpy(original,state,sizeof state);
        for(int hand=0;hand<2;hand++) for(int step=0;step<2;step++) {
            assert((int32_t)query(step,hand,state)==-1004);
            assert(!memcmp(state,original,sizeof state));
        }
    }
    setenv("KL_OVRP_ADAPTER_ID", "none", 1);
    const void *preferred = (void *)1;
    assert(adapter(&preferred) == 0 && preferred == NULL);
    unsetenv("KL_OVRP_ADAPTER_ID");
    check(0); check(1);
    setenv("KL_OVRP_TOUCH_FRAME","0",1);
    uint64_t (*origin)(int)=kl_ovrp_sym("ovrp_SetTrackingOriginType2");
    assert(origin);
    assert(kl_ovrp_tracking_head_y(1.1f)==1.1f); // Default/OpenXR remains physical.
    assert(!origin(0));
    assert(kl_ovrp_tracking_head_y(1.1f)==0);
    assert(fabsf(kl_ovrp_tracking_head_y(1.3f)-.2f)<.00001f);
    assert(!origin(0)); // Repeated requests must not erase head movement.
    assert(fabsf(kl_ovrp_tracking_head_y(1.3f)-.2f)<.00001f);
    float pos[3],quat[4],vel[3],ang[3];
    kl_ovrp_set_touch_pose(0,0,.8f,0,0,0,0,1);
    assert(kl_ovrp_hand_motion(0,pos,quat,vel,ang));
    assert(fabsf(pos[1]+.3f)<.00001f); // One shared offset, never twice.
    kl_ovrp_set_hand_pose(1,0,.9f,0,0,0,0,1);
    assert(kl_ovrp_hand_motion(1,pos,quat,vel,ang));
    assert(fabsf(pos[1]+.2f)<.00001f);
    kl_ovrp_reset_eye_level_height();
    assert(kl_ovrp_tracking_head_y(1.3f)==0);
    for(int mode=1;mode<=2;mode++) {
        assert(!origin(mode));
        assert(kl_ovrp_tracking_head_y(1.3f)==1.3f);
        kl_ovrp_set_hand_motion(0,0,.8f,0,0,0,0,1,0,0,0,0,0,0);
        assert(kl_ovrp_hand_motion(0,pos,quat,vel,ang)&&pos[1]==.8f);
    }
    assert(!origin(0));
    assert(isnan(kl_ovrp_tracking_head_y(NAN)));
    assert(kl_ovrp_tracking_head_y(1.0f)==0); // Invalid samples never calibrate.
    puts("Touch aim: exact Unity node poses for both hands, render/physics steps, rotated rays and correction on/off");
    puts("Tracking height: seated EyeLevel, relative movement, shared controller offset, recenter, Floor and Stage");
}
