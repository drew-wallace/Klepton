#ifndef KL_JNI_VUPLEX_H
#define KL_JNI_VUPLEX_H
int klj_vuplex_is_view(void *object);
void klj_vuplex_drain_events(void);
void klj_vuplex_render(void *object);
void klj_vuplex_pointer(void *object, const char *type, int x, int y, int button);
void klj_vuplex_scroll(void *object, int dx, int dy, int x, int y);
int klj_vuplex_is_fullscreen(void *object);
void klj_vuplex_exit_fullscreen(void *object);
#endif
