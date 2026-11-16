#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kl_ovrp.h"

static uint64_t (*update)(int,int,double);
static uint64_t (*query)(int,int,int,void *);
static void publish(float marker) {
    kl_ovrp_set_head_pose(marker,1.6f,0,0,sinf(marker*.1f),0,cosf(marker*.1f));
    kl_ovrp_set_touch_pose(0,marker,1,0,0,0,0,1);
}
static void expect(int step,int frame,float marker) {
    for(int node=3;node<=9;node+=6) {
        float pose[22]={0};assert(!query(step,frame,node,pose));
        assert(pose[4]==marker);
    }
}
static void *next_frame(void *unused) {
    publish(3);assert(!update(-1,11,0));expect(-1,11,3);
    return NULL;
}
static void run(int enabled) {
    setenv("KL_OVRP_FRAME_SAMPLES",enabled?"1":"0",1);
    setenv("KL_OVRP_TOUCH_FRAME","0",1);
    update=kl_ovrp_sym("ovrp_Update3");query=kl_ovrp_sym("ovrp_GetNodePoseState3");
    assert(update&&query);
    publish(1);assert(!update(-1,10,0));expect(-1,10,1);
    publish(2);assert(!update(-1,10,0));expect(-1,10,enabled?1:2);
    pthread_t other;assert(!pthread_create(&other,NULL,next_frame,NULL));
    pthread_join(other,NULL);
    expect(-1,10,enabled?1:3);expect(-1,-1,3);
    assert(!update(-1,10,0)); // Revisit the render thread's older frame.
    expect(-1,10,enabled?1:3);expect(-1,-1,3);
    uint64_t (*begin)(int,uint64_t)=kl_ovrp_sym("ovrp_BeginFrame4");
    uint64_t (*end)(int,const void *,int,void *)=kl_ovrp_sym("ovrp_EndFrame4");
    assert(!begin(10,0)&&!end(10,NULL,0,NULL));
    kl_ovrp_render_pose pose={0};
    assert(kl_ovrp_stage_render_pose(kl_ovrp_last_complete_stage(),&pose));
    assert(pose.px==(enabled?1:3));
    // Physics still samples current tracking independently of render history.
    assert(!update(0,10,0));expect(0,10,3);
    for(int frame=12;frame<=80;frame++) {
        float marker=(float)frame*.01f;
        publish(marker);assert(!update(-1,frame,0));expect(-1,frame,marker);
    }
    // Restarting the XR frame counter must not reuse a previous generation.
    publish(5);assert(!update(-1,0,0));expect(-1,0,5);
    publish(6);assert(!update(-1,0,0));expect(-1,0,enabled?5:6);
    assert(!begin(0,0)&&!end(0,NULL,0,NULL));
    assert(kl_ovrp_stage_render_pose(kl_ovrp_last_complete_stage(),&pose));
    assert(pose.px==(enabled?5:6));
}
int main(void) {
    for(int enabled=0;enabled<=1;enabled++) {
        pid_t child=fork();assert(child>=0);
        if(!child){run(enabled);_exit(0);}
        int status;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    puts("Indexed frame samples: immutable head/hands, cross-thread lookahead, matching render record, live physics and default-off behavior");
}
