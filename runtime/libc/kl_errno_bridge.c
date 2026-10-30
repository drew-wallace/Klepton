// BigSoup's bundled usrsctp assigns errno=socreate(...) and immediately reads
// it back. The legacy read-only errno translator discards that assignment.
// Preserve a Linux slot and commit guest writes at each native-call boundary.
// Scope this bridge to BigSoup: other guests keep their existing import ABI.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "klepton.h"
#include "kl_env.h"
#include "kl_errno_bridge.h"

static _Thread_local struct { int value, exposed, initialized; } state;
static void commit(void) {
    if(state.initialized && state.value!=state.exposed)
        errno=kl_errno_from_linux(state.value);
    else
        state.value=kl_errno_to_linux(errno);
    state.exposed=state.value;
    state.initialized=1;
}
static int *guest_errno(void) { commit();return &state.value; }
// Invoked before the native callee, not afterwards: a guest's errno=0 must
// reach Darwin even if the next failed call sets the SAME error as before.
void kl_errno_bridge_enter(void) { commit(); }
extern void kl_errno_bridge_entry(void);
void *kl_errno_bridge_import(const char *image,const char *name,void *target) {
    const char *base=image?strrchr(image,'/'):NULL;
    base=base?base+1:image;
    // The ELF loader names .so, while signed device/simulator bundles use
    // libBigSoup.framework/libBigSoup (or the host's .dylib translation).
    int bigsoup=base&&(!strcmp(base,"libBigSoup.so")||!strcmp(base,"libBigSoup.dylib")||!strcmp(base,"libBigSoup"));
    if(!bigsoup||!target||!name||
       !kl_env_on("KL_BIGSOUP_ERRNO",1))return target;
    if(!strcmp(name,"__errno"))return (void*)guest_errno;
    char key[256];
    if(snprintf(key,sizeof key,"BigSoup errno/%s",name)>=(int)sizeof key)return target;
    void *entry=kl_trace_stub(key,target,(void*)kl_errno_bridge_entry);
    // Exhaustion must not silently drop the write barrier for one import.
    return entry?entry:kl_named_stub(key,kl_shim_lookup("__klepton_unresolved"));
}
