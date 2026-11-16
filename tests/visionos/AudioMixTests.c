// Run the real PCM writer and render callback without opening an audio device.
#include <assert.h>
#include <AudioToolbox/AudioToolbox.h>
#include "../../runtime/media/kl_audio.c"

static void reset_sink(void) {
    free(g_ring); free(g_scratch);
    g_scratch = NULL; g_scratch_frames = 0;
    g_cap = 129; g_target = 64;
    g_ring = calloc(g_cap * 2, sizeof(float));
    g_in_rate = 48000; g_out_rate = 48000;
    g_in_ch = g_out_ch = 2; g_in_bits = 16;
    g_open = g_playing = 1;
    g_src_n = 0;
    memset(g_src_live, 0, sizeof g_src_live);
    memset(g_src_key, 0, sizeof g_src_key);
    memset(g_src_pos, 0, sizeof g_src_pos);
    atomic_store(&g_w, 0); atomic_store(&g_r, 0);
}

static void check_streams(const void *silent_source, const void *audible_source) {
    reset_sink();
    int16_t silence[32] = {0}, sound[32];
    for (int i = 0; i < 32; i++) sound[i] = i & 1 ? -8192 : 8192;
    for (int block = 0; block < 64; block++) {
        assert(kl_audio_write_src(silent_source, silence, sizeof silence) == sizeof silence);
        assert(kl_audio_write_src(audible_source, sound, sizeof sound) == sizeof sound);
        // Concurrent streams occupy the same timeline, rather than appending
        // a silent block before each sound block. Cover repeated ring wraps.
        assert(atomic_load(&g_w) - atomic_load(&g_r) == 16);
        float output[32];
        AudioBufferList buffers = {.mNumberBuffers = 1,
            .mBuffers = {{.mNumberChannels = 2, .mDataByteSize = sizeof output, .mData = output}}};
        AudioUnitRenderActionFlags flags = 0;
        assert(render_cb(NULL, &flags, NULL, 0, 16, &buffers) == noErr);
        for (int i = 0; i < 32; i++) assert(output[i] == (i & 1 ? -0.25f : 0.25f));
    }
}

typedef struct {
    const void *source;
    int16_t pcm[32];
    _Atomic int started, done;
    size_t written;
} writer;
static void *write_block(void *argument) {
    writer *w = argument;
    atomic_store(&w->started, 1);
    w->written = kl_audio_write_src(w->source, w->pcm, sizeof w->pcm);
    atomic_store(&w->done, 1);
    return NULL;
}
static void check_waiting_writer(int registered) {
    reset_sink();
    static const char fmod;
    src_slot(NULL);
    if (registered) src_slot(&fmod);
    // Unity has filled its timeline. FMOD still has room to mix into it.
    atomic_store(&g_w, 64);
    g_src_pos[0] = 64; g_src_pos[1] = 16;
    atomic_store(&g_last_render_ns, now_ns());
    writer unity = {0}, game = {.source = &fmod};
    for (int i = 0; i < 32; i++) game.pcm[i] = 8192;
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, write_block, &unity));
    while (!atomic_load(&unity.started)) nap_ms(1);
    nap_ms(10);
    assert(!pthread_create(&b, NULL, write_block, &game));
    for (int i = 0; i < 25 && !atomic_load(&game.done); i++) nap_ms(5);
    // FMOD must not wait for Unity's full queue to drain: its own cursor fits.
    assert(atomic_load(&game.done));
    assert(!atomic_load(&unity.done));
    float output[128];
    AudioBufferList buffers = {.mNumberBuffers = 1,
        .mBuffers = {{.mNumberChannels = 2, .mDataByteSize = sizeof output, .mData = output}}};
    AudioUnitRenderActionFlags flags = 0;
    render_cb(NULL, &flags, NULL, 0, 64, &buffers);
    int first = registered ? 32 : 0;
    for (int i = 0; i < 128; i++) assert(output[i] == (i >= first && i < first + 32 ? 0.25f : 0));
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(atomic_load(&unity.done));
    assert(unity.written == sizeof unity.pcm && game.written == sizeof game.pcm);
    // The waiting writer retains its own PCM when another conversion runs.
    render_cb(NULL, &flags, NULL, 0, 16, &buffers);
    for (int i = 0; i < 32; i++) assert(output[i] == 0);
}

static void check_flush_waiter(void) {
    reset_sink();
    src_slot(NULL);
    atomic_store(&g_w, 64); g_src_pos[0] = 64;
    atomic_store(&g_last_render_ns, now_ns());
    writer pending = {0};
    for (int i = 0; i < 32; i++) pending.pcm[i] = 8192;
    pthread_t thread; assert(!pthread_create(&thread, NULL, write_block, &pending));
    while (!atomic_load(&pending.started)) nap_ms(1);
    nap_ms(10); assert(!atomic_load(&pending.done));
    kl_audio_flush();
    pthread_join(thread, NULL);
    assert(pending.written == 0 && atomic_load(&g_w) == 0);
    for (size_t i = 0; i < g_cap * 2; i++) assert(g_ring[i] == 0);
    assert(kl_audio_write_src(NULL, pending.pcm, sizeof pending.pcm) == sizeof pending.pcm);
    assert(atomic_load(&g_w) == 16);
}

static void check_rate(unsigned rate, unsigned channels) {
    reset_sink();
    // Room for the complete sample-clock test; no hardware or sleeping.
    free(g_ring); g_cap = 200003; g_target = 200000;
    g_ring = calloc(g_cap * 2, sizeof(float));
    static const char voice, game;
    unsigned input_frames = 0;
    for (int block = 0; block < 300; block++) {
        // Irregular, small voice chunks expose fractional carry accidentally
        // shared with the independent 48 kHz game stream.
        unsigned count = 1 + block % 37;
        int16_t pcm[74];
        for (unsigned i = 0; i < count; i++) {
            pcm[i * channels] = (int16_t)(10000 * sin(2 * M_PI * 1000 * (input_frames + i) / rate));
            if (channels == 2) pcm[i * 2 + 1] = -pcm[i * 2];
        }
        assert(kl_audio_write_pcm(&voice, pcm, count * channels * 2, rate, channels) == count * channels * 2);
        input_frames += count;
        int16_t silence[32] = {0};
        assert(kl_audio_write_pcm(&game, silence, sizeof silence, 48000, 2) == sizeof silence);
    }
    int slot = src_slot(&voice);
    double expected = (double)input_frames * 48000 / rate;
    assert(fabs((double)g_src_pos[slot] - expected) <= ceil(48000.0 / rate));
    // Count audible cycles: delivered pitch must stay at 1 kHz, not change
    // when a stream at a different rate writes between voice chunks.
    int crossings = 0;
    for (size_t i = 2; i < g_src_pos[slot]; i++) {
        float left = g_ring[i * 2], right = g_ring[i * 2 + 1];
        assert(fabs(left - (channels == 1 ? right : -right)) < 0.00001);
        if (g_ring[(i-1) * 2] <= 0 && left > 0) crossings++;
    }
    assert(fabs(crossings - (double)input_frames * 1000 / rate) < 2);
    // Closed/recreated sources reuse slots instead of corrupting another
    // source when a guest recreates its devices repeatedly.
    for (int n = 0; n < 40; n++) {
        kl_audio_forget_src(&voice);
        int16_t pcm[8] = {0};
        assert(kl_audio_write_pcm(&voice, pcm, sizeof pcm, rate, channels) == sizeof pcm);
        assert(g_src_n == 2);
    }
}

int main(void) {
    static const char fmod, other;
    check_streams(NULL, &fmod);  // Unity/OpenSL plus Eleven's FMOD Java output
    check_streams(&fmod, NULL);  // Either registration order must work
    check_streams(&fmod, &other); // Existing Steam Link streams
    reset_sink();
    int16_t pcm[32] = {0};
    assert(kl_audio_write(pcm, sizeof pcm) == sizeof pcm);
    assert(kl_audio_write(pcm, sizeof pcm) == sizeof pcm);
    assert(atomic_load(&g_w) == 32); // Single-source writes still append
    check_waiting_writer(1);
    check_waiting_writer(0);
    check_flush_waiter();
    check_rate(16000, 1);
    check_rate(24000, 1);
    check_rate(44100, 2);
    check_rate(96000, 1);
    free(g_ring); free(g_scratch);
    puts("PCM streams mix without inserted silence or blocking another producer");
    return 0;
}
