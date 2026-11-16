// A compositor must keep the previous pose/image visible while Vulkan waits
// for the next image. No GPU is needed to exercise that publication boundary.
#include <assert.h>
#include <stdio.h>
#define kl_vulkan_guest_active test_vulkan_active
#define kl_vulkan_frame_done test_vulkan_frame_done
#define kl_vulkan_capture_eyes test_vulkan_capture_eyes
#include "../runtime/xr/kl_ovrp.c"
#undef kl_vulkan_guest_active
#undef kl_vulkan_frame_done
#undef kl_vulkan_capture_eyes

static int expected_previous=-1, waits, captures;
int test_vulkan_active(void) { return 1; }
void test_vulkan_capture_eyes(unsigned frame,int stage) { captures++; }
void test_vulkan_frame_done(int stage) {
    // This represents a blocking GPU wait. During it, all public completion
    // metadata must still describe the old image, even when a stage is reused.
    assert(kl_ovrp_last_complete_stage()==expected_previous);
    assert(g_frames.pending.complete==0);
    kl_ovrp_render_pose pose={0};
    int next=(int)((g_frames.serial-1)%kl_ovrp_stage_count());
    if(kl_ovrp_stage_render_pose(next,&pose))
        assert(pose.serial<g_frames.pending.serial);
    waits++;
}
int main(void) {
    setenv("KL_OVRP_STAGES","3",1);
    for(int frame=0;frame<7;frame++) {
        kl_ovrp_set_head_pose((float)frame,1.6f,0,0,0,0,1);
        kl_ovrp_frame_latch();
        assert(!klovrp_BeginFrame4(frame,0));
        assert(!klovrp_EndFrame4(frame,NULL,0,NULL));
        int stage=frame%3;
        assert(kl_ovrp_last_complete_stage()==stage);
        kl_ovrp_render_pose pose={0};
        assert(kl_ovrp_stage_render_pose(stage,&pose));
        assert(pose.complete&&pose.serial==(uint64_t)frame+1&&pose.px==(float)frame);
        expected_previous=stage;
    }
    assert(waits==7&&captures==7);
    puts("Vulkan frame publication: GPU completion precedes pose/stage publication, including stage reuse");
}
