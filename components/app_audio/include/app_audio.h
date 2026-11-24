// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_AUDIO_MIC_RATE_HZ      16000   /**< Uplink PCM rate (Gemini input). */
#define APP_AUDIO_SPK_RATE_HZ      24000   /**< Downlink PCM rate (Gemini output). */
#define APP_AUDIO_MIC_FRAME        640     /**< Samples per mic callback (40 ms). */

/** Mono 16 kHz PCM frame from the capture task. Must not block for long
 * (the I2S RX DMA keeps running); `pcm` is valid only during the call. */
typedef void (*app_audio_mic_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

typedef struct {
    app_audio_mic_cb_t on_mic;   /**< Required. */
    void *ctx;                   /**< Passed to on_mic; must outlive audio. */
    float mic_gain_db;           /**< ES8311 ADC gain, 0..42 dB (default 30). */
    int volume;                  /**< Speaker volume 0..100 (default 70). */
    uint32_t playback_tail_ms;   /**< Time after the last sample still "playing" (default 300). */
} app_audio_config_t;

/** Open the codec (48 kHz/16-bit/stereo bus) and start capture and playback
 * tasks. Singleton: call once. */
esp_err_t app_audio_start(const app_audio_config_t *config);

/** Gate the mic callback (I2S keeps running). Any task. */
void app_audio_set_mic_enabled(bool enabled);

/** Queue mono 24 kHz PCM for playback. Blocks up to timeout_ms when the
 * playback ring is full; returns ESP_ERR_TIMEOUT with partial data dropped.
 * Single producer task. */
esp_err_t app_audio_play(const int16_t *pcm, size_t samples, uint32_t timeout_ms);

/** Drop all queued playback immediately (interruption). Any task. */
void app_audio_flush_playback(void);

/** True while queued audio remains or within the playback tail. Any task. */
bool app_audio_is_playing(void);

/** Smoothed output loudness 0..100, lock-free (for the eye). */
uint8_t app_audio_output_level(void);

/** Capture diagnostics. `peak` is the largest |sample| of the 16 kHz uplink since
 * the previous call (reading clears it). Any task, lock-free. */
typedef struct {
    uint32_t frames;   /**< 40 ms frames captured since start */
    uint32_t peak;     /**< 0..32768 */
    uint32_t slot;     /**< I2S slot used (0 left, 1 right) */
    bool enabled;      /**< uplink gate */
} app_audio_mic_stats_t;
void app_audio_get_mic_stats(app_audio_mic_stats_t *out);

#ifdef __cplusplus
}
#endif
