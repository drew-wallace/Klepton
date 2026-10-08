// Exercise the real watchdog tick without opening a hardware audio device.
#include <assert.h>
#include <AudioToolbox/AudioToolbox.h>
static int device_attempts;
static int device_stops, device_starts;
static OSStatus fake_stop(AudioUnit unit) { (void)unit; device_stops++; return noErr; }
static OSStatus fake_start(AudioUnit unit) { (void)unit; device_starts++; return noErr; }
static OSStatus fake_dispose(AudioUnit unit) { (void)unit; return noErr; }
static AudioComponent no_device(AudioComponent previous, const AudioComponentDescription *description) {
    (void)previous; (void)description;
    device_attempts++;
    return NULL;
}
#define AudioComponentFindNext no_device
#define AudioOutputUnitStop fake_stop
#define AudioOutputUnitStart fake_start
#define AudioUnitUninitialize fake_dispose
#define AudioComponentInstanceDispose fake_dispose
#include "../../runtime/media/kl_audio.c"

int main(void) {
    g_open = 1;
    g_playing = 1;
    atomic_store(&g_last_render_ns, now_ns());
    kl_audio_set_session_managed(1);
    kl_audio_interrupted(1);
    g_interrupt_ns = now_ns() - 10000000000ull;
    for (int i = 0; i < 100; i++) watchdog_tick();
    assert(g_interrupted && device_attempts == 0 && g_restarts == 0);
    assert(kl_audio_restart() == -1);
    kl_audio_play();
    assert(g_interrupted && device_attempts == 0);

    // Both authoritative recovery paths release the interruption and rebuild.
    kl_audio_interrupted(0);
    assert(!g_interrupted && device_attempts == 1);
    kl_audio_interrupted(1);
    kl_audio_resume();
    assert(!g_interrupted && device_attempts == 2);

    // Retain the fallback for hosts that have no AVAudioSession lifecycle.
    kl_audio_set_session_managed(0);
    kl_audio_interrupted(1);
    g_interrupt_ns = now_ns() - 10000000000ull;
    watchdog_tick();
    assert(!g_interrupted && device_attempts == 3);

    // Home is a separate hold, not an OS interruption with an expiry. Both
    // output and opted-in capture stop, and late events cannot restart them.
    g_unit = (AudioUnit)(uintptr_t)1;
    g_running = 1;
    g_cap_unit = (AudioUnit)(uintptr_t)2;
    g_mic_open = 1;
    g_mic_enabled = 1;
    kl_audio_suspend();
    assert(g_host_suspended && g_mic_host_stopped && !g_running && device_stops == 2);
    kl_audio_suspend();
    assert(device_stops == 2);
    assert(!kl_audio_active());
    kl_audio_play();
    assert(device_starts == 0);
    assert(kl_audio_restart() == -1);
    assert(kl_audio_mic_open(48000, 1) == -1);
    int16_t pcm = 0;
    assert(kl_audio_write(&pcm, sizeof pcm) == 0);
    assert(kl_audio_mic_read_i16(&pcm, 1) == 0);
    kl_audio_interrupted(0);
    atomic_store(&g_last_render_ns, now_ns() - 10000000000ull);
    for (int i = 0; i < 100; i++) watchdog_tick();
    assert(g_host_suspended && device_attempts == 3 && device_starts == 0);
    g_unit = NULL; // synthetic test unit; no real hardware object to dispose
    kl_audio_resume();
    assert(!g_host_suspended && !g_mic_host_stopped && device_attempts == 4 && device_starts == 1);
    g_cap_unit = NULL;
    g_mic_open = 0;
    g_mic_enabled = 0;

    // Repeated foreground/window-close events must preserve a live callback
    // and queued audio. Bookkeeping can say stopped while callbacks still run.
    g_unit = (AudioUnit)(uintptr_t)1;
    g_running = 1;
    atomic_store(&g_last_render_ns, now_ns());
    atomic_store(&g_r, 17);
    atomic_store(&g_w, 42);
    int before_restarts = g_restarts;
    kl_audio_resume();
    g_running = 0;
    kl_audio_resume();
    assert(device_attempts == 4 && g_restarts == before_restarts);
    assert(atomic_load(&g_r) == 17 && atomic_load(&g_w) == 42);

    // A unit that has disappeared still needs repair even with a recent stamp.
    g_unit = NULL;
    kl_audio_resume();
    assert(device_attempts == 5 && g_restarts == before_restarts + 1);
    g_unit = (AudioUnit)(uintptr_t)1;
    atomic_store(&g_last_render_ns, now_ns() - 1000000000ull);
    kl_audio_resume();
    assert(device_attempts == 6);

    // Actual Home/interrupt recovery must rebuild even with a recent heartbeat.
    g_unit = (AudioUnit)(uintptr_t)1;
    atomic_store(&g_last_render_ns, now_ns());
    kl_audio_suspend();
    kl_audio_resume();
    assert(device_attempts == 7);
    g_unit = (AudioUnit)(uintptr_t)1;
    kl_audio_interrupted(1);
    kl_audio_resume();
    assert(device_attempts == 8 && !g_interrupted);
    // Releasing the hold must also work when no guest output exists yet.
    g_open = 0;
    kl_audio_suspend();
    kl_audio_resume();
    assert(!g_host_suspended);
    // The complete silent control never attempts an output or capture device,
    // including when a guest still asks for an opted-in microphone stream.
    int before_silent_attempts = device_attempts;
    int before_silent_starts = device_starts;
    setenv("KL_AUDIO", "0", 1);
    g_mic_enabled = 1;
    assert(kl_audio_open(48000, 2, 16) == -1);
    assert(kl_audio_mic_open(48000, 1) == -1);
    assert(device_attempts == before_silent_attempts && device_starts == before_silent_starts);
    puts("Audio interruption watchdog tests passed");
    return 0;
}
