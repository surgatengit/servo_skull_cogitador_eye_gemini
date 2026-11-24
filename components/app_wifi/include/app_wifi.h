// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WIFI_UNCONFIGURED = 0,  /**< No shared Wi-Fi credentials in Vibe Mode. */
    APP_WIFI_CONNECTING,        /**< Associating or waiting for DHCP. */
    APP_WIFI_CONNECTED,         /**< Station has an IPv4 address. */
    APP_WIFI_FAILED,            /**< Driver or configuration error; see logs. */
} app_wifi_state_t;

/** Called from the default event loop task on every state change. Must not block. */
typedef void (*app_wifi_state_cb_t)(app_wifi_state_t state, void *ctx);

/** Start the station once (singleton). Reads credentials with app_config_load_wifi(),
 * applies them with WIFI_STORAGE_RAM and reconnects with exponential backoff.
 * Requires nvs_flash_init(). cb may be NULL. */
esp_err_t app_wifi_start(app_wifi_state_cb_t cb, void *ctx);

/** Latest state; callable from any task. */
app_wifi_state_t app_wifi_get_state(void);

#ifdef __cplusplus
}
#endif
