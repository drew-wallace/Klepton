#ifndef KL_WEBVIEW_H
#define KL_WEBVIEW_H
#include <stdint.h>
#include <stddef.h>
// Commands are asynchronous; serialize access to each handle and close once,
// after its last command or frame copy. Events arrive on Apple's main thread; callers
// must marshal them to the guest thread before entering guest code.
typedef void (*kl_webview_event)(uint64_t view, const char *event,
                                 const char *value, uint64_t request);
void *kl_webview_create(uint64_t id, int width, int height, const char *agent,
                        int autoplay, kl_webview_event event);
void kl_webview_close(void *view);
void kl_webview_load(void *view, const char *content, const char *base, int html);
void kl_webview_eval(void *view, const char *script, uint64_t request);
void kl_webview_resize(void *view, int width, int height);
void kl_webview_command(void *view, const char *command, int value);
int kl_webview_is_fullscreen(void *view);
void kl_webview_user_agent(void *view, const char *agent);
// Select the native WebKit mobile/desktop browsing mode, clearing a custom UA.
// Takes effect on the next top-level navigation.
void kl_webview_user_agent_mode(void *view, int mobile);
void kl_webview_pointer(void *view, const char *type, int x, int y, int button);
void kl_webview_scroll(void *view, int dx, int dy, int x, int y);
void kl_webview_key(void *view, const char *key);
// Copies the latest BGRA frame. Caller frees pixels. Serial is only advanced
// after a successful copy, so texture upload can be retried after GL failure.
void *kl_webview_copy_frame(void *view, uint64_t *serial, int *width, int *height);
#endif
