// SPDX-License-Identifier: Apache-2.0

#include "app_wake.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "model_path.h"

static const char *TAG = "app_wake";

#define WAKE_PARTITION       "model"
#define WAKE_KEYWORD         "holaesp"
#define WAKE_TASK_STACK      8192
#define WAKE_TASK_PRIO       5          /* below capture (18) and playback (19) */
#define WAKE_TASK_CORE       1
#define WAKE_BUFFER_BYTES    (16000 * 2) /* 1 s of 16 kHz PCM */
#define WAKE_POLL_MS         100

/* Singleton. The wake task owns the detector instance and creates/destroys it
 * according to `desired`, so no other task touches WakeNet state. `desired` and
 * `listening` are aligned words accessed with __atomic builtins. */
static struct {
    bool available;
    app_wake_cb_t on_wake;
    void *ctx;
    srmodel_list_t *models;
    char *model_name;
    const esp_wn_iface_t *wn;
    model_iface_data_t *detector;
    int chunk_samples;
    int16_t *chunk;
    StreamBufferHandle_t stream;
    StaticStreamBuffer_t stream_struct;
    uint8_t *stream_storage;
    uint32_t desired;     /* requested by the session */
    uint32_t listening;   /* detector exists and accepts audio */
} s_wake;

static void destroy_detector(void)
{
    __atomic_store_n(&s_wake.listening, 0U, __ATOMIC_RELEASE);
    if (s_wake.detector) {
        s_wake.wn->destroy(s_wake.detector);
        s_wake.detector = NULL;
    }
    heap_caps_free(s_wake.chunk);
    s_wake.chunk = NULL;
    xStreamBufferReset(s_wake.stream);
}

static bool create_detector(void)
{
    s_wake.detector = s_wake.wn->create(s_wake.model_name, DET_MODE_90);
    if (!s_wake.detector) {
        ESP_LOGE(TAG, "WakeNet create failed");
        return false;
    }
    s_wake.chunk_samples = s_wake.wn->get_samp_chunksize(s_wake.detector);
    s_wake.chunk = heap_caps_malloc((size_t)s_wake.chunk_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wake.chunk || s_wake.chunk_samples <= 0) {
        destroy_detector();
        return false;
    }
    xStreamBufferReset(s_wake.stream);
    __atomic_store_n(&s_wake.listening, 1U, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "listening for \"Hola ESP\" (%s, %d samples/chunk, %d Hz)", s_wake.model_name,
             s_wake.chunk_samples, s_wake.wn->get_samp_rate(s_wake.detector));
    return true;
}

static void wake_task(void *arg)
{
    (void)arg;
    for (;;) {
        const bool desired = __atomic_load_n(&s_wake.desired, __ATOMIC_ACQUIRE) != 0;
        if (desired && !s_wake.detector) {
            if (!create_detector()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        } else if (!desired && s_wake.detector) {
            destroy_detector();
            ESP_LOGI(TAG, "local listening paused");
        }
        if (!s_wake.detector) {
            vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_MS));
            continue;
        }
        const size_t want = (size_t)s_wake.chunk_samples * sizeof(int16_t);
        size_t got = 0;
        while (got < want) {
            const size_t n = xStreamBufferReceive(s_wake.stream, (uint8_t *)s_wake.chunk + got,
                                                  want - got, pdMS_TO_TICKS(WAKE_POLL_MS));
            if (n == 0) {
                break; /* re-check the desired state */
            }
            got += n;
        }
        if (got < want || !__atomic_load_n(&s_wake.desired, __ATOMIC_ACQUIRE)) {
            continue;
        }
        if (s_wake.wn->detect(s_wake.detector, s_wake.chunk) == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected");
            /* Stop listening at once; the session re-arms it when idle again. */
            __atomic_store_n(&s_wake.desired, 0U, __ATOMIC_RELEASE);
            destroy_detector();
            if (s_wake.on_wake) {
                s_wake.on_wake(s_wake.ctx);
            }
        }
    }
}

esp_err_t app_wake_init(app_wake_cb_t on_wake, void *ctx)
{
    if (s_wake.available) {
        return ESP_ERR_INVALID_STATE;
    }
    s_wake.on_wake = on_wake;
    s_wake.ctx = ctx;
    s_wake.models = esp_srmodel_init(WAKE_PARTITION);
    if (!s_wake.models) {
        ESP_LOGW(TAG, "no \"%s\" partition or model image; wake word disabled", WAKE_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }
    s_wake.model_name = esp_srmodel_filter(s_wake.models, ESP_WN_PREFIX, WAKE_KEYWORD);
    s_wake.wn = s_wake.model_name ? esp_wn_handle_from_name(s_wake.model_name) : NULL;
    if (!s_wake.wn) {
        ESP_LOGW(TAG, "WakeNet model \"%s\" not found; wake word disabled", WAKE_KEYWORD);
        esp_srmodel_deinit(s_wake.models);
        s_wake.models = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    s_wake.stream_storage = heap_caps_malloc(WAKE_BUFFER_BYTES + 1,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wake.stream_storage) {
        return ESP_ERR_NO_MEM;
    }
    s_wake.stream = xStreamBufferCreateStatic(WAKE_BUFFER_BYTES, 1, s_wake.stream_storage,
                                              &s_wake.stream_struct);
    if (!s_wake.stream ||
            xTaskCreatePinnedToCore(wake_task, "vox_wake", WAKE_TASK_STACK, NULL, WAKE_TASK_PRIO,
                                    NULL, WAKE_TASK_CORE) != pdPASS) {
        heap_caps_free(s_wake.stream_storage);
        s_wake.stream_storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_wake.available = true;
    ESP_LOGI(TAG, "wake word ready: %s", s_wake.model_name);
    return ESP_OK;
}

bool app_wake_available(void)
{
    return s_wake.available;
}

void app_wake_set_active(bool active)
{
    if (s_wake.available) {
        __atomic_store_n(&s_wake.desired, active ? 1U : 0U, __ATOMIC_RELEASE);
    }
}

void app_wake_feed(const int16_t *pcm, size_t samples)
{
    if (!s_wake.available || !pcm || !__atomic_load_n(&s_wake.listening, __ATOMIC_ACQUIRE)) {
        return;
    }
    /* Single writer (capture task); drops when the detector falls behind. */
    (void)xStreamBufferSend(s_wake.stream, pcm, samples * sizeof(int16_t), 0);
}

const char *app_wake_model_name(void)
{
    return (s_wake.available && s_wake.model_name) ? s_wake.model_name : "";
}
