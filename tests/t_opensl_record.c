// Exercise the real OpenSL recording vtables with synthetic input. No hardware
// microphone is opened and no user audio is collected.
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include "kl_audio.h"
static int enabled;
static atomic_int callbacks,reads,opens,closes;
static int fixture_enabled(void){return enabled;}
static int fixture_open(unsigned rate,unsigned channels){atomic_fetch_add(&opens,1);return 0;}
static void fixture_close(void){atomic_fetch_add(&closes,1);}
static unsigned fixture_rate(void){return 48000;}
static unsigned fixture_channels(void){return 1;}
static int fixture_read(int16_t *out,int frames){
    int n=frames>121?121:frames;
    for(int i=0;i<n;i++)out[i]=1234;
    atomic_fetch_add(&reads,n);return n;
}
#define kl_audio_mic_enabled fixture_enabled
#define kl_audio_mic_open fixture_open
#define kl_audio_mic_close fixture_close
#define kl_audio_mic_rate fixture_rate
#define kl_audio_mic_channels fixture_channels
#define kl_audio_mic_read_i16 fixture_read
#define kl_opensl_sym fixture_sym
#define kl_opensl_report fixture_report
#define kl_opensl_claims fixture_claims
#define kl_opensl_dlopen fixture_dlopen
#define kl_opensl_is_handle fixture_is_handle
#include "../runtime/media/kl_opensl.c"
static int16_t pcm[160];
static void complete(SLBufferQueueItf queue,void *context){
    assert(context==pcm);
    for(unsigned i=0;i<160;i++)assert(pcm[i]==1234);
    atomic_fetch_add(&callbacks,1);
}
static void wait_for(int count){
    for(int n=0;n<1000&&atomic_load(&callbacks)<count;n++)usleep(1000);
    assert(atomic_load(&callbacks)==count);
}
int main(void){
    SLObjectItf engine=NULL,object=NULL;SLEngineItf api=NULL;
    int (*create)(SLObjectItf*,SLuint32,void*,SLuint32,const SLInterfaceID*,const SLboolean*)=fixture_sym("slCreateEngine");
    assert(!create(&engine,0,NULL,0,NULL,NULL));
    assert(!(*engine)->GetInterface(engine,SL_IID_ENGINE,&api));
    assert((*api)->CreateAudioRecorder(api,&object,NULL,NULL,0,NULL,NULL)==12);
    assert(!atomic_load(&opens));enabled=1;
    SLDataFormat_PCM format={2,1,16000000,16,16,4,2};
    SLDataSink sink={NULL,&format};
    assert(!(*api)->CreateAudioRecorder(api,&object,NULL,&sink,0,NULL,NULL));
    SLRecordItf record=NULL;SLBufferQueueItf queue=NULL;
    assert(!(*object)->GetInterface(object,SL_IID_RECORD,&record));
    assert(!(*object)->GetInterface(object,SL_IID_ANDROIDSIMPLEBUFFERQUEUE,&queue));
    assert(!(*object)->Realize(object,0));
    SLuint32 state=0,ms=0;
    assert(!(*record)->GetRecordState(record,&state)&&state==1);
    assert((*record)->SetRecordState(record,0)==2);
    assert((*record)->SetRecordState(record,4)==2);
    assert(!(*queue)->RegisterCallback(queue,complete,pcm));
    assert(!(*record)->SetRecordState(record,2)); // Khronos/AOSP PAUSED.
    assert(!(*queue)->Enqueue(queue,pcm,sizeof pcm));usleep(20000);
    assert(!atomic_load(&callbacks)&&!atomic_load(&reads));
    assert(!(*record)->SetRecordState(record,3)); // Khronos/AOSP RECORDING.
    wait_for(1);
    assert(!(*record)->GetPosition(record,&ms)&&ms==10);
    assert(!(*record)->SetRecordState(record,2));
    assert(!(*queue)->Enqueue(queue,pcm,sizeof pcm));usleep(20000);
    assert(atomic_load(&callbacks)==1);
    assert(!(*record)->GetPosition(record,&ms)&&ms==10);
    assert(!(*record)->SetRecordState(record,3));wait_for(2);
    assert(!(*record)->GetPosition(record,&ms)&&ms==20);
    assert(!(*record)->SetRecordState(record,1));
    assert(!(*record)->GetRecordState(record,&state)&&state==1);
    (*object)->Destroy(object);assert(atomic_load(&closes)==1);
    puts("OpenSL recorder: standard states, opt-in gate, partial PCM reads, resampling, callbacks, pause/resume and recording position");
}
