// SPDX-License-Identifier: Apache-2.0
/* Private integer-ratio FIR resamplers (decimate by M, interpolate by L). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_FIR_TAPS 47

typedef struct {
    float h[AUDIO_FIR_TAPS];
    float hist[AUDIO_FIR_TAPS];  /* circular history of recent inputs */
    unsigned pos;
    unsigned ratio;
    unsigned phase;
} audio_fir_t;

/** Windowed-sinc low-pass at cutoff_hz for sample rate fs_hz (Blackman window),
 * DC gain `gain`; ratio is M (decimation) or L (interpolation). */
void audio_fir_init(audio_fir_t *fir, float cutoff_hz, float fs_hz, float gain, unsigned ratio);

/** Decimate `in` (n samples) by fir->ratio; writes floor((n+phase)/M) samples. */
size_t audio_fir_decimate(audio_fir_t *fir, const float *in, size_t n, int16_t *out);

/** Interpolate `in` (n samples) by fir->ratio into stereo-duplicated `out`
 * (n * ratio frames = n * ratio * 2 int16). */
void audio_fir_interpolate_stereo(audio_fir_t *fir, const int16_t *in, size_t n, int16_t *out);
