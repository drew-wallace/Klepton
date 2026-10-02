// Exercise the actual indexed-draw wrapper using recorded Vulkan calls.
// No guest, GPU, game content or credentials are needed.
#include <assert.h>
#include "../runtime/gfx/kl_vulkan.c"
static unsigned draws, binds;
static int32_t drawn_base;
static uint64_t seen_offset, restored_offset;
static void VKAPI_CALL fixture_draw(void *cb, uint32_t count, uint32_t instances,
                                    uint32_t first, int32_t base, uint32_t instance) {
    assert(count==3 && instances==2 && first==7 && instance==11);
    draws++; drawn_base=base;
}
static void fixture_bind(void *cb, uint32_t first, uint32_t count,
                         const uint64_t *buffers, const uint64_t *offsets) {
    assert(first==3 && count==1 && buffers[0]==123);
    if (!binds) seen_offset=offsets[0]; else restored_offset=offsets[0];
    binds++;
}
static PFN_vkVoidFunction VKAPI_CALL fixture_gdpa(VkDevice dev, const char *name) {
    if (!strcmp(name,"vkCmdDrawIndexed")) return (PFN_vkVoidFunction)fixture_draw;
    if (!strcmp(name,"vkCmdBindVertexBuffers")) return (PFN_vkVoidFunction)fixture_bind;
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc==2); int native=atoi(argv[1]);
    unsetenv("KL_VK_EMULATE_BASE_VERTEX"); unsetenv("KL_VK_DRAW_GUARD");
    kl_driver_init(kl_target_lookup("walkabout-57013"),"",KL_SLINK_CLIENT);
    klvk_device device={.native_base_vertex=native}; g_devs[0]=&device; g_ndev=1;
    real_gdpa=fixture_gdpa;
    void *cb=(void*)1; VkPipeline pipeline=(VkPipeline)2;
    klvk_cb_set_idx(cb,99); klvk_cb_set_pipeline(cb,pipeline);
    uint64_t buffers[]={123},offsets[]={64}; klvk_cb_set_vertices(cb,3,1,buffers,offsets);
    VkVertexInputBindingDescription binding={.binding=3,.stride=16,.inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi={.vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding};
    klvk_pipe_meta_add(pipeline,&vi);
    klvk_CmdDrawIndexed(cb,3,2,7,12,11);
    assert(draws==1);
    if (native==0) {
        assert(drawn_base==0 && binds==2 && seen_offset==256 && restored_offset==64);
    } else {
        // A shader using VertexIndex must still receive the original base.
        // The old target-only default clears it even on a supported GPU.
        assert(drawn_base==12 && binds==0);
    }
    puts(native==0 ? "Unsupported-device rebase and sparse-binding restore passed"
                   : "Native indexed draw preserves vertex offset and shader vertex index");
}
