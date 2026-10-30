#ifndef KL_JNI_SURFACE_H
#define KL_JNI_SURFACE_H
// Recognize a decoder Surface and borrow its window (NULL after release).
// Returns zero for an unrelated Surface. ANativeWindow_fromSurface acquires it.
int klj_surface_window(void *surface, void **window);
#endif
