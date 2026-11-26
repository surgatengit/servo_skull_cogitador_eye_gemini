// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Buffer sizes including the trailing NUL. */
#define APP_SECRETS_API_KEY_CAPACITY 129   /* key: 30..128 printable ASCII bytes */
#define APP_SECRETS_MODEL_CAPACITY   64
#define APP_SECRETS_VOICE_CAPACITY   24

/** ESP-Iris RPC service used for private provisioning ("VX"). */
#define APP_SECRETS_RPC_SERVICE        0x5658U
#define APP_SECRETS_RPC_SET_API_KEY    1U
#define APP_SECRETS_RPC_CLEAR_API_KEY  2U
#define APP_SECRETS_RPC_STATUS         3U
#define APP_SECRETS_RPC_SET_MODEL      4U
#define APP_SECRETS_RPC_SET_VOICE      5U

/** Notified (from the ESP-Iris service task) after the key, model or voice changes.
 * Must not block; post to an application task instead. */
typedef void (*app_secrets_changed_cb_t)(void *ctx);

/** Register the provisioning RPC. Call once at startup after nvs_flash_init()
 * and esp_iris startup; on_changed may be NULL. */
esp_err_t app_secrets_init(app_secrets_changed_cb_t on_changed, void *ctx);

/** Copy the stored API key into out (capacity >= APP_SECRETS_API_KEY_CAPACITY).
 * Returns ESP_ERR_NVS_NOT_FOUND when unprovisioned. On any error out is cleared.
 * The caller must zeroize out after use and never log it. */
esp_err_t app_secrets_load_api_key(char *out, size_t capacity);

/** True when a syntactically valid key is stored (never reveals the value). */
bool app_secrets_has_api_key(void);

/** Copy the optional model override; ESP_ERR_NVS_NOT_FOUND when unset. */
esp_err_t app_secrets_load_model(char *out, size_t capacity);

/** Copy the stored prebuilt voice (1.6.0); ESP_ERR_NVS_NOT_FOUND when unset. */
esp_err_t app_secrets_load_voice(char *out, size_t capacity);

/** Store a prebuilt voice (NULL or "" clears it). Flash write: call from a
 * task with an internal-RAM stack. */
esp_err_t app_secrets_store_voice(const char *voice);

/** Validators shared with the RPC handler (exposed for tests). */
bool app_secrets_key_is_valid(const char *key, size_t len);
bool app_secrets_model_is_valid(const char *model, size_t len);
/** One of the 30 Gemini Live prebuilt voices (exact, case-sensitive name). */
bool app_secrets_voice_is_valid(const char *voice, size_t len);

#ifdef __cplusplus
}
#endif
