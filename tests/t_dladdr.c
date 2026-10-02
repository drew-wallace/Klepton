#include "klepton.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { const char *file; void *base; const char *symbol; void *address; } dl_info;
typedef struct {
    uint64_t base; const char *name; const void *phdr; uint16_t count;
    unsigned long long adds, subs; size_t tls_id; void *tls_data;
} phdr_info;
static void put16(unsigned char *p, uint16_t n) { memcpy(p, &n, 2); }
static void put32(unsigned char *p, uint32_t n) { memcpy(p, &n, 4); }
static void put64(unsigned char *p, uint64_t n) { memcpy(p, &n, 8); }
static int seen;
static int visit(void *raw, size_t size, void *data) {
    phdr_info *p = raw;
    assert(size == sizeof *p);
    assert(!strcmp(p->name, data));
    assert(p->phdr && p->count == 2);
    seen++;
    return 0;
}
int main(void) {
    // A data-only ELF with PT_LOAD and PT_DYNAMIC needs no game assets or compiler cross
    // toolchain. Loading by a relative name must still yield its real filename.
    unsigned char elf[4096] = {0};
    memcpy(elf, "\177ELF\2\1\1", 7);
    put16(elf+16, 3); put16(elf+18, 183); put32(elf+20, 1);
    put64(elf+32, 64); put16(elf+52, 64); put16(elf+54, 56); put16(elf+56, 2);
    put32(elf+64, 1); put32(elf+68, 4);
    put64(elf+96, sizeof elf); put64(elf+104, sizeof elf); put64(elf+112, 4096);
    put32(elf+120, 2); put32(elf+124, 4);
    put64(elf+128, 256); put64(elf+136, 256);
    put64(elf+152, 48); put64(elf+160, 48); put64(elf+168, 8);
    put64(elf+256, 6); put64(elf+264, 512);  // DT_SYMTAB: null symbol
    put64(elf+272, 5); put64(elf+280, 536);  // DT_STRTAB: empty string
    char path[] = "build/dladdr-fixture.XXXXXX", resolved[PATH_MAX];
    int fd = mkstemp(path); assert(fd >= 0);
    assert(write(fd, elf, sizeof elf) == sizeof elf && !close(fd));
    assert(realpath(path, resolved));
    kl_image *img = kl_load(path);
    if (!img) fprintf(stderr, "fixture load failed: %s\n", kl_error());
    assert(img);
    kl_register_image("fixture.so", img);
    assert(!strcmp(kl_image_path(img), resolved));
    int (*lookup)(const void *, dl_info *) = (void *)kl_shim_lookup("dladdr");
    int (*iterate)(int (*)(void *, size_t, void *), void *) = (void *)kl_shim_lookup("dl_iterate_phdr");
    assert(lookup && iterate);
    dl_info out = {0};
    assert(lookup((unsigned char *)kl_base(img)+128, &out) == 1);
    assert(!strcmp(out.file, resolved) && out.base == kl_base(img));
    assert(!out.symbol && !out.address);
    assert(!lookup((void *)(uintptr_t)1, &out));
    assert(iterate(visit, resolved) == 0 && seen == 1);
    // The returned filename remains usable after later lookups.
    assert(!strcmp(out.file, resolved));
    assert(!unlink(path));
    puts("dladdr: resolved image directory, load base, filename lifetime and phdr path passed");
}
