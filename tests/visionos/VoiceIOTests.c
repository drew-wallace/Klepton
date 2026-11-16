// Exercise the visionOS duplex path with fake AudioUnits, without recording or
// opening any device. Assert that the ACTUAL speaker PCM is the AEC reference.
#include <assert.h>
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    OSType subtype;
    int started, input, output;
    AURenderCallbackStruct speaker, mic;
    AudioStreamBasicDescription capture, playback;
} fake_unit;
static unsigned native_rate = 48000, native_channels = 1;
static int live_units, running_units, peak_units, fail_voice, fail_start;
static int edge_samples;
static AudioComponent fake_find(AudioComponent prev, const AudioComponentDescription *d) {
    return (AudioComponent)(uintptr_t)d->componentSubType;
}
static OSStatus fake_new(AudioComponent c, AudioComponentInstance *out) {
    fake_unit *u = calloc(1, sizeof *u); u->subtype = (OSType)(uintptr_t)c; u->output = 1;
    *out = (AudioUnit)u; live_units++;
    if (live_units > peak_units) peak_units = live_units;
    return noErr;
}
static OSStatus fake_set(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope,
                          AudioUnitElement bus, const void *data, UInt32 bytes) {
    fake_unit *u = (fake_unit *)unit;
    if (property == kAudioOutputUnitProperty_EnableIO) {
        if (scope == kAudioUnitScope_Input && bus == 1) u->input = *(const UInt32 *)data;
        if (scope == kAudioUnitScope_Output && bus == 0) u->output = *(const UInt32 *)data;
    } else if (property == kAudioUnitProperty_StreamFormat) {
        // PHASE's AVAudioEngine graph cannot connect integer capture PCM.
        // Refuse it here rather than allowing the permissive fake to hide the
        // physical headset's asynchronous -10868 exception during initialize.
        if (scope == kAudioUnitScope_Output && bus == 1 &&
            !(((const AudioStreamBasicDescription *)data)->mFormatFlags & kAudioFormatFlagIsFloat))
            return kAudioUnitErr_FormatNotSupported;
        if (scope == kAudioUnitScope_Input && bus == 0) u->playback = *(const AudioStreamBasicDescription *)data;
        if (scope == kAudioUnitScope_Output && bus == 1) u->capture = *(const AudioStreamBasicDescription *)data;
    } else if (property == kAudioUnitProperty_SetRenderCallback) u->speaker = *(const AURenderCallbackStruct *)data;
    else if (property == kAudioOutputUnitProperty_SetInputCallback) u->mic = *(const AURenderCallbackStruct *)data;
    return noErr;
}
static OSStatus fake_get(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope,
                          AudioUnitElement bus, void *data, UInt32 *bytes) {
    if (property != kAudioUnitProperty_StreamFormat) return -1;
    fake_unit *u = (fake_unit *)unit;
    AudioStreamBasicDescription f = {.mSampleRate = native_rate, .mChannelsPerFrame = native_channels};
    if (scope == kAudioUnitScope_Output && bus == 1) f = u->capture;
    if (scope == kAudioUnitScope_Input && bus == 0) f = u->playback;
    memcpy(data, &f, sizeof f); *bytes = sizeof f; return noErr;
}
static OSStatus fake_initialize(AudioUnit unit) {
    fake_unit *u = (fake_unit *)unit;
    if (fail_voice && u->subtype == kAudioUnitSubType_VoiceProcessingIO) return -1;
    assert(u->speaker.inputProc && u->output);
    if (u->input) {
        assert(u->mic.inputProc && u->capture.mSampleRate && u->playback.mSampleRate);
        UInt32 required=kAudioFormatFlagIsFloat|kAudioFormatFlagIsNonInterleaved;
        assert((u->capture.mFormatFlags&required)==required&&u->capture.mBitsPerChannel==32&&u->capture.mBytesPerFrame==4);
        assert((u->playback.mFormatFlags&required)==required&&u->playback.mBytesPerFrame==4);
    }
    return noErr;
}
static OSStatus fake_start(AudioUnit unit) {
    fake_unit *u = (fake_unit *)unit;
    if (fail_start && u->input) return -1;
    if (!u->started) { running_units++; u->started = 1; }
    assert(running_units == 1); return noErr;
}
static OSStatus fake_stop(AudioUnit unit) {
    fake_unit *u = (fake_unit *)unit;
    if (u->started) { running_units--; u->started = 0; }
    return noErr;
}
static OSStatus fake_uninitialize(AudioUnit unit) { assert(!((fake_unit *)unit)->started); return noErr; }
static OSStatus fake_dispose(AudioUnit unit) {
    assert(!((fake_unit *)unit)->started); free(unit); live_units--; return noErr;
}
static OSStatus fake_render(AudioUnit unit, AudioUnitRenderActionFlags *flags,
                             const AudioTimeStamp *timestamp, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    fake_unit *u = (fake_unit *)unit;
    assert(u->started && u->input && u->speaker.inputProc && bus == 1);
    assert(io->mNumberBuffers==u->capture.mChannelsPerFrame);
    const float edge[]={NAN,INFINITY,-INFINITY,2,-2,.5f,-.5f,0};
    for(unsigned ch=0;ch<io->mNumberBuffers;ch++){
        assert(io->mBuffers[ch].mNumberChannels==1&&io->mBuffers[ch].mDataByteSize==frames*sizeof(float));
        float *pcm=io->mBuffers[ch].mData;
        for(unsigned i=0;i<frames;i++)pcm[i]=edge_samples?edge[i%8]:(ch?-4321.f:4321.f)/32768.f;
    }
    return noErr;
}
#define KL_AUDIO_DUPLEX_TEST 1
#define kAudioUnitSubType_RemoteIO ((OSType)0x72696f63)
#define AudioComponentFindNext fake_find
#define AudioComponentInstanceNew fake_new
#define AudioUnitSetProperty fake_set
#define AudioUnitGetProperty fake_get
#define AudioUnitInitialize fake_initialize
#define AudioUnitUninitialize fake_uninitialize
#define AudioOutputUnitStart fake_start
#define AudioOutputUnitStop fake_stop
#define AudioComponentInstanceDispose fake_dispose
#define AudioUnitRender fake_render
#include "../../runtime/media/kl_audio.c"

static void check_reference(void) {
    static const char game;
    int16_t pcm[32]; for (int i = 0; i < 32; i++) pcm[i] = i & 1 ? -8192 : 8192;
    assert(kl_audio_write_pcm(&game, pcm, sizeof pcm, 48000, 2) == sizeof pcm);
    fake_unit *u = (fake_unit *)g_unit;
    float left[16],right[16];
    struct {UInt32 count;AudioBuffer buffers[2];} list={2,{{1,sizeof left,left},{1,sizeof right,right}}};
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp timestamp = {0};
    assert(!u->speaker.inputProc(u, &flags, &timestamp, 0, 16, (AudioBufferList*)&list));
    for (int i = 0; i < 16; i++) assert(left[i]==.25f&&right[i]==-.25f);
    assert(!u->mic.inputProc(u, &flags, &timestamp, 1, 16, NULL));
    int16_t captured[32]; assert(kl_audio_mic_read_i16(captured, 16) == 16);
    for (unsigned i = 0; i < 16 * g_mic_ch; i++) assert(captured[i] == (i%g_mic_ch?-4321:4321));
    // Every planar output buffer must be silenced on underrun, not only left.
    for(int i=0;i<16;i++)left[i]=right[i]=123;
    assert(!u->speaker.inputProc(u,&flags,&timestamp,0,16,(AudioBufferList*)&list));
    for(int i=0;i<16;i++)assert(left[i]==0&&right[i]==0);
}
int main(void) {
    setenv("KL_AUDIO_WATCHDOG", "0", 1);
    assert(kl_audio_mic_open(16000, 1) == -1 && live_units == 0);
    kl_audio_set_session_managed(1);
    kl_audio_mic_set_enabled(1); assert(live_units == 0); // opt-in remains lazy
    assert(!kl_audio_open(48000, 2, 16)); kl_audio_play();
    AudioUnit first = g_unit;
    assert(!kl_audio_open(24000, 1, 16) && g_unit == first); // preserve running game clock
    assert(!kl_audio_mic_open(16000, 1));
    assert(g_duplex && g_unit == g_cap_unit && live_units == 1 && running_units == 1);
    assert(((fake_unit *)g_unit)->subtype == kAudioUnitSubType_VoiceProcessingIO);
    assert(kl_audio_mic_voice_processing());
    check_reference();
    edge_samples=1;
    AudioUnitRenderActionFlags flags=0;AudioTimeStamp timestamp={0};
    fake_unit *voice=(fake_unit*)g_unit;
    assert(!voice->mic.inputProc(voice,&flags,&timestamp,1,8,NULL));
    int16_t edges[8],expected[]={0,0,0,32767,-32768,16384,-16384,0};
    assert(kl_audio_mic_read_i16(edges,8)==8&&!memcmp(edges,expected,sizeof expected));
    edge_samples=0;
    // After a reader stall, trim backlog on the consumer side rather than
    // letting voice remain one second behind or racing the producer's cursor.
    for (int i = 0; i < 10000; i++) g_mic_ring[i] = (int16_t)i;
    atomic_store(&g_mic_r, 0); atomic_store(&g_mic_w, 10000);
    int16_t fresh[16]; assert(kl_audio_mic_read_i16(fresh, 16) == 16);
    assert(fresh[0] == 5200 && atomic_load(&g_mic_r) == 5216);
    // Route changes keep the client clock and channel count already advertised
    // to an Android reader, even when the hardware format changes.
    native_rate = 24000; native_channels = 2;
    assert(!kl_audio_restart());
    assert(g_mic_rate == 48000 && g_mic_ch == 1 && g_out_rate == 48000);
    check_reference();
    kl_audio_interrupted(1); assert(!running_units && g_mic_host_stopped);
    assert(kl_audio_restart() == -1);
    kl_audio_interrupted(0); assert(running_units == 1 && !g_mic_host_stopped);
    kl_audio_suspend(); assert(!running_units);
    kl_audio_resume(); assert(running_units == 1 && g_duplex);
    kl_audio_mic_set_enabled(0);
    assert(!g_duplex && !g_cap_unit && g_open && live_units == 1 && running_units == 1);
    assert(!kl_audio_mic_voice_processing());
    assert(g_out_rate == 24000 && g_cap == 24001); // standalone route resize
    kl_audio_close(); assert(!live_units && !running_units);
    // Capture before playback, closing playback during capture, raw fallback,
    // and a failed capture start must each leave at most ONE running device.
    native_rate = 48000; native_channels = 1;
    kl_audio_mic_set_enabled(1);
    assert(!kl_audio_mic_open(16000, 1)); assert(!g_open && g_duplex);
    assert(!kl_audio_open(48000, 2, 16)); kl_audio_play(); check_reference();
    kl_audio_close(); assert(g_duplex && g_mic_open && running_units == 1);
    kl_audio_mic_close(); assert(!live_units);
    fail_voice = 1;
    assert(!kl_audio_open(48000, 2, 16));
    assert(!kl_audio_mic_open(16000, 1));
    assert(g_duplex && ((fake_unit *)g_unit)->subtype == kAudioUnitSubType_RemoteIO);
    assert(!kl_audio_mic_voice_processing());
    kl_audio_mic_close();
    fail_voice = 0; fail_start = 1;
    assert(kl_audio_mic_open(16000, 1) == -1);
    assert(!g_mic_open && !g_duplex && live_units == 1 && running_units == 1);
    kl_audio_close(); assert(!live_units && peak_units == 1);
    fail_start=0;native_channels=2;
    assert(!kl_audio_open(48000,2,16));assert(!kl_audio_mic_open(48000,2));
    assert(g_mic_ch==2);check_reference();
    kl_audio_mic_close();kl_audio_close();assert(!live_units);
    puts("Voice I/O: actual speaker echo reference, stable clocks, lazy opt-in, route/recovery and fallback passed");
    return 0;
}
