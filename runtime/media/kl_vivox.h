#ifndef KL_VIVOX_H
#define KL_VIVOX_H
void *kl_vivox_dlopen(const char *path);
int kl_vivox_is_handle(const void *handle);
void *kl_vivox_sym(const char *name);
int kl_vivox_native_active(void);
#endif
