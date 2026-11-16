#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_unity_compat.h"

static const char *package="com.LuckyVR.PokerStarsVR",*image="libOculusXRPlugin.so";
static long version=1102036;
static size_t offset=0xd1ec;
static const void *expected_version;
static unsigned calls;
const char *kl_jni_guest_package(void) { return package; }
void kl_jni_guest_version(long *code,const char **name) { if(code)*code=version;if(name)*name=NULL; }
const char *kl_addr_image(const void *p,size_t *out) { if(out)*out=offset;return image; }
static void vendor_settings(void *settings,const void *unity_version) {
    assert(unity_version==expected_version);
    uint16_t *s=settings;
    for(unsigned i=0;i<15;i++)assert(s[i]==(i==11?0:(uint16_t)(0x400+i)));
    s[14]=7; // Preserve the vendor setter's normalization writeback.
    calls++;
}
static int vendor_vivox(const void *config,size_t size) { calls++;return 0; }
int main(void) {
    struct {uint16_t before[2],settings[15],after[2];} record;
    memset(&record,0xa5,sizeof record);
    for(unsigned i=0;i<15;i++)record.settings[i]=(uint16_t)(0x400+i);
    int unity_version[]={6000,3,9,'f',1};expected_version=unity_version;
    void (*wrapped)(void*,const void*)=kl_unity_oculus_interpose("SetUserDefinedSettings",vendor_settings);
    assert(wrapped);wrapped(record.settings,unity_version);
    assert(calls==1&&record.before[0]==0xa5a5&&record.before[1]==0xa5a5);
    assert(record.after[0]==0xa5a5&&record.after[1]==0xa5a5);
    assert(record.settings[11]==0&&record.settings[14]==7);
    assert(unity_version[0]==6000&&unity_version[3]=='f');
    assert(!kl_unity_oculus_interpose("SetSpaceWarp",vendor_settings));
    version++;assert(!kl_unity_oculus_interpose("SetUserDefinedSettings",vendor_settings));version--;
    package="com.example.other";assert(!kl_unity_oculus_interpose("SetUserDefinedSettings",vendor_settings));package="com.LuckyVR.PokerStarsVR";
    image="other.so";assert(!kl_unity_oculus_interpose("SetUserDefinedSettings",vendor_settings));image="libOculusXRPlugin.so";
    offset++;assert(!kl_unity_oculus_interpose("SetUserDefinedSettings",vendor_settings));offset--;
    setenv("KL_VEGAS_SPACEWARP","1",1);assert(!kl_unity_oculus_interpose("SetUserDefinedSettings",vendor_settings));
    image="libvivox-sdk.so";offset=0x8a3410;unsigned before=calls;
    int (*voice)(const void*,size_t)=kl_unity_vivox_interpose("vx_initialize3",vendor_vivox);
    assert(voice && voice(&record,sizeof record)==5018 && calls==before);
    assert(!kl_unity_vivox_interpose("vx_initialize",vendor_vivox));
    version++;assert(!kl_unity_vivox_interpose("vx_initialize3",vendor_vivox));version--;
    package="com.example.other";assert(!kl_unity_vivox_interpose("vx_initialize3",vendor_vivox));package="com.LuckyVR.PokerStarsVR";
    image="other.so";assert(!kl_unity_vivox_interpose("vx_initialize3",vendor_vivox));image="libvivox-sdk.so";
    offset++;assert(!kl_unity_vivox_interpose("vx_initialize3",vendor_vivox));offset--;
    setenv("KL_VEGAS_VIVOX","1",1);assert(!kl_unity_vivox_interpose("vx_initialize3",vendor_vivox));
    puts("Unity Oculus settings: audited ABI, isolated SpaceWarp flag, version forwarding, normalization writeback and scope guards");
}
