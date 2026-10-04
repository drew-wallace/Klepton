// Exercise the real watchdog tick without opening a hardware audio device.
#include <assert.h>
#include <AudioToolbox/AudioToolbox.h>
static int device_attempts;
static AudioComponent no_device(AudioComponent previous, const AudioComponentDescription *description) {
    (void)previous; (void)description;
    device_attempts++;
    return NULL;
}
#define AudioComponentFindNext no_device
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
    puts("Audio interruption watchdog tests passed");
    return 0;
}
