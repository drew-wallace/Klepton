#ifndef KL_UNITY_COMPAT_H
#define KL_UNITY_COMPAT_H

// Optional, audited interposition of Unity plugin configuration exports.
// NULL means the original export must be used unchanged.
void *kl_unity_oculus_interpose(const char *name, void *real);
void *kl_unity_vivox_interpose(const char *name, void *real);

#endif
