#ifndef KL_ERRNO_BRIDGE_H
#define KL_ERRNO_BRIDGE_H
// Called only for defined host FUNCTION imports, never guest exports or data.
void *kl_errno_bridge_import(const char *image,const char *name,void *target);
#endif
