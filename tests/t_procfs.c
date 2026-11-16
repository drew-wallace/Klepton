#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "klepton.h"

extern const char *kl_guest_path(const char *, char *, size_t);

int main(void) {
    char mapped[1024];
    struct stat st;
    const char *root=kl_guest_path("/proc",mapped,sizeof mapped);
    assert(strcmp(root,"/proc") && !stat(root,&st) && S_ISDIR(st.st_mode));
    const char *memory=kl_guest_path("/proc/meminfo",mapped,sizeof mapped);
    assert(!stat(memory,&st) && S_ISREG(st.st_mode));
    FILE *f=fopen(memory,"r");assert(f);
    char line[256];unsigned long long total=0,available=0;
    while(fgets(line,sizeof line,f)) {
        sscanf(line,"MemTotal: %llu kB",&total);
        sscanf(line,"MemAvailable: %llu kB",&available);
    }
    fclose(f);assert(total>0 && available<=total);
    const char *self=kl_guest_path("/proc/self",mapped,sizeof mapped);
    assert(!stat(self,&st) && S_ISDIR(st.st_mode));
    // Similar-looking paths and nonexistent children retain normal failures.
    assert(!strcmp(kl_guest_path("/process",mapped,sizeof mapped),"/process"));
    assert(!strcmp(kl_guest_path("/proc/klepton-missing-fixture",mapped,sizeof mapped),
                   "/proc/klepton-missing-fixture"));
    puts("procfs: parent and self directories exist alongside readable memory data");
}
