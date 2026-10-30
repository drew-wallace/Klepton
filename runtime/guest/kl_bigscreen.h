#ifndef KL_BIGSCREEN_H
#define KL_BIGSCREEN_H

// Called only after the SDK has consumed a failed logged-in-user completion.
void kl_bigscreen_account_unavailable(void);
// Unity's attached guest thread, between frames. Never enters managed code
// unless the supported Bigscreen build has received that account failure.
void kl_bigscreen_tick(unsigned frame);
const char *kl_bigscreen_unity_arguments(void);

#endif
