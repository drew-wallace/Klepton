// Exercise the real watchdog tick without opening a hardware audio device.
#include <assert.h>
#include <AudioToolbox/AudioToolbox.h>
static int device_attempts;
static int device_stops, device_starts;
static OSStatus fake_stop(AudioUnit unit) { (void)unit; device_stops++; return noErr; }
static OSStatus fake_start(AudioUnit unit) { (void)unit; device_starts++; return noErr; }
static AudioComponent no_device(AudioComponent previous, const AudioComponentDescription *description) {
    (void)previous; (void)description;
    device_attempts++;
    return NULL;
}
#define AudioComponentFindNext no_device
#define AudioOutputUnitStop fake_stop
#define AudioOutputUnitStart fake_start
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
    // Releasing the hold must also work when no guest output exists yet.
    g_open = 0;
    kl_audio_suspend();
    kl_audio_resume();
    assert(!g_host_suspended);
    puts("Audio interruption watchdog tests passed");
    return 0;
}
