// SPDX-License-Identifier: Apache-2.0

#include "app_audio_fir.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

static int16_t clamp16(float v)
{
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)lrintf(v);
}

void audio_fir_init(audio_fir_t *fir, float cutoff_hz, float fs_hz, float gain, unsigned ratio)
{
    memset(fir, 0, sizeof(*fir));
    fir->ratio = ratio;
    const float fc = cutoff_hz / fs_hz;
    const int m = AUDIO_FIR_TAPS - 1;
    float sum = 0.0f;
    for (int k = 0; k < AUDIO_FIR_TAPS; ++k) {
        const float x = (float)k - (float)m / 2.0f;
        const float sinc = (x == 0.0f) ? 2.0f * fc : sinf(2.0f * PI_F * fc * x) / (PI_F * x);
        const float window = 0.42f - 0.5f * cosf(2.0f * PI_F * k / m) +
                             0.08f * cosf(4.0f * PI_F * k / m);
        fir->h[k] = sinc * window;
        sum += fir->h[k];
    }
    for (int k = 0; k < AUDIO_FIR_TAPS; ++k) {
        fir->h[k] *= gain / sum;
    }
}

/* y = sum_k h[k] * x[newest - k] over the circular history. */
static float convolve(const audio_fir_t *fir)
{
    float acc = 0.0f;
    unsigned idx = fir->pos;
    for (int k = 0; k < AUDIO_FIR_TAPS; ++k) {
        idx = idx ? idx - 1U : AUDIO_FIR_TAPS - 1U;
        acc += fir->h[k] * fir->hist[idx];
    }
    return acc;
}

size_t audio_fir_decimate(audio_fir_t *fir, const float *in, size_t n, int16_t *out)
{
    size_t produced = 0;
    for (size_t i = 0; i < n; ++i) {
        fir->hist[fir->pos] = in[i];
        fir->pos = (fir->pos + 1U) % AUDIO_FIR_TAPS;
        if (++fir->phase >= fir->ratio) {
            fir->phase = 0;
            out[produced++] = clamp16(convolve(fir));
        }
    }
    return produced;
}

/* Polyphase: y[L*n + p] = sum_j h[L*j + p] * x[n - j]. */
void audio_fir_interpolate_stereo(audio_fir_t *fir, const int16_t *in, size_t n, int16_t *out)
{
    const unsigned l = fir->ratio;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        fir->hist[fir->pos] = (float)in[i];
        fir->pos = (fir->pos + 1U) % AUDIO_FIR_TAPS;
        for (unsigned p = 0; p < l; ++p) {
            float acc = 0.0f;
            unsigned idx = fir->pos;
            for (unsigned k = p; k < AUDIO_FIR_TAPS; k += l) {
                /* x[n - j] with j = (k - p) / l: step back one input per tap. */
                idx = idx ? idx - 1U : AUDIO_FIR_TAPS - 1U;
                acc += fir->h[k] * fir->hist[idx];
            }
            const int16_t s = clamp16(acc);
            out[o++] = s;
            out[o++] = s;
        }
    }
}
