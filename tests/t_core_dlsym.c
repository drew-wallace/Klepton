// Unity must not discover an unrelated graphics plugin through libc's handle.
#include "klepton.h"
#include "kl_driver.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put16(unsigned char *p, uint16_t n) { memcpy(p, &n, 2); }
static void put32(unsigned char *p, uint32_t n) { memcpy(p, &n, 4); }
static void put64(unsigned char *p, uint64_t n) { memcpy(p, &n, 8); }

int main(void) {
    // A data-only AArch64 ELF exporting the same callback Unity probes for.
    // No proprietary APK or Android toolchain is needed, and it is never called.
    unsigned char elf[4096] = {0};
    memcpy(elf, "\177ELF\2\1\1", 7);
    put16(elf+16, 3); put16(elf+18, 183); put32(elf+20, 1);
    put64(elf+32, 64); put16(elf+52, 64); put16(elf+54, 56); put16(elf+56, 2);
    put32(elf+64, 1); put32(elf+68, 4);
    put64(elf+96, sizeof elf); put64(elf+104, sizeof elf); put64(elf+112, 4096);
    put32(elf+120, 2); put32(elf+124, 4);
    put64(elf+128, 256); put64(elf+136, 256);
    put64(elf+152, 80); put64(elf+160, 80); put64(elf+168, 8);
    put64(elf+256, 6); put64(elf+264, 512);  // DT_SYMTAB
    put64(elf+272, 5); put64(elf+280, 560);  // DT_STRTAB
    put64(elf+288, 4); put64(elf+296, 640);  // DT_HASH: two symbols
    put32(elf+640, 1); put32(elf+644, 2);
    put32(elf+648, 1);
    put32(elf+536, 1); elf[540] = 0x11; put16(elf+542, 1);
    put64(elf+544, 768); put64(elf+552, 8);
    memcpy(elf+561, "UnityPluginLoad", sizeof "UnityPluginLoad");
    char path[] = "build/core-dlsym-fixture.XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(write(fd, elf, sizeof elf) == sizeof elf && !close(fd));
    kl_image *img = kl_load(path); assert(img);
    kl_register_image("graphics-plugin.so", img);
    void *plugin = kl_sym(img, "UnityPluginLoad"); assert(plugin);

    kl_target target = {.name="core-dlsym-test", .kind=KL_GUEST_UNITY};
    kl_driver_init(&target, "build", 0);
    void *(*open_library)(const char *, int) = kl_shim_lookup("dlopen");
    void *(*symbol)(void *, const char *) = kl_shim_lookup("dlsym");
    char *(*error)(void) = kl_shim_lookup("dlerror");
    int (*close_library)(void *) = kl_shim_lookup("dlclose");
    assert(open_library && symbol && error && close_library);
    const char *libraries[] = {"libc.so", "/system/lib64/libc.so", "libdl.so", "libm.so"};
    for (unsigned i=0; i<sizeof libraries/sizeof libraries[0]; ++i) {
        void *handle = open_library(libraries[i], 2); assert(handle);
        intmax_t (*absolute)(intmax_t)=symbol(handle,"imaxabs");
        assert(absolute && (void*)absolute==kl_shim_lookup("imaxabs"));
        assert(absolute(0)==0 && absolute(-17)==17);
        assert(absolute(-INT64_C(0x100000001))==INT64_C(0x100000001));
        assert(absolute(-INTMAX_MAX)==INTMAX_MAX);
        long (*absolute_l)(long)=symbol(handle,"labs");
        assert(absolute_l && (void*)absolute_l==kl_shim_lookup("labs"));
        assert(absolute_l(0)==0 && absolute_l(-17)==17);
        assert(absolute_l(-0x100000001L)==0x100000001L);
        assert(absolute_l(-LONG_MAX)==LONG_MAX);
        long long (*absolute_ll)(long long)=symbol(handle,"llabs");
        assert(absolute_ll && (void*)absolute_ll==kl_shim_lookup("llabs"));
        assert(absolute_ll(0)==0 && absolute_ll(-17)==17);
        assert(absolute_ll(0x100000001LL)==0x100000001LL);
        assert(absolute_ll(-0x100000001LL)==0x100000001LL);
        assert(absolute_ll(-LLONG_MAX)==LLONG_MAX);
        assert(symbol(handle, "getifaddrs") == kl_shim_lookup("getifaddrs"));
        assert(symbol(handle, "freeifaddrs") == kl_shim_lookup("freeifaddrs"));
        error();
        assert(!symbol(handle, "UnityPluginLoad"));
        const char *message = error(); assert(message && strstr(message, "UnityPluginLoad"));
        assert(!error());
        assert(!close_library(handle));
    }
    assert(symbol(NULL, "UnityPluginLoad") == plugin);
    assert(symbol(open_library(NULL, 2), "UnityPluginLoad") == plugin);
    assert(symbol(img, "UnityPluginLoad") == plugin);
    assert(!unlink(path));
    puts("core dlsym: libc exports isolated from guest plugins; global and explicit image lookup preserved");
}
