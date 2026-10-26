// Exercise the real FMOD Java adapter with a controlled native PCM sink.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kl_jni_int.h"
#include "kl_audio.h"
static int fail_open, opens, plays, writes;
static unsigned opened_rate, opened_channels;
static size_t written_bytes;
static int16_t written_pcm[8];
static const void *producer;
klj_object *klj_as_object(void *p) { return p; }
klj_array *klj_arr(void *p) { return p; }
void *klj_own(void *p, void (*destroy)(void *)) { ((klj_object *)p)->destroy = destroy; return p; }
void *kl_jni_activity(void) { return NULL; }
int kl_audio_open(unsigned rate, unsigned channels, unsigned bits) {
    assert(bits == 16); opens++; opened_rate = rate; opened_channels = channels; return fail_open ? -1 : 0;
}
void kl_audio_play(void) { plays++; }
size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes) {
    assert(src && bytes <= sizeof written_pcm); producer = src; writes++; written_bytes = bytes;
    memcpy(written_pcm, pcm, bytes); return bytes;
}
#include "../../../runtime/jni/kl_jni_fmod.c"
int main(void) {
    klj_object device = {0}, relaunched = {0};
    int16_t pcm[] = {-32768, -1234, 1234, 32767};
    klj_array samples = {.len = 4, .kind = 'S', .data = pcm};
    klj_val format[] = {{.j=2}, {.j=48000}, {.j=512}, {.j=4}};
    klj_val buffer[] = {{.l=&samples}, {.j=4}};
    klf_device_write(NULL, &device, buffer, 2); assert(!writes);
    fail_open=1; assert(!klf_device_init(NULL, &device, format, 4).j);
    klf_device_write(NULL, &device, buffer, 2); assert(!writes && !plays);
    fail_open=0; assert(klf_device_init(NULL, &device, format, 4).j);
    assert(opened_rate==48000 && opened_channels==2 && plays==1);
    klf_device_write(NULL, &device, buffer, 2);
    assert(writes==1 && written_bytes==sizeof pcm && !memcmp(pcm,written_pcm,sizeof pcm));
    buffer[1].j=5; klf_device_write(NULL,&device,buffer,2); assert(writes==1);
    buffer[1].j=(uint64_t)-1; klf_device_write(NULL,&device,buffer,2); assert(writes==1);
    buffer[1].j=4; samples.kind='B'; klf_device_write(NULL,&device,buffer,2); assert(writes==1);
    samples.kind='S'; klf_device_close(NULL,&device,NULL,0);
    klf_device_write(NULL,&device,buffer,2); assert(writes==1);
    const void *first_producer=producer;
    assert(klf_device_init(NULL,&relaunched,format,4).j);
    klf_device_write(NULL,&relaunched,buffer,2);
    assert(writes==2 && producer==first_producer); // no new mixer slot after restart
    device.destroy(device.data); relaunched.destroy(relaunched.data);
    puts("FMOD PCM format, signed samples, bounds, failed initialization, close and restart passed");
}
