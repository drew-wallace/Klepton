// A simulator aim ray must survive the same frame and node queries Unity uses.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_ovrp.h"

static void check(int correction) {
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        setenv("KL_OVRP_TOUCH_FRAME",correction?"1":"0",1);
        unsetenv("KL_OVRP_HANDS_IN_VIEW");
        uint64_t (*update)(int,int,double)=kl_ovrp_sym("ovrp_Update2");
        uint64_t (*query)(int,int,int,void*)=kl_ovrp_sym("ovrp_GetNodePoseState3");
        assert(update&&query);
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
                update(step,(int)i,0);
                for (int node=3;node<=4;node++) {
                    float pose[22]={0}; query(-1,step,node,pose);
                    for (int j=0;j<7;j++) assert(fabsf(pose[j]-s[j])<.00001f);
                    for (int j=7;j<22;j++) assert(pose[j]==0);
                }
            }
        }
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(void) {
    check(0); check(1);
    puts("Touch aim: exact Unity node poses for both hands, render/physics steps, rotated rays and correction on/off");
}
