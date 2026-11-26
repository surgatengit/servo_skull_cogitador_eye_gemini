// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Saved role-playing game (1.6.0): one JSON document, kept on VOX and offered
 * for download/upload on the phone page. Pure C + cJSON, host-tested.
 *
 * {"vox_partida":1,"titulo":"…","juego":"…","resumen":"…",
 *  "dados":{"modo":"reserva","cantidad":6,"caras":10,"exito_con":8},
 *  "jugadores":[{"nombre":"Ana","ficha":"…","secreto":"…"}]}
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROL_GAME_VERSION      1
#define ROL_GAME_MAX_BYTES    (16 * 1024)
#define ROL_GAME_TEXT_CAP     80
#define ROL_GAME_SUMMARY_CAP  6000

typedef struct {
    char title[ROL_GAME_TEXT_CAP];   /**< "La cripta de Vael" */
    char game[ROL_GAME_TEXT_CAP];    /**< system or setting, "fantasía con d20" */
    char dice_mode[16];              /**< "suma", "porcentaje" or "reserva" */
    int dice_count;
    int dice_sides;
    int dice_target;
} rol_game_meta_t;

struct rol_players;

/**
 * Build the document from meta, the master's summary and the players (name,
 * sheet, secret). Returns a cJSON-allocated string (free with cJSON_free), or
 * NULL when it would exceed ROL_GAME_MAX_BYTES or memory runs out.
 */
char *rol_game_build(const rol_game_meta_t *meta, const char *summary,
                     const struct rol_players *players);

/** True when json[0..len) is a VOX saved game of a known version within the size limit. */
bool rol_game_valid(const char *json, size_t len);

/**
 * Read meta and put the saved players into the table: every sheet, and the
 * secrets when with_secrets. Players already on a phone keep it; the others
 * are reserved for when they join. Returns false for an invalid document.
 */
bool rol_game_restore(const char *json, rol_game_meta_t *meta, struct rol_players *players,
                      bool with_secrets);

#ifdef __cplusplus
}
#endif
