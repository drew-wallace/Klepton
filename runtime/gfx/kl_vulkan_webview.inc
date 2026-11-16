// Included after the image registry helpers. No queue calls from WebKit or
// Java threads: all transfers and retirement happen at the guest submit seam.
typedef struct klvk_webview {
    struct klvk_webview *next;
    klvk_device *d;
    VkImage image;
    VkDeviceMemory memory, staging_memory;
    VkBuffer staging;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    void *mapped, *pending;
    unsigned w,h;
    int owner, guest, released, submitted, initialized;
} klvk_webview;
static pthread_mutex_t webview_lock=PTHREAD_MUTEX_INITIALIZER;
static klvk_webview *webview_images;

static void webview_free(klvk_webview *v) {
    klvk_device *d=v->d;
    if(v->mapped)d->UnmapMemory(d->dev,v->staging_memory);
    if(v->fence)d->DestroyFence(d->dev,v->fence,NULL);
    if(v->pool)d->DestroyCommandPool(d->dev,v->pool,NULL);
    if(v->staging)d->DestroyBuffer(d->dev,v->staging,NULL);
    if(v->staging_memory)d->FreeMemory(d->dev,v->staging_memory,NULL);
    if(v->image) {klvk_img_reg_del(v->image);d->DestroyImage(d->dev,v->image,NULL);}
    if(v->memory)d->FreeMemory(d->dev,v->memory,NULL);
    free(v->pending);free(v);
}
static klvk_webview *webview_allocate(klvk_device *d,unsigned w,unsigned h) {
    if(!d || !d->CmdCopyBufferToImage || !d->GetFenceStatus)return NULL;
    klvk_webview *v=calloc(1,sizeof *v);if(!v)return NULL;
    v->d=d;v->w=w;v->h=h;v->owner=1;
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(d->CreateImage(d->dev,&ic,NULL,&v->image)!=VK_SUCCESS)goto fail;
    VkMemoryRequirements mr;d->GetImageMemoryRequirements(d->dev,v->image,&mr);
    uint32_t mt=mem_type(d,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(mt==UINT32_MAX)goto fail;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};
    if(d->AllocateMemory(d->dev,&ai,NULL,&v->memory)!=VK_SUCCESS ||
       d->BindImageMemory(d->dev,v->image,v->memory,0)!=VK_SUCCESS)goto fail;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(VkDeviceSize)w*h*4,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(d->CreateBuffer(d->dev,&bi,NULL,&v->staging)!=VK_SUCCESS)goto fail;
    d->GetBufferMemoryRequirements(d->dev,v->staging,&mr);
    mt=mem_type(d,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(mt==UINT32_MAX)goto fail; // Never upload unflushed noncoherent memory.
    ai.allocationSize=mr.size;ai.memoryTypeIndex=mt;
    if(d->AllocateMemory(d->dev,&ai,NULL,&v->staging_memory)!=VK_SUCCESS ||
       d->BindBufferMemory(d->dev,v->staging,v->staging_memory,0)!=VK_SUCCESS ||
       d->MapMemory(d->dev,v->staging_memory,0,VK_WHOLE_SIZE,0,&v->mapped)!=VK_SUCCESS)goto fail;
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=d->queue_family};
    if(d->CreateCommandPool(d->dev,&pi,NULL,&v->pool)!=VK_SUCCESS)goto fail;
    VkCommandBufferAllocateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=v->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(d->AllocateCommandBuffers(d->dev,&ci,&v->cmd)!=VK_SUCCESS)goto fail;
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(d->CreateFence(d->dev,&fi,NULL,&v->fence)!=VK_SUCCESS)goto fail;
    klvk_img_reg_add(v->image);
    return v;
fail:
    webview_free(v);return NULL;
}
unsigned long long kl_vulkan_webview_upload(unsigned long long image,unsigned w,unsigned h,const void *bgra) {
    if(!bgra || !w || !h || w>4096 || h>4096)return 0;
    size_t bytes=(size_t)w*h*4;
    unsigned char *rgba=malloc(bytes);if(!rgba)return 0;
    const unsigned char *src=bgra;
    for(size_t i=0;i<bytes;i+=4) {rgba[i]=src[i+2];rgba[i+1]=src[i+1];rgba[i+2]=src[i];rgba[i+3]=src[i+3];}
    pthread_mutex_lock(&webview_lock);
    klvk_webview *v=webview_images;
    while(v && (unsigned long long)v->image!=image)v=v->next;
    if(image && (!v || !v->owner || v->released || v->w!=w || v->h!=h))goto fail;
    if(!image) {
        v=webview_allocate(g_ndev?g_devs[0]:NULL,w,h);if(!v)goto fail;
        v->next=webview_images;webview_images=v;
        VKI("WebKit Vulkan image %p %ux%u\n",(void*)v->image,w,h);
    }
    free(v->pending);v->pending=rgba;
    unsigned long long result=(unsigned long long)v->image;
    pthread_mutex_unlock(&webview_lock);return result;
fail:
    pthread_mutex_unlock(&webview_lock);free(rgba);return 0;
}
void kl_vulkan_webview_close(unsigned long long image) {
    pthread_mutex_lock(&webview_lock);
    for(klvk_webview *v=webview_images;v;v=v->next)if((unsigned long long)v->image==image)v->owner=0;
    pthread_mutex_unlock(&webview_lock);
}
void kl_vulkan_webview_publish(unsigned long long image) {
    pthread_mutex_lock(&webview_lock);
    for(klvk_webview *v=webview_images;v;v=v->next)if((unsigned long long)v->image==image && !v->released)v->guest=1;
    pthread_mutex_unlock(&webview_lock);
}
int kl_vulkan_webview_release(unsigned long long image) {
    int found=0;pthread_mutex_lock(&webview_lock);
    for(klvk_webview *v=webview_images;v;v=v->next)if((unsigned long long)v->image==image) {v->guest=0;v->released=1;found=1;break;}
    pthread_mutex_unlock(&webview_lock);return found;
}
static void klvk_webview_pump(VkQueue queue) {
    pthread_mutex_lock(&webview_lock);
    klvk_webview **slot=&webview_images;
    while(*slot) {
        klvk_webview *v=*slot;klvk_device *d=v->d;
        if(queue!=d->queue) {slot=&v->next;continue;}
        if(!v->owner && !v->guest) {
            // Unity has released its external texture and Java has closed it.
            // Drain preceding readers only on retirement, never per frame.
            if(d->QueueWaitIdle(queue)!=VK_SUCCESS) {slot=&v->next;continue;}
            *slot=v->next;webview_free(v);continue;
        }
        if(!v->pending || !v->owner || v->released ||
           (v->submitted && d->GetFenceStatus(d->dev,v->fence)!=VK_SUCCESS)) {slot=&v->next;continue;}
        if(d->ResetCommandBuffer(v->cmd,0)!=VK_SUCCESS) {slot=&v->next;continue;}
        VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if(d->BeginCommandBuffer(v->cmd,&bi)!=VK_SUCCESS) {slot=&v->next;continue;}
        memcpy(v->mapped,v->pending,(size_t)v->w*v->h*4);
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=v->initialized?VK_ACCESS_MEMORY_READ_BIT:0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=v->initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=v->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        d->CmdPipelineBarrier(v->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={v->w,v->h,1}};
        d->CmdCopyBufferToImage(v->cmd,v->staging,v->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        d->CmdPipelineBarrier(v->cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
        if(d->EndCommandBuffer(v->cmd)==VK_SUCCESS && d->ResetFences(d->dev,1,&v->fence)==VK_SUCCESS) {
            VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&v->cmd};
            if(d->QueueSubmit(queue,1,&si,v->fence)==VK_SUCCESS) {
                if(!v->initialized)VKI("WebKit Vulkan first upload %p\n",(void*)v->image);
                v->submitted=v->initialized=1;free(v->pending);v->pending=NULL;
            } else v->submitted=0;
        }
        slot=&v->next;
    }
    pthread_mutex_unlock(&webview_lock);
}
