#ifndef KL_META_ACCOUNT_H
#define KL_META_ACCOUNT_H

// Install the native Meta Graph provider only when this app has an imported
// game session in its own Keychain. Missing sessions preserve existing runtime
// behavior. The import is consumed before a guest starts.
void kl_meta_account_install(void);

#endif
