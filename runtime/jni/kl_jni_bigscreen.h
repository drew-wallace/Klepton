#ifndef KL_JNI_BIGSCREEN_H
#define KL_JNI_BIGSCREEN_H
// Deliver stream state callbacks only on Unity's attached guest thread.
void klj_bigscreen_drain_events(void);
// Copy produced frames only on a thread with the actual ANGLE context current.
void klj_bigscreen_render(void);
#endif
