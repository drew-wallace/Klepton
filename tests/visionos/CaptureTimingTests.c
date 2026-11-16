// Synthetic PCM through the actual guest capture converters; no device access.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
static unsigned source_rate, source_channels, source_frames, source_limit, tone;
static int fixture_read(int16_t *pcm, int frames) {
    unsigned take = frames > 121 ? 121 : (unsigned)frames;
    if (take > source_limit - source_frames) take = source_limit - source_frames;
    for (unsigned i = 0; i < take; i++) {
        int16_t sample = (int16_t)(12000 * sin(2 * M_PI * tone * (source_frames + i) / source_rate));
        pcm[i * source_channels] = sample;
        if (source_channels == 2) pcm[i*2+1] = -sample;
    }
    source_frames += take; return (int)take;
}
#define kl_audio_mic_read_i16 fixture_read
#include "../../runtime/media/kl_opensl.c"
#include "../../runtime/media/kl_aaudio.c"
static void check_capture(int aaudio, unsigned rate, unsigned channels, unsigned frequency) {
    source_rate = 48000; source_channels = channels; source_frames = 0;
    source_limit = source_rate * 2; tone = frequency;
    kl_sl_recorder recorder = {.rate = rate, .channels = channels, .mic_rate = source_rate, .mic_ch = channels};
    klaa_stream stream = {.rate = rate, .channels = channels, .format = AAUDIO_FORMAT_PCM_I16,
                         .mic_rate = source_rate, .mic_ch = channels};
    unsigned frames = 0, crosses = 0, counted = 0;
    double energy = 0; int16_t previous = 0;
    for (int iteration = 0; iteration < 20000 && frames < rate; iteration++) {
        int16_t pcm[320];
        int request = 1 + iteration % 137;
        if ((unsigned)request > rate - frames) request = rate - frames;
        int got = aaudio ? klaa_capture_into(&stream, pcm, request, 0)
                         : rec_pull(&recorder, pcm, request);
        for (int i = 0; i < got; i++) {
            int16_t sample = pcm[i * channels];
            if (channels == 2) assert(abs(sample + pcm[i*2+1]) <= 1);
            if (frames + i > rate / 10) {
                if (previous <= 0 && sample > 0) crosses++;
                energy += (double)sample * sample; counted++;
            }
            previous = sample;
        }
        frames += got;
    }
    assert(frames == rate); // exactly one second of guest voice
    assert(source_frames >= source_rate && source_frames <= source_rate + 5);
    double rms = sqrt(energy / counted);
    if (frequency == 1000) {
        assert(rms > 8100 && rms < 8600);
        assert(crosses >= 898 && crosses <= 901); // no slow motion / pitch shift
    } else assert(rms < 80); // 12 kHz must not alias into the 16 kHz voice stream
    free(recorder.in_hold); free(stream.in_hold);
}
int main(void) {
    for (int api = 0; api < 2; api++) {
        check_capture(api, 16000, 1, 1000);
        check_capture(api, 24000, 2, 1000);
        check_capture(api, 48000, 1, 1000);
        check_capture(api, 16000, 1, 12000);
    }
    puts("OpenSL/AAudio capture: one-second sample clocks, 1 kHz pitch, partial reads, stereo and alias rejection passed");
}
