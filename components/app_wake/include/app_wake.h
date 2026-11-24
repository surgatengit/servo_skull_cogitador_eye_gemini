// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Wake word detected. Runs on the wake task: return quickly, never block. */
typedef void (*app_wake_cb_t)(void *ctx);

/** Load the WakeNet model list from the "model" partition and start the wake task
 * (singleton). Returns ESP_ERR_NOT_FOUND when the partition or the configured
 * model is missing; the caller keeps working with touch only. */
esp_err_t app_wake_init(app_wake_cb_t on_wake, void *ctx);

/** True after a successful app_wake_init(). */
bool app_wake_available(void);

/** Request local listening on/off. The detector instance (internal RAM) exists
 * only while active, so it never competes with a Gemini session. Any task. */
void app_wake_set_active(bool active);

/** Feed mono 16 kHz PCM from the capture task while active. Never blocks;
 * drops audio when the internal buffer is full or listening is off. */
void app_wake_feed(const int16_t *pcm, size_t samples);

/** Model name in use (e.g. "wn10_es_holaesp"), or "" when unavailable. */
const char *app_wake_model_name(void);

#ifdef __cplusplus
}
#endif
