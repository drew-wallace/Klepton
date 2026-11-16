// See kl_audio.h. The producer/consumer split, in one paragraph:
//
//   feeder thread (kl_opensl.c)          CoreAudio render thread
//   ----------------------------          -----------------------
//   pop guest buffer                      memcpy out of the ring
//   int16 -> float32, N ch -> out_ch      advance the read cursor
//   resample guest rate -> device rate    (nothing else, ever)
//   push into the ring, block if full
//   call the guest's buffer-queue cb
//
// The ring is single-producer/single-consumer with monotonic frame counters, so
// neither side needs a lock and the render callback is genuinely lock-free. The
// producer blocks by polling, not by waiting on a condvar, because signalling a
// condvar from the render callback is exactly the kind of thing that is fine
// until the day it is not.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <TargetConditionals.h>
#include <AudioToolbox/AudioToolbox.h>

#include "klepton.h"
#include "kl_audio.h"

#define KL_AUDIO_MAX_CH 2
#if !TARGET_OS_OSX || defined(KL_AUDIO_DUPLEX_TEST)
#define KL_AUDIO_DUPLEX 1
#else
#define KL_AUDIO_DUPLEX 0
#endif

// ---- state ----
static AudioUnit    g_unit;
static int          g_open;           // a unit exists and is initialised
static int          g_running;        // ...and AudioOutputUnitStart succeeded
static int          g_playing;        // the guest wants sound
static _Atomic int  g_interrupted;    // the OS took the stream
static _Atomic int  g_session_managed; // Swift owns interruption recovery
static _Atomic int  g_host_suspended;  // no audio I/O while the display is away
static uint64_t     g_interrupt_ns;   // ...when, so a lost `.ended` is escapable
static unsigned     g_interrupt_max_ms = 1500;
// The recovery machinery lives at the bottom of the file, beside the producer
// it used to be part of; kl_audio_open and kl_audio_interrupted are above it.
static unsigned     g_wd_streak;      // consecutive restarts, for the backoff
static void         watchdog_start(void);
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // unit lifecycle only

static unsigned g_in_rate, g_in_ch, g_in_bits;   // the guest's format
static unsigned g_out_ch = 2;
static double   g_out_rate;                      // the device's, measured
static double   g_session_rate;                  // what Swift told us, if anything

// The ring. Indices are monotonic frame counts; the modulo happens at use, so
// there is no full/empty ambiguity and no wrap flag to get wrong.
static float           *g_ring;
static size_t           g_cap;          // frames
static size_t           g_target;       // frames of fill the producer aims for
static _Atomic size_t   g_w, g_r;

// Multiple producers, MIXED. Steam Link's VR client opens TWO output streams at
// once (a float one and an int16 one, both AAudio, both 48 kHz stereo after the
// feeder's conversion) and each has its own feeder thread calling in here. The
// ring is single-producer by construction, so two of them appending raced the
// write cursor and interleaved their audio — heard as the wild, unfixable
// stutter that no buffer touched. So writes now MIX (add) into the ring at a
// per-source position kept in step with the reader, and the render callback
// ZEROES each slot as it consumes it so the next pass sums from silence. One
// source is the old behaviour exactly (add into a zeroed slot == a copy). A
// small lock serialises the producers (never the reader): they run on feeder
// threads that can afford to block, the reader cannot.
#define KL_AU_MAX_SRC 8
static const void      *g_src_key[KL_AU_MAX_SRC];
static uint64_t         g_src_pos[KL_AU_MAX_SRC];   // next write frame, per source
static int              g_src_n;
static int              g_src_live[KL_AU_MAX_SRC];
static unsigned         g_src_rate[KL_AU_MAX_SRC], g_src_ch[KL_AU_MAX_SRC];
static double           g_src_phase[KL_AU_MAX_SRC];
static float            g_src_prev[KL_AU_MAX_SRC][KL_AUDIO_MAX_CH];
static unsigned long    g_output_epoch;
static pthread_mutex_t  g_wlock = PTHREAD_MUTEX_INITIALIZER;

// NULL identifies the OpenSL/default producer and must be registered like any
// other key. Otherwise Unity's OpenSL output and FMOD's Java output share one
// cursor, concatenating silent Unity blocks with game audio instead of mixing.
// Caller holds g_wlock. A full table refuses a source instead of reusing a live clock.
static int src_slot(const void *key) {
    for (int i = 0; i < g_src_n; i++)
        if (g_src_live[i] && g_src_key[i] == key) return i;
    for (int i = 0; i < KL_AU_MAX_SRC; i++) {
        if (i < g_src_n && g_src_live[i]) continue;
        if (i >= g_src_n) g_src_n = i + 1;
        g_src_live[i] = 1; g_src_key[i] = key;
        g_src_pos[i] = atomic_load(&g_r);
        g_src_rate[i] = g_src_ch[i] = 0;
        g_src_phase[i] = 0;
        memset(g_src_prev[i], 0, sizeof g_src_prev[i]);
        return i;
    }
    return -1;
}

// A driver-set floor for the fill target, in ms (0 = none). See kl_audio_open.
static unsigned         g_latency_override_ms;
void kl_audio_set_latency_ms(unsigned ms) { g_latency_override_ms = ms; }

// Resampler state, producer-side only.
static float *g_scratch;                 // converted+resampled staging
static size_t g_scratch_frames;

// Counters, for the report and for the watchdog.
static _Atomic uint64_t g_last_render_ns;
static _Atomic uint64_t g_frames_played, g_frames_silent;
static _Atomic uint32_t g_underruns;
static unsigned long    g_writes, g_short_writes, g_restarts;
static int              g_trace;
// The peak sample the guest ever handed us. A buffer count says the mixer ran;
// this says the mixer produced audio — the same distinction `make check` draws
// for the opus roundtrip, and the one that would have caught "3,238 buffers
// consumed, nothing heard" years earlier.
static int              g_peak;
static unsigned long    g_silent_buffers;

// ---- microphone capture state ----
//
// Capture stays lazy behind the microphone toggle. On visionOS g_cap_unit
// aliases g_unit while recording: both directions use one VoiceProcessingIO,
// with the actual speaker PCM as its echo reference. The macOS test host keeps
// an independent raw HAL capture unit.
//
// The input callback writes native-rate interleaved int16 into an SPSC ring.
// Guest reader threads perform filtered resampling and channel mapping. Neither
// device callback allocates, locks, logs, or calls guest code.
static AudioUnit        g_cap_unit;
static _Atomic int      g_mic_enabled;    // the KleptonMic toggle
static _Atomic int      g_mic_voice_processing;
static int              g_mic_open;       // guest capture is open (also during recovery)
static int              g_mic_host_stopped; // retained capture paused by the host
static unsigned         g_mic_rate;       // stable capture client rate advertised to readers
static unsigned         g_mic_dev_rate;   // the hardware's own capture rate, for the log / fallback
static unsigned         g_mic_ch = 1;     // capture channels (1 or 2)
// Output and input share one lifecycle lock, so route changes cannot dispose
// a duplex unit underneath a reader, a microphone toggle, or output recovery.
static int              g_duplex;
static int              cap_unit_create(unsigned want_rate, unsigned want_ch);

static int16_t         *g_mic_ring;
static size_t           g_mic_cap;        // frames
static _Atomic size_t   g_mic_w, g_mic_r;

// The input callback pulls the hardware's frames into this fixed scratch and
// then copies them into the ring — a real-time callback may not allocate, so the
// AudioBufferList and its backing buffers are built once at open. The PHASE
// graph behind visionOS duplex I/O requires canonical float32 planes; convert
// those to the existing interleaved int16 reader contract before publication.
static struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[KL_AUDIO_MAX_CH]; } g_cap_abl;
static int16_t         *g_cap_scratch;
static float           *g_cap_float_scratch;
static size_t           g_cap_scratch_frames;
static void cap_free_scratch(void) {
    free(g_cap_scratch);g_cap_scratch=NULL;
    free(g_cap_float_scratch);g_cap_float_scratch=NULL;
    g_cap_scratch_frames=0;
}

// Counters for the report. Producer-side (the callback) writes the capture/drop
// pair with relaxed atomics — no log or malloc on that thread — and g_mic_peak
// is the capture twin of g_peak: "did the microphone actually hear anything".
static _Atomic uint64_t g_mic_frames_cap, g_mic_frames_drop, g_mic_frames_read;
static _Atomic int      g_mic_peak;
static _Atomic int      g_mic_render_error;
static const char      *g_mic_unit_name = "?";

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- KL_AUDIO_DUMP — the capture, and the reason it is worth having ----
//
// "Sound" is the one output of this runtime that cannot be checked by reading a
// log. A buffer count, a peak level and an underrun count together say the
// plumbing is right; they cannot distinguish music from a resampler running at
// the wrong ratio, channels swapped, or a ring being read one frame behind
// itself — all of which are audible and none of which are visible. So the
// device path can also tee to a WAV, exactly the frames the render callback
// will hand the hardware, and that file is playable evidence. The GL twin is
// KL_GLFB_OUT, and it earned its keep for the same reason.
//
// What it records is what the producer accepted, contiguously — so a pause is
// elided rather than rendered as silence, and the file is shorter than the run.
// That is deliberate: the question this answers is "is what we send correct",
// and silence between tracks is not evidence either way.
static FILE  *g_dump;
static size_t g_dump_frames;

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void dump_open(void) {
    const char *path = kl_env_str("KL_AUDIO_DUMP", NULL);
    if (!path || !*path) return;
    g_dump = fopen(path, "wb");
    if (!g_dump) { fprintf(stderr, "  [au] cannot write %s\n", path); return; }
    g_dump_frames = 0;
    // Sizes are patched in dump_close(); a run that dies without closing leaves
    // a header claiming zero, which most players read as an empty file. Better
    // than a header claiming a length the data does not have.
    fwrite("RIFF", 1, 4, g_dump); put32(g_dump, 0); fwrite("WAVEfmt ", 1, 8, g_dump);
    put32(g_dump, 16); put16(g_dump, 1); put16(g_dump, (uint16_t)g_out_ch);
    put32(g_dump, (uint32_t)g_out_rate);
    put32(g_dump, (uint32_t)(g_out_rate * g_out_ch * 2));
    put16(g_dump, (uint16_t)(g_out_ch * 2)); put16(g_dump, 16);
    fwrite("data", 1, 4, g_dump); put32(g_dump, 0);
    fprintf(stderr, "  [au] capturing the device stream to %s\n", path);
}

static void dump_write(const float *frames, size_t n) {
    if (!g_dump || !n) return;
    for (size_t i = 0; i < n * g_out_ch; i++) {
        float v = frames[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        put16(g_dump, (uint16_t)(int16_t)lrintf(v * 32767.0f));
    }
    g_dump_frames += n;
}

static void dump_close(void) {
    if (!g_dump) return;
    uint32_t bytes = (uint32_t)(g_dump_frames * g_out_ch * 2);
    fseek(g_dump, 4, SEEK_SET);  put32(g_dump, 36 + bytes);
    fseek(g_dump, 40, SEEK_SET); put32(g_dump, bytes);
    fclose(g_dump);
    g_dump = NULL;
    fprintf(stderr, "  [au] capture closed: %.2f s\n",
            g_out_rate > 0 ? (double)g_dump_frames / g_out_rate : 0.0);
}

static void nap_ms(unsigned ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// ---- the render callback ----
//
// Everything here is a load, a memcpy and a store. No locks, no allocation, no
// guest code — the guest's callback runs on the feeder thread and always has.
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                          AudioBufferList *io) {
    (void)ref; (void)ts; (void)bus;
    atomic_store_explicit(&g_last_render_ns, now_ns(), memory_order_relaxed);

    if (!io || io->mNumberBuffers < 1) return noErr;
    float *dst = (float *)io->mBuffers[0].mData;
    size_t want = nframes;
    int planar=io->mNumberBuffers>1;
    int valid=planar?io->mNumberBuffers==g_out_ch:io->mBuffers[0].mNumberChannels==g_out_ch;
    for(UInt32 ch=0;ch<io->mNumberBuffers;ch++)
        valid=valid&&io->mBuffers[ch].mData&&
            (!planar||io->mBuffers[ch].mNumberChannels==1)&&
            io->mBuffers[ch].mDataByteSize>=nframes*sizeof(float)*(planar?1:g_out_ch);
    if (!valid || !g_ring) {
        for(UInt32 ch=0;ch<io->mNumberBuffers;ch++)
            if(io->mBuffers[ch].mData)memset(io->mBuffers[ch].mData,0,io->mBuffers[ch].mDataByteSize);
        if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    size_t r = atomic_load_explicit(&g_r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&g_w, memory_order_acquire);
    size_t have = w - r;
    size_t take = have < want ? have : want;

    for (size_t done = 0; done < take; ) {
        size_t off = (r + done) % g_cap;
        size_t run = g_cap - off;
        if (run > take - done) run = take - done;
        if(planar){
            for(unsigned ch=0;ch<g_out_ch;ch++){
                float *plane=io->mBuffers[ch].mData;
                for(size_t i=0;i<run;i++)plane[done+i]=g_ring[(off+i)*g_out_ch+ch];
            }
        }else memcpy(dst + done * g_out_ch, g_ring + off * g_out_ch,
                      run * g_out_ch * sizeof(float));
        // Zero what we just took: the ring is a mix accumulator now, and a slot
        // reused on the next wrap must start from silence so producers can add
        // into it. Harmless for the single-source case (it re-zeros a slot the
        // one producer will fully overwrite anyway).
        memset(g_ring + off * g_out_ch, 0, run * g_out_ch * sizeof(float));
        done += run;
    }
    if (take < want) {
        if(planar){
            for(unsigned ch=0;ch<g_out_ch;ch++)
                memset((float*)io->mBuffers[ch].mData+take,0,(want-take)*sizeof(float));
        }else memset(dst + take * g_out_ch, 0, (want - take) * g_out_ch * sizeof(float));
        atomic_fetch_add_explicit(&g_frames_silent, want - take, memory_order_relaxed);
        // An underrun while the guest thinks it is playing is a real dropout;
        // one while it is stopped is just an idle device, and counting those
        // would bury the signal under thousands of them.
        if (take == 0 && !g_playing) {
            if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        } else {
            atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&g_frames_played, take, memory_order_relaxed);
    atomic_store_explicit(&g_r, r + take, memory_order_release);
    return noErr;
}

// ---- the unit ----
static const char *fourcc(OSStatus s, char buf[8]) {
    uint32_t v = (uint32_t)s;
    unsigned char c[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 8), (unsigned char)v};
    for (int i = 0; i < 4; i++)
        if (c[i] < 0x20 || c[i] > 0x7e) { snprintf(buf, 8, "%d", (int)s); return buf; }
    snprintf(buf, 8, "'%c%c%c%c'", c[0], c[1], c[2], c[3]);
    return buf;
}

#define CHECK(call, what) do { \
    OSStatus _s = (call); \
    if (_s != noErr) { char _b[8]; \
        fprintf(stderr, "  [au] %s failed: %s\n", (what), fourcc(_s, _b)); \
        goto fail; } } while (0)

// Ask the unit what the hardware is actually doing. On RemoteIO the output
// scope of element 0 IS the hardware side, and it will refuse to initialise if
// our client rate disagrees with it — there is no sample-rate converter in
// there. Hence a resampler on our side rather than a hopeful SetProperty.
static double measure_device_rate(AudioUnit u) {
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 0, &hw, &sz) == noErr &&
        hw.mSampleRate > 8000.0 && hw.mSampleRate < 400000.0)
        return hw.mSampleRate;
    return 0.0;
}

static int unit_create(void) {
#if KL_AUDIO_DUPLEX
    if (g_mic_open) {
        if (cap_unit_create(0, 0) != 0) return -1;
        g_unit = g_cap_unit;
        g_duplex = 1;
        return 0;
    }
#endif
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_OSX
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#else
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { fprintf(stderr, "  [au] no output AudioComponent\n"); return -1; }
    CHECK(AudioComponentInstanceNew(comp, &g_unit), "AudioComponentInstanceNew");

    double hw = measure_device_rate(g_unit);
    if (hw <= 0.0) hw = g_session_rate > 0.0 ? g_session_rate : (double)g_in_rate;
    g_out_rate = hw;

    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = g_out_rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = g_out_ch;
    fmt.mFramesPerPacket  = 1;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = (UInt32)(sizeof(float) * g_out_ch);
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    CHECK(AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &fmt, sizeof fmt),
          "SetProperty(StreamFormat)");

    AURenderCallbackStruct cb = {render_cb, NULL};
    CHECK(AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof cb),
          "SetProperty(SetRenderCallback)");

    CHECK(AudioUnitInitialize(g_unit), "AudioUnitInitialize");
    return 0;
fail:
    if (g_unit) { AudioComponentInstanceDispose(g_unit); g_unit = NULL; }
    return -1;
}

static void unit_destroy(void) {
    if (!g_unit) return;
    if (g_running) AudioOutputUnitStop(g_unit);
    g_running = 0;
    AudioUnitUninitialize(g_unit);
    AudioComponentInstanceDispose(g_unit);
    if (g_duplex) {
        atomic_store(&g_mic_voice_processing, 0);
        g_cap_unit = NULL;
        g_duplex = 0;
        g_mic_host_stopped = 1;
    }
    g_unit = NULL;
}

static int unit_start(void) {
    if (g_interrupted || g_host_suspended) return -1;
    if (!g_unit || g_running) return 0;
    OSStatus s = AudioOutputUnitStart(g_unit);
    if (s != noErr) {
        char b[8];
        fprintf(stderr, "  [au] AudioOutputUnitStart failed: %s\n", fourcc(s, b));
        return -1;
    }
    g_running = 1;
    if (g_duplex) g_mic_host_stopped = 0;
    atomic_store_explicit(&g_last_render_ns, now_ns(), memory_order_relaxed);
    return 0;
}

// Called with lifecycle + writer locks held and device callbacks stopped.
// A waiting producer carries the previous epoch and abandons that old block.
static void reset_output_clock(void) {
    g_output_epoch++;
    atomic_store(&g_w, 0); atomic_store(&g_r, 0);
    if (g_ring) memset(g_ring, 0, g_cap * g_out_ch * sizeof(float));
    memset(g_src_pos, 0, sizeof g_src_pos);
    memset(g_src_phase, 0, sizeof g_src_phase);
    memset(g_src_prev, 0, sizeof g_src_prev);
}

static int resize_output_ring(void) {
    if (!g_open || g_cap == (size_t)g_out_rate + 1) return 0;
    size_t capacity = (size_t)g_out_rate + 1;
    float *ring = calloc(capacity * g_out_ch, sizeof(float));
    if (!ring) return -1;
    if (g_cap > 1) g_target = (size_t)((double)g_target * g_out_rate / (double)(g_cap - 1));
    free(g_ring); g_ring = ring; g_cap = capacity;
    return 0;
}

// ---- open / close ----
int kl_audio_open(unsigned rate, unsigned channels, unsigned bits) {
    if (!kl_env_on("KL_AUDIO", 1)) {
        static int said;
        if (!said++) fprintf(stderr, "  [au] KL_AUDIO=0 — no device, the feeder keeps its own clock\n");
        return -1;
    }
    // 16-bit is what FMOD produces and the only width the converter below
    // implements. Anything else is refused loudly rather than played as noise.
    if (bits != 16 || channels < 1 || channels > 8 || rate < 8000 || rate > 192000) {
        fprintf(stderr, "  [au] refusing guest format %u Hz / %u ch / %u bit "
                        "(only 16-bit is implemented)\n", rate, channels, bits);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    // One hardware sink. Every producer supplies its own PCM format on write;
    // opening another stream must not reinterpret or reset existing streams.
    if (g_open) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    pthread_mutex_lock(&g_wlock);
    // Capture may have opened first. Stop its callback before allocating the
    // output ring, then rebuild the same duplex unit rather than adding a sink.
    if (g_unit) unit_destroy();
    g_open = 0;

    g_in_rate = rate; g_in_ch = channels; g_in_bits = bits;
    g_out_ch = 2;
    g_trace = kl_env_on("KL_AUDIO_TRACE", 0);

    if (unit_create() != 0) {
        pthread_mutex_unlock(&g_wlock); pthread_mutex_unlock(&g_lock); return -1;
    }

    // The ring holds a full second so a scheduling hiccup on either side cannot
    // wrap it; the *fill target* is what sets latency, and it is much smaller.
    // Keeping those two separate is what lets the target grow to fit an
    // unexpectedly large guest buffer without reallocating anything.
    // 80 ms is fine for a LOCAL mixer (FMOD), which never starves its own feed.
    // A streamed guest (Steam Link) delivers audio over the network in bursts,
    // and a stall shorter than the fill target is silently absorbed while a
    // longer one is a dropout — so a driver that knows its audio arrives over a
    // jittery link raises this floor (kl_audio_set_latency_ms). KL_AUDIO_LATENCY_MS
    // still overrides both.
    unsigned lat_ms = kl_env_int("KL_AUDIO_LATENCY_MS",
                                 g_latency_override_ms ? g_latency_override_ms : 80);
    if (lat_ms < 10) lat_ms = 10;
    if (lat_ms > 500) lat_ms = 500;

    free(g_ring);
    g_cap = (size_t)g_out_rate + 1;
    g_ring = calloc(g_cap * g_out_ch, sizeof(float));
    free(g_scratch);
    g_scratch_frames = 0;
    g_scratch = NULL;
    if (!g_ring) {
        unit_destroy(); pthread_mutex_unlock(&g_wlock); pthread_mutex_unlock(&g_lock); return -1;
    }

    g_target = (size_t)(g_out_rate * lat_ms / 1000.0);
    reset_output_clock();
    g_open = 1;
    if (!g_session_managed) g_interrupted = 0;
    // Once per process, not once per open: FMOD destroys and re-creates the
    // player, and a capture that restarted with it would silently keep only the
    // last few seconds of a long run.
    static int dump_tried;
    if (!dump_tried++) dump_open();

    g_interrupt_ns = now_ns();
    // Shorter than the original 3 s, but NOT so short it fires before the audio
    // session has finished reactivating. The common interruption here is the
    // shell window closing as the immersive space takes over, whose ".ended" this
    // OS routinely drops; the session comes back on its own ~1–1.5 s later. Fire
    // BEFORE that and the restart lands on a still-dead session, plays silence,
    // and the heartbeat restarts again — several short jumps instead of one gap
    // (600 ms did exactly that). 1.5 s lands just past the reactivation, so the
    // single escape restart takes and the handoff recovers in one step.
    // KL_AUDIO_INTERRUPT_MAX_MS overrides — lower it if your recovery is faster.
    g_interrupt_max_ms = kl_env_int("KL_AUDIO_INTERRUPT_MAX_MS", 1500);
    // Start the unit immediately, even with nothing queued. A stopped unit is
    // one the OS is free to tear down harder than a running silent one, and the
    // render callback's timestamp is the watchdog's only heartbeat.
    unit_start();
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
    watchdog_start();

    fprintf(stderr, "  [au] CoreAudio out: guest %u Hz/%u ch -> device %.0f Hz/%u ch, "
                    "%u ms target%s\n",
            rate, channels, g_out_rate, g_out_ch, lat_ms,
            fabs(g_out_rate - (double)rate) > 1.0 ? " (resampling)" : "");
    return 0;
}

void kl_audio_close(void) {
    pthread_mutex_lock(&g_lock);
    pthread_mutex_lock(&g_wlock);
    g_playing = 0;
    unit_destroy();
    // NOT dump_close(): FMOD destroys and re-creates the player during startup,
    // so closing here ended the capture 0.12 s into a 12.8 s run. The capture is
    // per-process and the report closes it.
    g_open = 0;
    free(g_ring);   g_ring = NULL;   g_cap = 0;
    free(g_scratch); g_scratch = NULL; g_scratch_frames = 0;
    atomic_store(&g_w, 0); atomic_store(&g_r, 0);
    reset_output_clock();
#if KL_AUDIO_DUPLEX
    if (g_mic_open && unit_create() == 0) unit_start();
#endif
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
}

int kl_audio_active(void) { return g_open && g_running && !g_host_suspended; }

void kl_audio_play(void) {
    if (!g_open) return;
    g_playing = 1;
    pthread_mutex_lock(&g_lock);
    unit_start();
    pthread_mutex_unlock(&g_lock);
}

void kl_audio_pause(void) { g_playing = 0; }

void kl_audio_flush(void) {
    if (!g_open) return;
    // The read cursor belongs to the render thread, so it cannot simply be
    // assigned from here: a callback that had already loaded `r` would store
    // r+take afterwards and rewind the ring over stale audio. AudioOutputUnitStop
    // blocks until the callback has returned, which is the only cheap way to get
    // exclusive ownership of that cursor. Flushes happen on a play-state change,
    // so the cost is not on any hot path.
    pthread_mutex_lock(&g_lock);
    pthread_mutex_lock(&g_wlock);
    int was = g_running;
    if (was && g_unit) { AudioOutputUnitStop(g_unit); g_running = 0; }
    reset_output_clock();
    memset(g_src_phase, 0, sizeof g_src_phase);
    memset(g_src_prev, 0, sizeof g_src_prev);
    if (was) unit_start();
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
}

// ---- the OS taking the stream away ----
void kl_audio_interrupted(int began) {
    fprintf(stderr, "  [au] session interruption %s\n", began ? "began" : "ended");
    g_interrupt_ns = now_ns();
    if (began) {
        g_interrupted = 1;
        pthread_mutex_lock(&g_lock);
        if (g_unit && g_running) {
            AudioOutputUnitStop(g_unit); g_running = 0;
            if (g_duplex) g_mic_host_stopped = 1;
        }
        pthread_mutex_unlock(&g_lock);
    } else {
        g_interrupted = 0;
        g_wd_streak = 0;
        kl_audio_restart();
    }
}

// Rebuild the unit. Used by both the notification path and the watchdog, and it
// has to be a full rebuild rather than a stop/start: a route change can leave
// the unit initialised against a sample rate the hardware no longer has, and
// that unit will start successfully and never call back.
int kl_audio_restart(void) {
    if (!g_open && !g_mic_open) return -1;
    pthread_mutex_lock(&g_lock);
    if (g_interrupted || g_host_suspended) { pthread_mutex_unlock(&g_lock); return -1; }
    pthread_mutex_lock(&g_wlock);
    unit_destroy();
    int rc = unit_create();
    if (rc == 0) rc = resize_output_ring();
    if (rc == 0) {
        reset_output_clock();
        atomic_store(&g_mic_r, atomic_load(&g_mic_w));
        rc = unit_start();
    }
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
    g_restarts++;
    fprintf(stderr, "  [au] restart %s (device now %.0f Hz)\n",
            rc == 0 ? "ok" : "FAILED", g_out_rate);
    return rc;
}

void kl_audio_session_ready(double sample_rate) {
    double previous_rate = g_session_rate;
    g_session_rate = sample_rate;
    fprintf(stderr, "  [au] audio session ready, %.0f Hz\n", sample_rate);
    // If a unit already exists it was built against a guess; rebuild it against
    // the real rate. Before the session is active the hardware format reads as
    // a default, and a unit initialised against the wrong rate is the failure
    // that presents as "everything succeeded and nothing is audible".
    double comparison = g_duplex ? previous_rate : g_out_rate;
    if ((g_open || g_mic_open) && fabs(sample_rate - comparison) > 1.0) kl_audio_restart();
}

void kl_audio_set_session_managed(int managed) {
    g_session_managed = managed != 0;
}

// ---- the producer ----
//
// int16 interleaved at the guest rate -> float32 interleaved at the device
// rate, with linear interpolation across the chunk boundary. The virtual input
// stream is v[-1] = the last frame of the previous chunk, v[0..n-1] = this one,
// so each source carries fractional position between calls and a rate ratio of
// exactly 1.0 degenerates to a straight copy.
static size_t convert(int slot, const int16_t *src, size_t n, float **out) {
    double ratio = (double)g_src_rate[slot] / g_out_rate;
    size_t max_out = (size_t)((double)(n + 2) / ratio) + 2;
    if (max_out > g_scratch_frames) {
        float *p = realloc(g_scratch, max_out * g_out_ch * sizeof(float));
        if (!p) return 0;
        g_scratch = p;
        g_scratch_frames = max_out;
    }
    float *dst = g_scratch;
    unsigned ich = g_src_ch[slot], och = g_out_ch;
    double pos = g_src_phase[slot];
    size_t produced = 0;

    // The frame fetch folds channel mapping in: mono fans out, anything wider
    // than stereo keeps the first two channels. Unity's FMOD has only ever
    // asked for stereo, so the other paths are correctness, not tuning.
    // Clamped at BOTH ends. The upper clamp is not defensive tidiness: the loop
    // condition admits pos == n-1 (so that a 1.0 ratio degenerates to an exact
    // copy rather than dropping the last frame), and that reads v[n]. The
    // interpolation weight there is 0, so the value is discarded — but the read
    // is not, and it is two bytes past the guest's buffer.
    #define FETCH(idx, ch) \
        ((idx) < 0 ? g_src_prev[slot][ch] \
                   : (float)src[(size_t)((idx) < (long)n ? (idx) : (long)n - 1) * ich \
                                + ((ch) < ich ? (ch) : 0)] * (1.0f / 32768.0f))

    while (pos < (double)n - 1.0 + 1e-9 && produced < max_out) {
        long i = (long)floor(pos);
        float f = (float)(pos - (double)i);
        for (unsigned c = 0; c < och; c++) {
            float a = FETCH(i, c), b = FETCH(i + 1, c);
            dst[produced * och + c] = a + (b - a) * f;
        }
        produced++;
        pos += ratio;
    }
    #undef FETCH

    if (n > 0) {
        for (unsigned c = 0; c < och; c++)
            g_src_prev[slot][c] = (float)src[(n - 1) * ich + (c < ich ? c : 0)] * (1.0f / 32768.0f);
    }
    g_src_phase[slot] = pos - (double)n;
    *out = dst;
    return produced;
}

// The watchdog. A render callback that has stopped arriving while we believe we
// are running is the visionOS failure mode this file was written expecting:
// the OS reclaims the stream on a route change or a scene transition and the
// unit stays nominally started.
//
// **The heartbeat is the only honest signal.** visionOS silently stops calling
// the render callback when the immersive space is dismissed — a Digital Crown
// press, the app's window being closed — with no error, no interruption
// notification and a unit that still reports itself running. ALVR carries the
// same check for the same reason and against the same OS bug; there it lives in
// the receive loop, here it has a thread of its own (below).
//
// Consecutive restarts back off. A restart that cannot work — the app really is
// off screen, or the session really is inactive — would otherwise print twice a
// second for as long as that lasts, which buries the one line that says what
// actually happened.
static void watchdog(void) {
    if ((!g_open && !g_mic_open) || g_interrupted || g_host_suspended) return;
    uint64_t last = atomic_load_explicit(&g_last_render_ns, memory_order_relaxed);
    uint64_t idle = now_ns() - last;
    uint64_t limit = 500ull * 1000000ull;
    if (g_wd_streak > 3) limit = 3000ull * 1000000ull;    // backed off
    // The render callback's heartbeat is the ONLY honest signal that the device
    // is alive. g_running is our own bookkeeping, and on this OS AudioOutputUnitStop
    // does not reliably stop the callback firing — so a stale g_running=0 while the
    // callback keeps arriving must NOT tear the unit down. It did: with the callback
    // healthy (idle ~0.01 s) but g_running read as 0, this rebuilt the unit on every
    // pass — hundreds a run, ~2 ms apart from the producer's full-ring spin — and
    // each rebuild is a real gap. THAT was the "stutter", and no buffer could hide
    // it. So the decision is the heartbeat alone: the callback is alive, leave it be.
    if (idle <= limit) { g_wd_streak = 0; return; }
    // The callback has genuinely stopped. Only worth rebuilding if the guest still
    // wants sound; a stopped guest is just an idle device, not a fault to fight.
    if (!g_playing && !g_mic_open) { g_wd_streak = 0; return; }
    if (g_wd_streak < 4 || (g_wd_streak % 20) == 0)
        fprintf(stderr, "  [au] no render callback for %.2f s — restarting%s\n",
                (double)idle / 1e9,
                g_wd_streak >= 4 ? " (still; further attempts are quiet)" : "");
    g_wd_streak++;
    kl_audio_restart();
}

// ...and the thread that runs it, because the producer cannot.
//
// `watchdog()` used to be reachable from exactly one place: the spin inside
// kl_audio_write that waits for the ring to drain. That covers the case the
// file was written for — the render callback dies while the guest keeps
// producing — and misses every other one. Two that matter here:
//
//   * The guest stops producing too. Steam Link's audio is a pull path
//     (kl_aaudio.c calls the guest's data callback), and a guest whose own
//     clock has stalled writes nothing, so nothing checks.
//   * An interruption that never ends. `kl_audio_interrupted(1)` latches
//     g_interrupted, and the write loop breaks out of the spin *before* the
//     watchdog on that flag — so a `.began` with no matching `.ended` (which is
//     a documented hazard on this OS family, and is what a scene transition
//     looks like when the notification is dropped) is silence for the rest of
//     the run with nothing left running to notice.
//
static void watchdog_tick(void) {
    if (!g_open || g_host_suspended) return;
    if (g_interrupted && g_playing) {
        // An interruption can be a live Discord call, not a missing ended
        // notification. Rebuilding after 1.5 seconds fought over the route.
        // The session host recovers on ended/foreground events; retain the
        // timeout fallback only for hosts without AVAudioSession management.
        if (g_session_managed) return;
        uint64_t since = now_ns() - g_interrupt_ns;
        if (since > (uint64_t)g_interrupt_max_ms * 1000000ull) {
            fprintf(stderr, "  [au] interrupted for %.1f s with no resume — "
                            "assuming the notification was lost\n",
                    (double)since / 1e9);
            g_interrupted = 0;
            g_interrupt_ns = now_ns();
            kl_audio_restart();
            // If this restart lands on a session that is STILL reactivating
            // it plays silence and the normal watchdog below would rebuild it
            // again every 500 ms — the jump storm. Pre-load the backoff so the
            // next attempt is seconds away, not milliseconds: one more try,
            // spaced, by which point the session is certainly back.
            g_wd_streak = 4;
        }
        return;
    }
    watchdog();
}

// A detached thread at 4 Hz costs nothing and depends on neither side.
static void *watchdog_thread(void *unused) {
    (void)unused;
    pthread_setname_np("klepton-audio-watchdog");
    for (;;) {
        nap_ms(250);
        watchdog_tick();
    }
    return NULL;
}

static void watchdog_start(void) {
    static int started;
    if (started) return;
    if (!kl_env_on("KL_AUDIO_WATCHDOG", 1)) {
        fprintf(stderr, "  [au] KL_AUDIO_WATCHDOG=0 — no independent watchdog\n");
        started = 1;
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_thread, NULL) != 0) {
        fprintf(stderr, "  [au] could not start the audio watchdog thread\n");
        return;
    }
    pthread_detach(t);
    started = 1;
}

// Stop native I/O before the host releases its audio session. This hold is
// independent of OS interruptions and never expires in the watchdog.
void kl_audio_suspend(void) {
    g_host_suspended = 1;
    pthread_mutex_lock(&g_lock);
    if (g_unit && g_running) {
            AudioOutputUnitStop(g_unit); g_running = 0;
            if (g_duplex) g_mic_host_stopped = 1;
        }
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_lock);
    if (g_cap_unit && g_mic_open && !g_mic_host_stopped && !g_duplex) {
        AudioOutputUnitStop(g_cap_unit);
        g_mic_host_stopped = 1;
        atomic_store(&g_mic_r, atomic_load(&g_mic_w));
    }
    pthread_mutex_unlock(&g_lock);
}

// The host has reactivated its session. A window closing during immersion can
// call this without interrupting output: preserve its ring and healthy callback.
// Real suspension/interruption, a missing unit, or a dead callback still needs
// a rebuild. The watchdog catches a callback that stops after this check.
void kl_audio_resume(void) {
    int was_suspended = atomic_exchange(&g_host_suspended, 0);
    int was = atomic_exchange(&g_interrupted, 0);
    g_interrupt_ns = now_ns();
    g_wd_streak = 0;
    if (g_open || (KL_AUDIO_DUPLEX && g_mic_open)) {
        pthread_mutex_lock(&g_lock);
        uint64_t last = atomic_load_explicit(&g_last_render_ns, memory_order_relaxed);
        int healthy = g_unit && last && now_ns() - last <= 500ull * 1000000ull;
        pthread_mutex_unlock(&g_lock);
        if (was_suspended || was || !healthy) {
            fprintf(stderr, "  [au] resume%s\n", was ? " (was interrupted)" : "");
            kl_audio_restart();
        }
    }
    pthread_mutex_lock(&g_lock);
    if (g_cap_unit && g_mic_open && g_mic_host_stopped && g_mic_enabled && !g_duplex) {
        OSStatus status = AudioOutputUnitStart(g_cap_unit);
        if (status == noErr) g_mic_host_stopped = 0;
        else fprintf(stderr, "  [au] capture resume failed: %d\n", (int)status);
    }
    pthread_mutex_unlock(&g_lock);
}

void kl_audio_forget_src(const void *src) {
    pthread_mutex_lock(&g_wlock);
    for (int i = 0; i < g_src_n; i++) {
        if (g_src_live[i] && g_src_key[i] == src) {
            g_src_live[i] = 0;
            g_src_phase[i] = 0;
        }
    }
    int live = 0;
    for (int i = 0; i < g_src_n; i++) live += g_src_live[i];
    g_playing = live != 0;
    pthread_mutex_unlock(&g_wlock);
}

size_t kl_audio_write(const void *pcm, size_t bytes) {
    return kl_audio_write_src(NULL, pcm, bytes);
}

size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes) {
    return kl_audio_write_pcm(src, pcm, bytes, g_in_rate, g_in_ch);
}

size_t kl_audio_write_pcm(const void *src, const void *pcm, size_t bytes,
                          unsigned rate, unsigned channels) {
    if (!g_open || g_host_suspended || !pcm || !bytes ||
        rate < 8000 || rate > 192000 || channels < 1 || channels > 8) return 0;
    unsigned in_frame = channels * 2;              // 16-bit, checked at open
    size_t n = bytes / in_frame;
    if (!n) return 0;

    int peak = 0;
    const int16_t *s16 = (const int16_t *)pcm;
    for (size_t i = 0; i < n * channels; i++) {
        int v = s16[i] < 0 ? -s16[i] : s16[i];
        if (v > peak) peak = v;
    }
    // Conversion uses shared scratch state, but each writer keeps its own
    // converted block while waiting. Holding the writer lock during that wait
    // starves a second source even when it can mix into already queued frames.
    pthread_mutex_lock(&g_wlock);
    if (!g_open || !g_ring) { pthread_mutex_unlock(&g_wlock); return 0; }
    unsigned long epoch = g_output_epoch;
    double output_rate = g_out_rate;
    int slot = src_slot(src);
    if (slot < 0) { pthread_mutex_unlock(&g_wlock); return 0; }
    if (g_src_rate[slot] != rate || g_src_ch[slot] != channels) {
        g_src_rate[slot] = rate; g_src_ch[slot] = channels;
        g_src_phase[slot] = 0;
        memset(g_src_prev[slot], 0, sizeof g_src_prev[slot]);
    }
    float *scratch = NULL;
    size_t frames = convert(slot, (const int16_t *)pcm, n, &scratch);
    // Tiny downsampled chunks can advance the input clock without emitting a
    // device frame. They were consumed; reporting a short write adds extra sleep.
    if (!frames) { pthread_mutex_unlock(&g_wlock); return scratch ? bytes : 0; }
    float *conv = malloc(frames * g_out_ch * sizeof(float));
    if (!conv) { pthread_mutex_unlock(&g_wlock); return 0; }
    memcpy(conv, scratch, frames * g_out_ch * sizeof(float));
    unsigned long write_number = ++g_writes;
    if (peak > g_peak) g_peak = peak;
    if (!peak) g_silent_buffers++;

    // The target must hold at least two blocks for this producer.
    if (g_target < frames * 2) {
        g_target = frames * 2;
        if (g_target > g_cap - 1) g_target = g_cap - 1;
    }
    pthread_mutex_unlock(&g_wlock);

    uint64_t deadline = now_ns() + 2000ull * 1000000ull;
    size_t written = 0;
    while (written < frames) {
        pthread_mutex_lock(&g_wlock);
        if (epoch != g_output_epoch || !g_open || !g_ring) {
            pthread_mutex_unlock(&g_wlock); break;
        }
        int mixing = g_src_n > 1;
        uint64_t r = atomic_load_explicit(&g_r, memory_order_acquire);
        // One source uses the shared write cursor. Concurrent sources use
        // separate cursors on that same timeline, including a source that
        // registers while another writer is waiting for room.
        uint64_t here = mixing ? g_src_pos[slot]
                               : atomic_load_explicit(&g_w, memory_order_relaxed);
        if (mixing && (here < r || here > r + g_cap)) {
            size_t left = frames - written;
            here = r + (g_target > left ? g_target - left : 0);
        }
        if (here < r) here = r;
        uint64_t ahead = here - r;
        size_t room = ahead < g_target ? (size_t)(g_target - ahead) : 0;
        if (ahead < g_cap && g_cap - (size_t)ahead < room) room = g_cap - (size_t)ahead;
        size_t take = frames - written;
        if (take > room) take = room;

        if (take) {
            if (!mixing) {
                for (size_t done = 0; done < take; ) {
                    size_t off = (size_t)((here + done) % g_cap);
                    size_t run = g_cap - off;
                    if (run > take - done) run = take - done;
                    memcpy(g_ring + off * g_out_ch,
                           conv + (written + done) * g_out_ch,
                           run * g_out_ch * sizeof(float));
                    done += run;
                }
            } else {
                for (size_t k = 0; k < take; k++) {
                    size_t off = (size_t)((here + k) % g_cap);
                    const float *sp = conv + (written + k) * g_out_ch;
                    float *dp = g_ring + off * g_out_ch;
                    for (unsigned c = 0; c < g_out_ch; c++) dp[c] += sp[c];
                }
            }
            written += take;
            g_src_pos[slot] = here + take;
            uint64_t curw = atomic_load_explicit(&g_w, memory_order_relaxed);
            if (here + take > curw)
                atomic_store_explicit(&g_w, here + take, memory_order_release);
        }
        pthread_mutex_unlock(&g_wlock);
        if (take) continue;

        // Waiting for this source must leave other producers free to fill
        // their own cursors. The real-time render callback never takes a lock.
        if (!g_playing || g_interrupted || g_host_suspended) break;
        watchdog();
        if (now_ns() > deadline) {
            pthread_mutex_lock(&g_wlock);
            g_short_writes++;
            pthread_mutex_unlock(&g_wlock);
            break;
        }
        nap_ms(2);
    }

    pthread_mutex_lock(&g_wlock);
    dump_write(conv, written);

    if (g_trace && (write_number % 200) == 1) {
        size_t fill = atomic_load(&g_w) - atomic_load(&g_r);
        fprintf(stderr, "  [au] write #%lu src%d: %zu in -> %zu out, fill %zu/%zu "
                        "(%.1f ms), %d src(s), underruns %u\n",
                write_number, slot, n, frames, fill, g_target,
                1000.0 * (double)fill / g_out_rate, g_src_n,
                atomic_load(&g_underruns));
    }

    // Periodic peak verdict, WITHOUT the trace flag: "can I hear anything?" is a
    // question a normal run has to be able to answer. A peak of 0 across hundreds
    // of buffers means the guest's mixer is running but producing silence (banks
    // not loaded, no events) — a different bug from an output that never opened.
    if ((write_number % 1000) == 1) {
        fprintf(stderr, "  [au] guest PCM so far: peak %d/32767, %lu of %lu "
                        "buffers all-zero — %s\n",
                g_peak, g_silent_buffers, g_writes,
                g_peak ? "producing audio" : "ONLY SILENCE so far");
    }
    pthread_mutex_unlock(&g_wlock);
    free(conv);
    return written == frames ? bytes : (size_t)((double)written * rate / output_rate) * in_frame;
}

unsigned kl_audio_underruns(void) { return atomic_load(&g_underruns); }

void kl_audio_report(FILE *f) {
    if (!g_writes && !g_open) return;
    dump_close();   // the report is the last thing a clean run does
    uint64_t played = atomic_load(&g_frames_played);
    fprintf(f, "  CoreAudio: %.2f s played, %u underruns (%.3f s of silence filled), "
               "%lu writes (%lu short), %lu restarts\n",
            g_out_rate > 0 ? (double)played / g_out_rate : 0.0,
            atomic_load(&g_underruns),
            g_out_rate > 0 ? (double)atomic_load(&g_frames_silent) / g_out_rate : 0.0,
            g_writes, g_short_writes, g_restarts);
    fprintf(f, "  guest PCM: peak %d/32767 (%.1f dBFS), %lu of %lu buffers all-zero — %s\n",
            g_peak, g_peak ? 20.0 * log10((double)g_peak / 32767.0) : -1e9,
            g_silent_buffers, g_writes,
            g_peak ? "the guest produced audio" : "THE GUEST PRODUCED ONLY SILENCE");
    // The capture twin of the line above: it ran only if a guest opened an input
    // stream with the mic toggle on, so it is silent (no line) on the common
    // playback-only run.
    uint64_t cap = atomic_load(&g_mic_frames_cap);
    if (g_mic_enabled || cap) {
        int mpk = atomic_load(&g_mic_peak);
        fprintf(f, "  microphone: %.2f s captured, %.2f s read by guest, %.2f s dropped "
                   "(overrun/stale), peak %d/32767 — %s\n",
                g_mic_rate ? (double)cap / g_mic_rate : 0.0,
                g_mic_rate ? (double)atomic_load(&g_mic_frames_read) / g_mic_rate : 0.0,
                g_mic_rate ? (double)atomic_load(&g_mic_frames_drop) / g_mic_rate : 0.0,
                mpk, mpk ? "the microphone heard audio" : "THE MICROPHONE HEARD ONLY SILENCE");
        fprintf(f, "  capture path: %s%s; client %u Hz/%u ch; last render error %d\n",
                g_mic_unit_name, g_duplex ? " (shared speaker reference)" : "",
                g_mic_rate, g_mic_ch, atomic_load(&g_mic_render_error));
    }
}

// ===========================================================================
// Microphone capture and the duplex voice-processing unit.
//
// Hardware format discovery determines the initial capture client format.
// Rebuilds retain the client rate/channels already advertised to guest readers;
// the voice unit converts from the new hardware route as necessary.
// ===========================================================================

static double measure_input_rate(AudioUnit u) {
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 1, &hw, &sz) == noErr &&
        hw.mSampleRate > 8000.0 && hw.mSampleRate < 400000.0)
        return hw.mSampleRate;
    return 0.0;
}

// Initial hardware channel count; guest channel mapping runs on the reader.
static unsigned measure_input_channels(AudioUnit u) {
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 1, &hw, &sz) == noErr &&
        hw.mChannelsPerFrame >= 1 && hw.mChannelsPerFrame <= 8)
        return hw.mChannelsPerFrame;
    return 0;
}

// ---- the input callback ----
//
// Real-time, and the producer for the capture ring. AAudio's render callback is
// the twin of render_cb above with the arrow reversed: CoreAudio hands us an
// empty AudioBufferList slot (io is NULL for an input unit), we AudioUnitRender()
// the mic's frames into our fixed scratch, and copy them into the ring. No lock,
// no allocation, no guest code — the resample/convert the guest needs happens on
// its own reader thread.
static OSStatus capture_cb(void *ref, AudioUnitRenderActionFlags *flags,
                           const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                           AudioBufferList *io) {
    (void)ref; (void)io;
    AudioUnit u = g_cap_unit;
    if (!u || !g_mic_ring || !g_cap_scratch) return noErr;
    // Never ask AudioUnitRender for more than the scratch can hold. iOS bursts
    // are far below this cap; a larger one is simply clipped rather than risking
    // a write past the buffer.
    if (nframes > g_cap_scratch_frames) nframes = (UInt32)g_cap_scratch_frames;

#if KL_AUDIO_DUPLEX
    g_cap_abl.mNumberBuffers=g_mic_ch;
    for(unsigned ch=0;ch<g_mic_ch;ch++){
        g_cap_abl.mBuffers[ch].mNumberChannels=1;
        g_cap_abl.mBuffers[ch].mDataByteSize=nframes*sizeof(float);
        g_cap_abl.mBuffers[ch].mData=g_cap_float_scratch+ch*g_cap_scratch_frames;
    }
#else
    g_cap_abl.mNumberBuffers = 1;
    g_cap_abl.mBuffers[0].mNumberChannels = g_mic_ch;
    g_cap_abl.mBuffers[0].mDataByteSize   = (UInt32)(nframes * g_mic_ch * sizeof(int16_t));
    g_cap_abl.mBuffers[0].mData           = g_cap_scratch;
#endif
    OSStatus s = AudioUnitRender(u, flags, ts, bus, nframes, (AudioBufferList*)&g_cap_abl);
    if (s != noErr) {
        atomic_store_explicit(&g_mic_render_error, s, memory_order_relaxed);
        return noErr;
    }
#if KL_AUDIO_DUPLEX
    for(size_t frame=0;frame<nframes;frame++)for(unsigned ch=0;ch<g_mic_ch;ch++){
        float v=g_cap_float_scratch[ch*g_cap_scratch_frames+frame];
        int sample=!isfinite(v)?0:v>=1?32767:v<=-1?-32768:(int)lrintf(v*32768.f);
        if(sample>32767)sample=32767;
        g_cap_scratch[frame*g_mic_ch+ch]=(int16_t)sample;
    }
#endif

    // A cheap peak — the "did the mic hear anything" verdict the report prints,
    // and pure arithmetic, so it is fine on the real-time thread.
    int peak = 0;
    size_t ns = (size_t)nframes * g_mic_ch;
    for (size_t i = 0; i < ns; i++) {
        int v = g_cap_scratch[i] < 0 ? -g_cap_scratch[i] : g_cap_scratch[i];
        if (v > peak) peak = v;
    }
    if (peak > atomic_load_explicit(&g_mic_peak, memory_order_relaxed))
        atomic_store_explicit(&g_mic_peak, peak, memory_order_relaxed);

    // Append to the ring. The reader owns g_mic_r; we own g_mic_w. If the guest
    // is not draining fast enough the ring fills, and we drop the NEWEST frames
    // rather than touch g_mic_r from the wrong thread — a bounded, lock-free
    // overrun that a continuously-reading guest (the voice path) never reaches.
    size_t w = atomic_load_explicit(&g_mic_w, memory_order_relaxed);
    size_t r = atomic_load_explicit(&g_mic_r, memory_order_acquire);
    size_t freeframes = g_mic_cap - (w - r);
    size_t take = nframes;
    if (take > freeframes) {
        atomic_fetch_add_explicit(&g_mic_frames_drop, take - freeframes, memory_order_relaxed);
        take = freeframes;
    }
    for (size_t done = 0; done < take; ) {
        size_t off = (w + done) % g_mic_cap;
        size_t run = g_mic_cap - off;
        if (run > take - done) run = take - done;
        memcpy(g_mic_ring + off * g_mic_ch, g_cap_scratch + done * g_mic_ch,
               run * g_mic_ch * sizeof(int16_t));
        done += run;
    }
    atomic_store_explicit(&g_mic_w, w + take, memory_order_release);
    atomic_fetch_add_explicit(&g_mic_frames_cap, take, memory_order_relaxed);
    return noErr;
}

// The device client format, set on the OUTPUT scope
// of the input element (element 1). Initial capture uses the native clock;
// recovery retains that client format. Guest-rate conversion is filtered and
// performed on the reader thread, outside the real-time callback.
static OSStatus cap_set_client_format(unsigned rate, unsigned ch) {
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
#if KL_AUDIO_DUPLEX
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = sizeof(float);
#else
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 16;
    fmt.mBytesPerFrame    = (UInt32)(sizeof(int16_t) * ch);
#endif
    fmt.mChannelsPerFrame = ch;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    return AudioUnitSetProperty(g_cap_unit, kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Output, 1, &fmt, sizeof fmt);
}

// Build a duplex voice unit on visionOS, or raw HAL input on the macOS host.
// The first open measures native input. Later rebuilds retain the reader's
// client clock. Refuse a mismatched negotiated format instead of relabelling it.
static int cap_build(OSType subtype, const char *name, int vpio) {
    // AUVoiceIO implicitly selects voiceChat. Session recovery must preserve
    // that mode while this unit is being initialized, without taking g_lock.
    atomic_store(&g_mic_voice_processing, vpio);
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = subtype;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        atomic_store(&g_mic_voice_processing, 0);
        fprintf(stderr, "  [au] no %s AudioComponent\n", name); return -1;
    }
    CHECK(AudioComponentInstanceNew(comp, &g_cap_unit), "AudioComponentInstanceNew(capture)");

    // Capture and the real speaker mix use the SAME voice-processing unit.
    // Its bus 0 is the echo canceller's far-end reference; a separate output
    // unit (or a silent reference) leaves the speaker signal in the microphone.
    UInt32 one = 1, zero = 0;
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &one, sizeof one),
          "EnableIO(input,bus1)");
    UInt32 output_enabled = KL_AUDIO_DUPLEX ? one : zero;
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &output_enabled, sizeof output_enabled),
          "EnableIO(output,bus0)");
#if KL_AUDIO_DUPLEX
    double output_rate = measure_device_rate(g_cap_unit);
    // Keep the existing mix/capture client clocks stable across route changes.
    // VPIO performs hardware conversion; guests must never see relabelled PCM.
    if (g_out_rate <= 0) g_out_rate = output_rate > 0 ? output_rate : 48000;
    AudioStreamBasicDescription output = {0};
    output.mSampleRate = g_out_rate;
    output.mFormatID = kAudioFormatLinearPCM;
    output.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    output.mChannelsPerFrame = g_out_ch;
    output.mFramesPerPacket = 1;
    output.mBitsPerChannel = 32;
    output.mBytesPerFrame = output.mBytesPerPacket = sizeof(float);
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &output, sizeof output),
          "SetProperty(duplex output format)");
    AURenderCallbackStruct speaker = {render_cb, NULL};
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &speaker, sizeof speaker),
          "SetProperty(duplex speaker reference)");
#endif

#if TARGET_OS_OSX
    // HALOutput records from no device until told which; point it at the system
    // default input. (No-op on the visionOS units, which have exactly one.)
    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDefaultInputDevice,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioDeviceID dev = 0; UInt32 dsz = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &dsz, &dev) == noErr && dev)
        CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &dev, sizeof dev),
              "SetProperty(CurrentDevice)");
#endif

    // VoiceProcessingIO's voice-processing knobs, set defensively (a device that
    // rejects one still records) and only on VPIO — a plain RemoteIO/HALOutput has
    // no such properties. BypassVoiceProcessing = 0 makes sure the noise
    // suppression + AGC that cleans the raw beamformed array is actually ON;
    // EnableAGC = 1 asks for the automatic gain that lifts the low capture level.
    if (vpio) {
        UInt32 off = 0, on = 1;
        OSStatus vp = AudioUnitSetProperty(g_cap_unit, kAUVoiceIOProperty_BypassVoiceProcessing,
                                           kAudioUnitScope_Global, 0, &off, sizeof off);
        OSStatus ag = AudioUnitSetProperty(g_cap_unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
                                           kAudioUnitScope_Global, 0, &on, sizeof on);
        // VPIO ducks other apps independently of AVAudioSession's duckOthers
        // option, even for capture-only use. Minimize that separate attenuation;
        // this API has no "none" level. Mic OFF remains the control that avoids
        // VPIO and competing for capture altogether.
        AUVoiceIOOtherAudioDuckingConfiguration ducking = {
            .mEnableAdvancedDucking = false,
            .mDuckingLevel = kAUVoiceIOOtherAudioDuckingLevelMin,
        };
        OSStatus duck = AudioUnitSetProperty(g_cap_unit,
            kAUVoiceIOProperty_OtherAudioDuckingConfiguration,
            kAudioUnitScope_Global, 0, &ducking, sizeof ducking);
        if (duck != noErr) {
            char b[8];
            fprintf(stderr, "  [au] VPIO minimum other-audio ducking rejected (non-fatal): %s\n",
                    fourcc(duck, b));
        }
        if (vp != noErr || ag != noErr) {
            char b1[8], b2[8];
            fprintf(stderr, "  [au] VPIO props (non-fatal): bypass=%s, AGC=%s\n",
                    vp == noErr ? "ok" : fourcc(vp, b1), ag == noErr ? "ok" : fourcc(ag, b2));
        }
    }

    double hw = measure_input_rate(g_cap_unit);
    if (hw <= 0.0) hw = g_session_rate > 0.0 ? g_session_rate : 48000.0;
    g_mic_dev_rate = (unsigned)hw;
    unsigned hw_ch = measure_input_channels(g_cap_unit);
    if (!g_mic_open || !g_mic_rate) {
        g_mic_ch = hw_ch < 1 ? 1 : (hw_ch > 2 ? 2 : hw_ch);
        g_mic_rate = g_mic_dev_rate;
    }

    // The input callback is registered on the global scope; the element it names
    // is the input bus the callback then renders from.
    AURenderCallbackStruct cb = {capture_cb, NULL};
    CHECK(AudioUnitSetProperty(g_cap_unit, kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Global, 0, &cb, sizeof cb),
          "SetProperty(SetInputCallback)");

    // Scratch big enough for any plausible burst; the callback clips to it.
    cap_free_scratch();
    g_cap_scratch_frames = 4096;
    g_cap_scratch = calloc(g_cap_scratch_frames * g_mic_ch, sizeof(int16_t));
    if (!g_cap_scratch) { fprintf(stderr, "  [au] capture scratch OOM\n"); goto fail; }
#if KL_AUDIO_DUPLEX
    g_cap_float_scratch=calloc(g_cap_scratch_frames*g_mic_ch,sizeof(float));
    if(!g_cap_float_scratch){fprintf(stderr,"  [au] float capture scratch OOM\n");goto fail;}
#endif

    // Native on first open; stable client format across route recovery.
    CHECK(cap_set_client_format(g_mic_rate, g_mic_ch), "SetProperty(StreamFormat,capture)");
    fprintf(stderr,"  [au] initializing capture: %s, %u Hz/%u ch %s, speaker %.0f Hz/%u ch\n",
            name,g_mic_rate,g_mic_ch,KL_AUDIO_DUPLEX?"float32 planar":"int16 interleaved",g_out_rate,g_out_ch);
    CHECK(AudioUnitInitialize(g_cap_unit), "AudioUnitInitialize(capture)");
    if (vpio) {
        // Report effective settings after initialization, rather than treating
        // a successful pre-initialize request as proof of active processing.
        UInt32 bypass = UINT32_MAX, agc = UINT32_MAX, size = sizeof bypass;
        OSStatus bp = AudioUnitGetProperty(g_cap_unit, kAUVoiceIOProperty_BypassVoiceProcessing,
                                           kAudioUnitScope_Global, 0, &bypass, &size);
        size = sizeof agc;
        OSStatus ap = AudioUnitGetProperty(g_cap_unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
                                           kAudioUnitScope_Global, 0, &agc, &size);
        fprintf(stderr, "  [au] voice processing effective: bypass=%s (status %d), AGC=%s (status %d)\n",
                bp == noErr ? (bypass ? "ON" : "off") : "unknown", (int)bp,
                ap == noErr ? (agc ? "on" : "OFF") : "unknown", (int)ap);
    }
    AudioStreamBasicDescription actual = {0};
    UInt32 actual_size = sizeof actual;
    CHECK(AudioUnitGetProperty(g_cap_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 1, &actual, &actual_size),
          "GetProperty(capture client format)");
    if (fabs(actual.mSampleRate - g_mic_rate) > 0.5 || actual.mChannelsPerFrame != g_mic_ch) {
        fprintf(stderr, "  [au] capture client format changed unexpectedly: %.0f Hz/%u ch\n",
                actual.mSampleRate, (unsigned)actual.mChannelsPerFrame);
        goto fail;
    }
#if KL_AUDIO_DUPLEX
    UInt32 required=kAudioFormatFlagIsFloat|kAudioFormatFlagIsNonInterleaved;
    if((actual.mFormatFlags&required)!=required||actual.mBitsPerChannel!=32||actual.mBytesPerFrame!=sizeof(float)){
        fprintf(stderr,"  [au] capture did not retain canonical float32 planes\n");goto fail;
    }
#endif

    g_mic_unit_name = name;
    return 0;
fail:
    atomic_store(&g_mic_voice_processing, 0);
    if (g_cap_unit) { AudioComponentInstanceDispose(g_cap_unit); g_cap_unit = NULL; }
    cap_free_scratch();
    return -1;
}

// ---- the capture unit ----
static int cap_unit_create(unsigned want_rate, unsigned want_ch) {
    (void)want_rate; (void)want_ch;   // captured native; kl_aaudio.c converts to the guest's ask
#if TARGET_OS_OSX && !defined(KL_AUDIO_DUPLEX_TEST)
    // macOS is the `make check` host, not the target: HALOutput records raw, which
    // is enough to verify the plumbing. No VPIO fallback dance here.
    if (cap_build(kAudioUnitSubType_HALOutput, "HALOutput", 0) != 0) return -1;
#else
    // VoiceProcessingIO supplies noise suppression, AGC, and echo cancellation.
    // Its output callback receives the actual game mix as the far-end reference.
    // If unavailable, a raw duplex RemoteIO fallback keeps capture working,
    // without claiming echo cancellation or noise processing.
    if (kl_env_on("KL_MIC_RAW", 0)) {
        // Isolate VPIO's cross-app ducking from microphone/route competition.
        // Diagnostic only: this sacrifices noise suppression and automatic gain.
        if (cap_build(kAudioUnitSubType_RemoteIO, "RemoteIO (KL_MIC_RAW)", 0) != 0) return -1;
    } else if (cap_build(kAudioUnitSubType_VoiceProcessingIO, "VoiceProcessingIO", 1) != 0) {
        fprintf(stderr, "  [au] VPIO capture unavailable — falling back to plain "
                        "RemoteIO (raw mic)\n");
        if (cap_build(kAudioUnitSubType_RemoteIO, "RemoteIO", 0) != 0) return -1;
    }
#endif

    // The one-time verdict the next steamlink log is read against.
    fprintf(stderr, "  [au] microphone capture: %s, reader %u Hz/%u ch int16 (device %u Hz; host %s)\n",
            g_mic_unit_name, g_mic_rate, g_mic_ch, g_mic_dev_rate,
            KL_AUDIO_DUPLEX?"float32 planar":"int16 interleaved");
    return 0;
}

// ---- the opt-in surface (see kl_audio.h) ----
void kl_audio_mic_set_enabled(int on) {
    on = on ? 1 : 0;
    if (on == g_mic_enabled) return;
    g_mic_enabled = on;
    fprintf(stderr, "  [au] microphone capture %s\n", on ? "armed (lazy — opens on first guest input stream)" : "disarmed");
    // Turning it off closes the device and drops the ring immediately: the whole
    // point of the toggle is that OFF touches no hardware.
    if (!on) kl_audio_mic_close();
}

int kl_audio_mic_enabled(void) { return g_mic_enabled; }
int kl_audio_mic_voice_processing(void) { return atomic_load(&g_mic_voice_processing); }

int kl_audio_mic_open(unsigned want_rate, unsigned want_channels) {
    if (!g_mic_enabled || !kl_env_on("KL_AUDIO", 1)) return -1;
    pthread_mutex_lock(&g_lock);
    if (g_host_suspended || g_interrupted || !g_mic_enabled) {
        pthread_mutex_unlock(&g_lock); return -1;
    }
    if (g_mic_open) { pthread_mutex_unlock(&g_lock); return 0; }
    pthread_mutex_lock(&g_wlock);
#if KL_AUDIO_DUPLEX
    unit_destroy();
#endif
    // First open measures the input format. Rebuilds retain that client format
    // so an already-open Android stream keeps its sample rate and channel count.
    g_mic_rate = 0;
    if (!g_open) g_out_rate = 0;
    if (cap_unit_create(want_rate, want_channels) != 0) goto fail;
    free(g_mic_ring);
    g_mic_cap = (size_t)g_mic_rate + 1;
    g_mic_ring = calloc(g_mic_cap * g_mic_ch, sizeof(int16_t));
    if (!g_mic_ring) goto fail;
    atomic_store(&g_mic_w, 0); atomic_store(&g_mic_r, 0);
    g_mic_open = 1;
#if KL_AUDIO_DUPLEX
    g_unit = g_cap_unit;
    g_duplex = 1;
    if (unit_start() != 0) goto fail;
#else
    if (AudioOutputUnitStart(g_cap_unit) != noErr) goto fail;
#endif
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
    watchdog_start();
    fprintf(stderr, "  [au] CoreAudio in: guest wants %u Hz/%u ch -> capturing %u Hz/%u ch%s\n",
            want_rate, want_channels, g_mic_rate, g_mic_ch,
            g_duplex ? " (shared capture/playback)" : "");
    return 0;
fail:
    atomic_store(&g_mic_voice_processing, 0);
    if (g_duplex) unit_destroy();
    if (g_cap_unit) {
        AudioOutputUnitStop(g_cap_unit);
        AudioUnitUninitialize(g_cap_unit);
        AudioComponentInstanceDispose(g_cap_unit); g_cap_unit = NULL;
    }
    g_mic_open = 0;
    free(g_mic_ring); g_mic_ring = NULL; g_mic_cap = 0;
    cap_free_scratch();
#if KL_AUDIO_DUPLEX
    // A refused microphone must not take existing game playback away.
    if (g_open && unit_create() == 0 && resize_output_ring() == 0) {
        reset_output_clock(); unit_start();
    }
#endif
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
    return -1;
}

void kl_audio_mic_close(void) {
    pthread_mutex_lock(&g_lock);
    pthread_mutex_lock(&g_wlock);
    int was_duplex = g_duplex;
    if (was_duplex) unit_destroy();
    if (g_cap_unit) {
        AudioOutputUnitStop(g_cap_unit);
        AudioUnitUninitialize(g_cap_unit);
        AudioComponentInstanceDispose(g_cap_unit); g_cap_unit = NULL;
    }
    g_mic_open = 0;
    g_mic_host_stopped = 0;
    atomic_store(&g_mic_voice_processing, 0);
    free(g_mic_ring); g_mic_ring = NULL; g_mic_cap = 0;
    cap_free_scratch();
    atomic_store(&g_mic_w, 0); atomic_store(&g_mic_r, 0);
    if (was_duplex && g_open && unit_create() == 0 && resize_output_ring() == 0) {
        reset_output_clock(); unit_start();
    }
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_unlock(&g_lock);
}

unsigned kl_audio_mic_rate(void)     { return g_mic_rate; }
unsigned kl_audio_mic_channels(void) { return g_mic_ch; }

int kl_audio_mic_read_i16(int16_t *buf, int frames) {
    if (!buf || frames <= 0) return 0;
    // Consumers run off the render thread. Serialize ring access with disable /
    // close so a live Photon worker cannot read storage being freed by the UI.
    pthread_mutex_lock(&g_lock);
    if (!g_mic_open || !g_mic_enabled || !g_mic_ring || g_host_suspended) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    size_t r = atomic_load_explicit(&g_mic_r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&g_mic_w, memory_order_acquire);
    size_t have = w - r;
    size_t fresh = g_mic_rate / 10; // At most 100 ms of queued voice after a stall.
    if (fresh < (size_t)frames) fresh = (size_t)frames;
    if (have > fresh) {
        size_t stale = have - fresh;
        r += stale; have -= stale;
        atomic_fetch_add_explicit(&g_mic_frames_drop, stale, memory_order_relaxed);
    }
    size_t take = have < (size_t)frames ? have : (size_t)frames;
    for (size_t done = 0; done < take; ) {
        size_t off = (r + done) % g_mic_cap;
        size_t run = g_mic_cap - off;
        if (run > take - done) run = take - done;
        memcpy(buf + done * g_mic_ch, g_mic_ring + off * g_mic_ch,
               run * g_mic_ch * sizeof(int16_t));
        done += run;
    }
    atomic_store_explicit(&g_mic_r, r + take, memory_order_release);
    atomic_fetch_add_explicit(&g_mic_frames_read, take, memory_order_relaxed);
    pthread_mutex_unlock(&g_lock);
    return (int)take;
}
