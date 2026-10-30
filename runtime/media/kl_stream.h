#ifndef KL_STREAM_H
#define KL_STREAM_H
#include <stdint.h>
// Pollable AVPlayer adapter. No JNI or GL work runs on its main-thread timer.
enum { KL_STREAM_READY=1, KL_STREAM_PLAYING=2, KL_STREAM_BUFFERING=4,
       KL_STREAM_ENDED=8, KL_STREAM_ERROR=16 };
typedef struct {
    uint64_t serial;
    int width,height,ready,playing,error_code,has_frame;
    int ended,buffering,seeking,has_audio,has_video,audio_tracks;
    float source_fps;
    float seek_start,seek_end,buffered_fraction;
    uint64_t frame_count;
    int64_t position_ms,duration_ms;
    unsigned events;
    char error[192]; // Fixed categories only; never a URL or native error text.
} kl_stream_state;
void *kl_stream_new(void);
void kl_stream_close(void *stream);
void kl_stream_open(void *stream,const char *url,const char *mime);
void kl_stream_reject(void *stream,int code,const char *message);
void kl_stream_play(void *stream,int playing);
void kl_stream_stop(void *stream);
void kl_stream_seek(void *stream,int64_t milliseconds);
void kl_stream_volume(void *stream,float volume);
void kl_stream_loop(void *stream,int looping);
void kl_stream_rate(void *stream,float rate);
void kl_stream_limits(void *stream,int bitrate,int height);
void kl_stream_snapshot(void *stream,kl_stream_state *state,int consume_events);
// Retained CVPixelBufferRef, or NULL; caller releases it using CoreVideo.
void *kl_stream_copy_frame(void *stream,uint64_t *serial);
#endif
