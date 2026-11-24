// SPDX-License-Identifier: Apache-2.0

#include "app_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app_audio_fir.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_audio";

#define BUS_RATE_HZ          48000
#define BUS_CHANNELS         2
#define CAPTURE_FRAMES       (APP_AUDIO_MIC_FRAME * (BUS_RATE_HZ / APP_AUDIO_MIC_RATE_HZ)) /* 1920 */
#define PLAY_IN_SAMPLES      480                                                           /* 20 ms @24k */
#define PLAY_RATIO           (BUS_RATE_HZ / APP_AUDIO_SPK_RATE_HZ)                         /* 2 */
#define PLAY_OUT_FRAMES      (PLAY_IN_SAMPLES * PLAY_RATIO)                                /* 960 */
#define RING_SAMPLES         (APP_AUDIO_SPK_RATE_HZ * 8)                                   /* 8 s */
#define CAPTURE_TASK_PRIO    18
#define PLAYBACK_TASK_PRIO   19
#define AUDIO_TASK_CORE      1
#define AUDIO_TASK_STACK     4096

/* Singleton: one ES8311 codec. Buffers are instance-owned (no large stack arrays).
 * Cross-task fields use GCC __atomic builtins; the ring is single-producer
 * (app_audio_play caller) / single-consumer (playback task). */
static struct {
    bool started;
    app_audio_config_t config;
    esp_codec_dev_handle_t mic;
    esp_codec_dev_handle_t spk;
    audio_fir_t decimator;
    audio_fir_t interpolator;
    int16_t *capture_raw;     /* CAPTURE_FRAMES * 2 */
    float *capture_mono;      /* CAPTURE_FRAMES */
    int16_t *mic_out;         /* APP_AUDIO_MIC_FRAME */
    int16_t *play_in;         /* PLAY_IN_SAMPLES */
    int16_t *play_out;        /* PLAY_OUT_FRAMES * 2 */
    int16_t *ring;            /* RING_SAMPLES, PSRAM */
    uint32_t ring_write;      /* monotonic sample counters */
    uint32_t ring_read;
    uint32_t flush_request;
    uint32_t mic_enabled;
    int64_t last_audio_us;
    uint32_t level;
} s_audio;

static esp_err_t codec_err(int ret, const char *what)
{
    if (ret == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed (codec error %d)", what, ret);
    return ESP_FAIL;
}

static void capture_task(void *arg)
{
    (void)arg;
    const size_t bytes = CAPTURE_FRAMES * BUS_CHANNELS * sizeof(int16_t);
    for (;;) {
        if (esp_codec_dev_read(s_audio.mic, s_audio.capture_raw, (int)bytes) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* ES8311 is a mono ADC: use whichever I2S slot carries energy. */
        int64_t energy_l = 0;
        int64_t energy_r = 0;
        for (size_t i = 0; i < CAPTURE_FRAMES; ++i) {
            energy_l += abs(s_audio.capture_raw[2 * i]);
            energy_r += abs(s_audio.capture_raw[2 * i + 1]);
        }
        const size_t slot = energy_r > energy_l ? 1 : 0;
        for (size_t i = 0; i < CAPTURE_FRAMES; ++i) {
            s_audio.capture_mono[i] = (float)s_audio.capture_raw[2 * i + slot];
        }
        const size_t n = audio_fir_decimate(&s_audio.decimator, s_audio.capture_mono,
                                            CAPTURE_FRAMES, s_audio.mic_out);
        if (__atomic_load_n(&s_audio.mic_enabled, __ATOMIC_ACQUIRE) && n > 0) {
            s_audio.config.on_mic(s_audio.mic_out, n, s_audio.config.ctx);
        }
    }
}

static size_t ring_take(int16_t *out, size_t max)
{
    if (__atomic_exchange_n(&s_audio.flush_request, 0U, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&s_audio.ring_read,
                         __atomic_load_n(&s_audio.ring_write, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    }
    const uint32_t write = __atomic_load_n(&s_audio.ring_write, __ATOMIC_ACQUIRE);
    uint32_t read = s_audio.ring_read;
    size_t available = write - read;
    const size_t n = available < max ? available : max;
    for (size_t i = 0; i < n; ++i) {
        out[i] = s_audio.ring[(read + i) % RING_SAMPLES];
    }
    __atomic_store_n(&s_audio.ring_read, read + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
}

static void playback_task(void *arg)
{
    (void)arg;
    const size_t bytes = PLAY_OUT_FRAMES * BUS_CHANNELS * sizeof(int16_t);
    float smoothed = 0.0f;
    for (;;) {
        const size_t got = ring_take(s_audio.play_in, PLAY_IN_SAMPLES);
        if (got < PLAY_IN_SAMPLES) {
            memset(&s_audio.play_in[got], 0, (PLAY_IN_SAMPLES - got) * sizeof(int16_t));
        }
        float rms = 0.0f;
        if (got > 0) {
            double acc = 0.0;
            for (size_t i = 0; i < got; ++i) {
                acc += (double)s_audio.play_in[i] * s_audio.play_in[i];
            }
            rms = sqrtf((float)(acc / (double)got));
            __atomic_store_n(&s_audio.last_audio_us, esp_timer_get_time(), __ATOMIC_RELEASE);
        }
        /* Map RMS (≈ -50..-10 dBFS) to 0..100 with attack/decay smoothing. */
        float db = rms > 1.0f ? 20.0f * log10f(rms / 32768.0f) : -90.0f;
        float target = (db + 50.0f) * 2.5f;
        target = target < 0.0f ? 0.0f : (target > 100.0f ? 100.0f : target);
        smoothed += (target - smoothed) * (target > smoothed ? 0.6f : 0.15f);
        __atomic_store_n(&s_audio.level, (uint32_t)smoothed, __ATOMIC_RELAXED);

        audio_fir_interpolate_stereo(&s_audio.interpolator, s_audio.play_in, PLAY_IN_SAMPLES,
                                     s_audio.play_out);
        if (esp_codec_dev_write(s_audio.spk, s_audio.play_out, (int)bytes) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void *alloc_internal(size_t bytes)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void free_buffers(void)
{
    heap_caps_free(s_audio.capture_raw);
    heap_caps_free(s_audio.capture_mono);
    heap_caps_free(s_audio.mic_out);
    heap_caps_free(s_audio.play_in);
    heap_caps_free(s_audio.play_out);
    heap_caps_free(s_audio.ring);
    s_audio.capture_raw = NULL;
    s_audio.capture_mono = NULL;
    s_audio.mic_out = NULL;
    s_audio.play_in = NULL;
    s_audio.play_out = NULL;
    s_audio.ring = NULL;
}

esp_err_t app_audio_start(const app_audio_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->on_mic, ESP_ERR_INVALID_ARG, TAG, "on_mic required");
    ESP_RETURN_ON_FALSE(!s_audio.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_audio.config = *config;
    if (s_audio.config.mic_gain_db <= 0.0f) {
        s_audio.config.mic_gain_db = 30.0f;
    }
    if (s_audio.config.volume <= 0 || s_audio.config.volume > 100) {
        s_audio.config.volume = 70;
    }
    if (s_audio.config.playback_tail_ms == 0) {
        s_audio.config.playback_tail_ms = 300;
    }

    s_audio.capture_raw = alloc_internal(CAPTURE_FRAMES * BUS_CHANNELS * sizeof(int16_t));
    s_audio.capture_mono = alloc_internal(CAPTURE_FRAMES * sizeof(float));
    s_audio.mic_out = alloc_internal(APP_AUDIO_MIC_FRAME * sizeof(int16_t));
    s_audio.play_in = alloc_internal(PLAY_IN_SAMPLES * sizeof(int16_t));
    s_audio.play_out = alloc_internal(PLAY_OUT_FRAMES * BUS_CHANNELS * sizeof(int16_t));
    s_audio.ring = heap_caps_calloc(RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_audio.capture_raw || !s_audio.capture_mono || !s_audio.mic_out ||
            !s_audio.play_in || !s_audio.play_out || !s_audio.ring) {
        free_buffers();
        return ESP_ERR_NO_MEM;
    }

    /* 48k -> 16k: pass band to 7 kHz; 24k -> 48k: image rejection above 11 kHz. */
    audio_fir_init(&s_audio.decimator, 7000.0f, BUS_RATE_HZ, 1.0f, BUS_RATE_HZ / APP_AUDIO_MIC_RATE_HZ);
    audio_fir_init(&s_audio.interpolator, 11000.0f, BUS_RATE_HZ, (float)PLAY_RATIO, PLAY_RATIO);

    s_audio.mic = bsp_audio_codec_microphone_init();
    s_audio.spk = bsp_audio_codec_speaker_init();
    if (!s_audio.mic || !s_audio.spk) {
        free_buffers();
        ESP_LOGE(TAG, "ES8311 unavailable");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t info = {
        .sample_rate = BUS_RATE_HZ,
        .bits_per_sample = 16,
        .channel = BUS_CHANNELS,
        .channel_mask = 0,
    };
    esp_err_t err = codec_err(esp_codec_dev_open(s_audio.mic, &info), "open microphone");
    if (err == ESP_OK) {
        err = codec_err(esp_codec_dev_open(s_audio.spk, &info), "open speaker");
    }
    if (err == ESP_OK) {
        err = codec_err(esp_codec_dev_set_in_gain(s_audio.mic, s_audio.config.mic_gain_db), "mic gain");
    }
    if (err == ESP_OK) {
        err = codec_err(esp_codec_dev_set_out_vol(s_audio.spk, s_audio.config.volume), "volume");
    }
    if (err != ESP_OK) {
        free_buffers();
        return err;
    }
    if (xTaskCreatePinnedToCore(playback_task, "vox_play", AUDIO_TASK_STACK, NULL,
                                PLAYBACK_TASK_PRIO, NULL, AUDIO_TASK_CORE) != pdPASS ||
            xTaskCreatePinnedToCore(capture_task, "vox_mic", AUDIO_TASK_STACK, NULL,
                                    CAPTURE_TASK_PRIO, NULL, AUDIO_TASK_CORE) != pdPASS) {
        /* A started playback task keeps running on silence; buffers stay owned. */
        ESP_LOGE(TAG, "audio task creation failed");
        return ESP_ERR_NO_MEM;
    }
    s_audio.started = true;
    ESP_LOGI(TAG, "audio ready: bus %d Hz, mic %d Hz, speaker %d Hz, gain %.0f dB, volume %d",
             BUS_RATE_HZ, APP_AUDIO_MIC_RATE_HZ, APP_AUDIO_SPK_RATE_HZ,
             (double)s_audio.config.mic_gain_db, s_audio.config.volume);
    return ESP_OK;
}

void app_audio_set_mic_enabled(bool enabled)
{
    __atomic_store_n(&s_audio.mic_enabled, enabled ? 1U : 0U, __ATOMIC_RELEASE);
}

esp_err_t app_audio_play(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (!s_audio.started || !pcm) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t done = 0;
    while (done < samples) {
        const uint32_t read = __atomic_load_n(&s_audio.ring_read, __ATOMIC_ACQUIRE);
        const uint32_t write = s_audio.ring_write;
        size_t space = RING_SAMPLES - (write - read);
        if (space == 0) {
            if (esp_timer_get_time() >= deadline) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const size_t n = (samples - done) < space ? (samples - done) : space;
        for (size_t i = 0; i < n; ++i) {
            s_audio.ring[(write + i) % RING_SAMPLES] = pcm[done + i];
        }
        __atomic_store_n(&s_audio.ring_write, write + (uint32_t)n, __ATOMIC_RELEASE);
        done += n;
    }
    return ESP_OK;
}

void app_audio_flush_playback(void)
{
    __atomic_store_n(&s_audio.flush_request, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_audio.last_audio_us, 0, __ATOMIC_RELEASE);
}

bool app_audio_is_playing(void)
{
    if (!s_audio.started) {
        return false;
    }
    const uint32_t queued = __atomic_load_n(&s_audio.ring_write, __ATOMIC_ACQUIRE) -
                            __atomic_load_n(&s_audio.ring_read, __ATOMIC_ACQUIRE);
    if (queued > 0 && !__atomic_load_n(&s_audio.flush_request, __ATOMIC_ACQUIRE)) {
        return true;
    }
    const int64_t last = __atomic_load_n(&s_audio.last_audio_us, __ATOMIC_ACQUIRE);
    return last != 0 &&
           esp_timer_get_time() - last < (int64_t)s_audio.config.playback_tail_ms * 1000;
}

uint8_t app_audio_output_level(void)
{
    return (uint8_t)__atomic_load_n(&s_audio.level, __ATOMIC_RELAXED);
}
