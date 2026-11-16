#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "kl_pcm_filter.h"
static double measure(unsigned rate, unsigned target, unsigned frequency) {
    kl_pcm_filter filter = {0};
    double energy = 0;
    size_t frames = 0;
    // Vary chunk length to cover partial mic reads and held buffer boundaries.
    for (int block = 0; block < 400; block++) {
        int n = 1 + block % 121;
        int16_t pcm[242];
        for (int i = 0; i < n; i++) {
            pcm[i*2] = (int16_t)(12000 * sin(2 * M_PI * frequency * (frames + i) / rate));
            pcm[i*2+1] = -pcm[i*2];
        }
        kl_pcm_filter_apply(&filter, pcm, n, 2, rate, target);
        for (int i = 0; i < n; i++) {
            assert(abs(pcm[i*2] + pcm[i*2+1]) <= 1);
            if (frames + i > 200) energy += (double)pcm[i*2] * pcm[i*2];
        }
        frames += n;
    }
    return sqrt(energy / (frames - 201));
}
int main(void) {
    // Preserve speech, reject energy which would alias to an audible voice
    // frequency. The old unfiltered 48->16 kHz path fails the stopband checks.
    assert(measure(48000, 16000, 1000) > 8300);
    assert(measure(48000, 16000, 3000) > 8300);
    assert(measure(48000, 16000, 12000) < 50);
    assert(measure(48000, 8000, 1000) > 8300);
    assert(measure(48000, 8000, 7000) < 50);
    kl_pcm_filter filter = {0};
    int16_t pcm[] = {-32000, 31999, 1, -1}, original[4];
    memcpy(original, pcm, sizeof pcm);
    kl_pcm_filter_apply(&filter, pcm, 2, 2, 48000, 48000);
    assert(!memcmp(original, pcm, sizeof pcm));
    puts("Capture filter: speech passband, alias rejection, stereo, partial reads and native-rate identity passed");
}
