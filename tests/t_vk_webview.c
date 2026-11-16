// Real MoltenVK readback: channel order, rows, updates, backpressure and the
// independent lifetimes of the Java browser and Unity external texture.
#include <assert.h>
#include "../runtime/gfx/kl_vulkan.c"
static void check(uint64_t image,unsigned w,unsigned h,const unsigned char *expected) {
    klvk_device *d=g_devs[0];
    assert(klvk_QueueSubmit(d->queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(d->QueueWaitIdle(d->queue)==VK_SUCCESS);
    assert(cap_prepare(d,(VkDeviceSize)w*h*4));
    assert(d->ResetCommandBuffer(d->cap_cmd,0)==VK_SUCCESS);
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(d->BeginCommandBuffer(d->cap_cmd,&bi)==VK_SUCCESS);
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=(VkImage)image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    d->CmdPipelineBarrier(d->cap_cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={w,h,1}};
    d->CmdCopyImageToBuffer(d->cap_cmd,(VkImage)image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,d->cap_buf,1,&c);
    b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    d->CmdPipelineBarrier(d->cap_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
    assert(d->EndCommandBuffer(d->cap_cmd)==VK_SUCCESS);
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&d->cap_cmd};
    assert(d->QueueSubmit(d->queue,1,&si,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(d->QueueWaitIdle(d->queue)==VK_SUCCESS);
    void *pixels=NULL;assert(d->MapMemory(d->dev,d->cap_mem,0,VK_WHOLE_SIZE,0,&pixels)==VK_SUCCESS);
    VkMappedMemoryRange mr={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=d->cap_mem,.size=VK_WHOLE_SIZE};
    assert(d->InvalidateMappedMemoryRanges(d->dev,1,&mr)==VK_SUCCESS);
    assert(!memcmp(pixels,expected,(size_t)w*h*4));d->UnmapMemory(d->dev,d->cap_mem);
}
int main(void) {
    assert(kl_vulkan_available());
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance instance;assert(klvk_CreateInstance(&ii,NULL,&instance)==VK_SUCCESS);
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)real_gipa(instance,"vkEnumeratePhysicalDevices");
    uint32_t n=1;VkPhysicalDevice phys;assert(enumerate(instance,&n,&phys)==VK_SUCCESS && n);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device;assert(klvk_CreateDevice(phys,&di,NULL,&device)==VK_SUCCESS);
    unsigned char bgra[]={0,0,255,255,255,0,0,255,0,255,0,255,10,20,30,128};
    unsigned char rgba[]={255,0,0,255,0,0,255,255,0,255,0,255,30,20,10,128};
    assert(!kl_vulkan_webview_upload(0,4097,2,bgra));
    assert(!kl_vulkan_webview_upload(0,0,2,bgra));
    assert(!kl_vulkan_webview_upload(0,2,2,NULL));
    uint64_t image=kl_vulkan_webview_upload(0,2,2,bgra);assert(image);check(image,2,2,rgba);
    kl_vulkan_webview_publish(image);
    memset(bgra,77,sizeof bgra);memset(rgba,77,sizeof rgba);
    for(int i=0;i<100;i++)assert(kl_vulkan_webview_upload(image,2,2,bgra)==image);
    check(image,2,2,rgba);
    assert(!kl_vulkan_webview_upload(image,1,2,bgra));
    uint64_t resized=kl_vulkan_webview_upload(0,1,2,bgra);assert(resized && resized!=image);
    check(resized,1,2,rgba);
    kl_vulkan_webview_publish(resized);
    kl_vulkan_webview_close(image);klvk_webview_pump(g_devs[0]->queue);
    assert(webview_images && webview_images->next); // Unity still owns the old image.
    assert(kl_vulkan_webview_release(image));klvk_webview_pump(g_devs[0]->queue);
    assert(webview_images && !webview_images->next);
    assert(kl_vulkan_webview_release(resized));
    assert(!kl_vulkan_webview_upload(resized,1,2,bgra));
    kl_vulkan_webview_close(resized);klvk_webview_pump(g_devs[0]->queue);
    assert(!webview_images && !kl_vulkan_webview_release(123));
    image=kl_vulkan_webview_upload(0,2,2,bgra);assert(image);
    kl_vulkan_webview_close(image);klvk_webview_pump(g_devs[0]->queue);
    assert(!webview_images); // Browser closed before its queued event reached Unity.
    puts("Vulkan WebKit: real GPU pixels, updates, bounded pending frames, resize and external-texture retirement passed");
}
