// Streaming anti-alias filter for microphone downsampling. Run on the guest's
// reader thread before interpolation, never in CoreAudio's real-time callback.
#ifndef KL_PCM_FILTER_H
#define KL_PCM_FILTER_H
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define KL_PCM_TAPS 63
#define KL_PCM_CHANNELS 2
typedef struct {
    float taps[KL_PCM_TAPS], history[KL_PCM_CHANNELS][KL_PCM_TAPS];
    unsigned index, source_rate, target_rate;
    int primed;
} kl_pcm_filter;
static inline void kl_pcm_filter_apply(kl_pcm_filter *f, int16_t *pcm, size_t frames,
                                        unsigned channels, unsigned source, unsigned target) {
    if (!pcm || !frames || !channels || channels > KL_PCM_CHANNELS || !target || source <= target) return;
    if (f->source_rate != source || f->target_rate != target) {
        memset(f, 0, sizeof *f);
        f->source_rate = source; f->target_rate = target;
        // Windowed-sinc FIR: flat speech passband, bounded group delay, and a
        // stopband before frequencies fold into the guest's lower sample rate.
        double cutoff = 0.45 * (double)target / source, sum = 0;
        for (int i = 0; i < KL_PCM_TAPS; i++) {
            double x = i - (KL_PCM_TAPS - 1) * 0.5;
            double sinc = x == 0 ? 2 * cutoff : sin(2 * M_PI * cutoff * x) / (M_PI * x);
            double window = 0.42 - 0.5 * cos(2 * M_PI * i / (KL_PCM_TAPS - 1))
                                 + 0.08 * cos(4 * M_PI * i / (KL_PCM_TAPS - 1));
            f->taps[i] = (float)(sinc * window); sum += f->taps[i];
        }
        for (int i = 0; i < KL_PCM_TAPS; i++) f->taps[i] /= (float)sum;
    }
    if (!f->primed) {
        for (unsigned c = 0; c < channels; c++)
            for (int i = 0; i < KL_PCM_TAPS; i++) f->history[c][i] = pcm[c];
        f->primed = 1;
    }
    for (size_t n = 0; n < frames; n++) {
        for (unsigned c = 0; c < channels; c++) {
            f->history[c][f->index] = pcm[n * channels + c];
            float value = 0;
            unsigned j = f->index;
            for (int i = 0; i < KL_PCM_TAPS; i++) {
                value += f->taps[i] * f->history[c][j];
                j = j ? j - 1 : KL_PCM_TAPS - 1;
            }
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            pcm[n * channels + c] = (int16_t)lrintf(value);
        }
        f->index = (f->index + 1) % KL_PCM_TAPS;
    }
}
#endif
