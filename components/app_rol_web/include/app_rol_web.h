// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "rol_game.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Role-mode phone page (1.6.0): a tiny HTTP server on the home Wi-Fi where each
 * player registers by name and then sees only their own secrets and character
 * sheet; it also offers the saved game for download/upload. The server exists
 * only while role mode is on; the saved game lives in NVS. Contracts in
 * ../README.md. Session task only.
 */

/** Start the server on port 80 with an empty player table (no-op if running). */
esp_err_t app_rol_web_start(void);

/** Stop the server and forget players, secrets and sheets (no-op if stopped). */
void app_rol_web_stop(void);

/** True while the server runs. */
bool app_rol_web_running(void);

/** "http://192.168.1.36" of the station interface; ESP_ERR_INVALID_STATE without an IP. */
esp_err_t app_rol_web_address(char *out, size_t cap);

typedef enum {
    APP_ROL_WEB_PHONE = 0,   /**< on the player's phone now */
    APP_ROL_WEB_PENDING,     /**< kept for the name: shown when that player joins */
    APP_ROL_WEB_FAILED,      /**< server off, bad name or 6 players already */
} app_rol_web_put_t;

/** Secret for `player` (name match ignores case and accents). */
app_rol_web_put_t app_rol_web_deliver(const char *player, const char *secret);

/** Character sheet for `player` (replaces the previous one). */
app_rol_web_put_t app_rol_web_set_sheet(const char *player, const char *sheet);

/** Comma-separated names of the players with a phone ("" if none). */
size_t app_rol_web_players(char *out, size_t cap);

/**
 * Player status for the master as a JSON array (cJSON_free the result):
 * [{"nombre":"Ana","movil":true,"secreto_sin_leer":false,"ficha":true}]. NULL if off.
 */
char *app_rol_web_status_json(void);

/** Move the pending notices ("Ana se ha conectado…", one sentence each) to out. */
size_t app_rol_web_take_notices(char *out, size_t cap);

/** Saved game from the current players plus meta and summary (cJSON_free it). */
char *app_rol_web_build_game(const rol_game_meta_t *meta, const char *summary);

/** Keep `json` (a valid saved game) as the saved game on VOX (NVS). */
esp_err_t app_rol_web_store_game(const char *json);

/** The saved game (heap string, free() it), or NULL when there is none. */
char *app_rol_web_load_game(void);

/** Put a saved game's players into the table (see rol_game_restore). False if off or invalid. */
bool app_rol_web_restore_game(const char *json, rol_game_meta_t *meta, bool with_secrets);

#ifdef __cplusplus
}
#endif
