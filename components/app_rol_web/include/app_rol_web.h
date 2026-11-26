// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Role-mode phone page (1.6.0): a tiny HTTP server on the home Wi-Fi where each
 * player registers by name and later reads only their own secret. It exists
 * only while role mode is on; contracts in ../README.md. Session task only.
 */

/** Start the server on port 80 with an empty player table (no-op if running). */
esp_err_t app_rol_web_start(void);

/** Stop the server and forget players and secrets (no-op if stopped). */
void app_rol_web_stop(void);

/** True while the server runs. */
bool app_rol_web_running(void);

/** "http://192.168.1.36" of the station interface; ESP_ERR_INVALID_STATE without an IP. */
esp_err_t app_rol_web_address(char *out, size_t cap);

/** Hand `secret` to the phone registered as `player` (name match ignores case and
 * accents). False when no phone joined with that name. */
bool app_rol_web_deliver(const char *player, const char *secret);

/** Comma-separated names of the registered players ("" if none). */
size_t app_rol_web_players(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
