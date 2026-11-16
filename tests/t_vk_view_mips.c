// Exercise the actual image/view hooks with a recorded backend; no GPU needed.
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../runtime/gfx/kl_vulkan.c"

static VkImage image=(VkImage)(uintptr_t)0x123456;
static VkImageViewCreateInfo seen;
static unsigned rejected_creates;
static uint32_t backend_mips;
static VkResult VKAPI_CALL create_image(VkDevice device,const VkImageCreateInfo *ci,
                                        const VkAllocationCallbacks *a,VkImage *out) {
    if(rejected_creates) {rejected_creates--;*out=VK_NULL_HANDLE;return VK_ERROR_FORMAT_NOT_SUPPORTED;}
    backend_mips=ci->mipLevels;*out=image;return VK_SUCCESS;
}
static VkResult VKAPI_CALL create_view(VkDevice device,const VkImageViewCreateInfo *ci,
                                       const VkAllocationCallbacks *a,VkImageView *out) {
    seen=*ci;*out=(VkImageView)(uintptr_t)0x234567;return VK_SUCCESS;
}
static void VKAPI_CALL destroy_image(VkDevice device,VkImage img,const VkAllocationCallbacks *a) {
    assert(img==image);
}
static PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice device,const char *name) {
    if(!strcmp(name,"vkCreateImage"))return (PFN_vkVoidFunction)create_image;
    if(!strcmp(name,"vkCreateImageView"))return (PFN_vkVoidFunction)create_view;
    if(!strcmp(name,"vkDestroyImage"))return (PFN_vkVoidFunction)destroy_image;
    return NULL;
}
static void metadata(const char *root,const char *package,long version) {
    static char assets[1024];
    char path[1024];snprintf(path,sizeof path,"%s/AndroidManifest.xml",root);
    FILE *f=fopen(path,"w");assert(f);fprintf(f,"<manifest package=\"%s\"/>",package);fclose(f);
    snprintf(path,sizeof path,"%s/apktool.yml",root);f=fopen(path,"w");assert(f);
    fprintf(f,"versionCode: '%ld'\n",version);fclose(f);
    snprintf(assets,sizeof assets,"%s/assets",root);kl_jni_set_assets_dir(assets);
}
static void allocate(uint32_t mips,VkImageUsageFlags usage) {
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_ASTC_6x6_SRGB_BLOCK,.extent={64,64,1},.mipLevels=mips,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage};
    VkImageCreateInfo original=ci;VkImage out=VK_NULL_HANDLE;
    assert(klvk_CreateImage(NULL,&ci,NULL,&out)==VK_SUCCESS && out==image);
    assert(!memcmp(&ci,&original,sizeof ci));
}
static void view(uint32_t base,uint32_t count,uint32_t expected) {
    VkImageViewCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,base,count,0,1}};
    VkImageViewCreateInfo original=ci;VkImageView out=VK_NULL_HANDLE;
    assert(klvk_CreateImageView(NULL,&ci,NULL,&out)==VK_SUCCESS && out);
    assert(!memcmp(&ci,&original,sizeof ci));
    original.subresourceRange.levelCount=expected;
    assert(!memcmp(&seen,&original,sizeof seen));
}
int main(void) {
    char root[]="/tmp/klepton-vk-mips.XXXXXX";assert(mkdtemp(root));
    char path[1024];snprintf(path,sizeof path,"%s/assets",root);assert(!mkdir(path,0700));
    char *original=strdup(kl_jni_assets_dir());assert(original);
    unsetenv("KL_VK_ASTC_RGBA8");unsetenv("KL_VEGAS_VIEW_MIPS");real_gdpa=gdpa;
    metadata(root,"com.LuckyVR.PokerStarsVR",1102036);
    allocate(7,VK_IMAGE_USAGE_SAMPLED_BIT);
    view(0,9,7);view(2,9,5);view(0,7,7);view(2,4,4);
    view(0,VK_REMAINING_MIP_LEVELS,VK_REMAINING_MIP_LEVELS);
    setenv("KL_VEGAS_VIEW_MIPS","0",1);view(0,9,9);unsetenv("KL_VEGAS_VIEW_MIPS");
    metadata(root,"com.LuckyVR.PokerStarsVR",1102037);view(0,9,9);
    metadata(root,"com.example.other",1102036);view(0,9,9);
    metadata(root,"com.LuckyVR.PokerStarsVR",1102036);
    klvk_DestroyImage(NULL,image,NULL);
    uint32_t mips=99;assert(!klvk_img_desc(image,NULL,NULL,NULL,NULL,NULL,&mips));
    allocate(9,VK_IMAGE_USAGE_SAMPLED_BIT);view(0,9,9); // Recycled handle gets its new extent.
    klvk_DestroyImage(NULL,image,NULL);
    allocate(1,VK_IMAGE_USAGE_SAMPLED_BIT);view(0,9,1);
    klvk_DestroyImage(NULL,image,NULL);
    allocate(7,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);view(0,9,9);
    klvk_DestroyImage(NULL,image,NULL);
    rejected_creates=2;allocate(7,VK_IMAGE_USAGE_SAMPLED_BIT);
    assert(backend_mips==1 && klvk_img_desc(image,NULL,NULL,NULL,NULL,NULL,&mips) && mips==1);
    klvk_DestroyImage(NULL,image,NULL);
    kl_jni_set_assets_dir(original);free(original);
    snprintf(path,sizeof path,"%s/AndroidManifest.xml",root);unlink(path);
    snprintf(path,sizeof path,"%s/apktool.yml",root);unlink(path);
    snprintf(path,sizeof path,"%s/assets",root);rmdir(path);rmdir(root);
    puts("Vulkan views: resident mip bounds, scope, unchanged descriptors, remaining levels, recycled handles and actual fallback allocation");
}
