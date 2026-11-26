// SPDX-License-Identifier: Apache-2.0
#pragma once

/* RoleMaster helpers (1.6.0): voice table, fair dice, roll modes and roll text.
 * Pure C, host-tested (tools/host_tests). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOX_ROL_MAX_DICE   10
#define VOX_ROL_MIN_SIDES  2
#define VOX_ROL_MAX_SIDES  100

/** Voices offered on screen and to the model (Google Cloud Chirp 3 HD genders). */
typedef struct {
    const char *name;   /**< Gemini prebuilt voice name */
    const char *label;  /**< status text (<= 12 chars), e.g. "CHICA FIRME" */
} vox_rol_voice_t;

/** Number of voices in the on-screen cycle. */
size_t vox_rol_voice_count(void);

/** Voice i of the cycle (NULL when out of range). */
const vox_rol_voice_t *vox_rol_voice_at(size_t i);

/** Index of `name` in the cycle, or -1 when it is not one of them. */
int vox_rol_voice_index(const char *name);

/** Next voice in the cycle after `name` (the first one when `name` is unknown). */
const vox_rol_voice_t *vox_rol_voice_next(const char *name);

/** Random source: 32 uniformly distributed bits per call. */
typedef uint32_t (*vox_rol_rand32_fn)(void *ctx);

/** Unbiased value in 1..sides (rejection sampling); sides >= 1. */
unsigned vox_rol_die(vox_rol_rand32_fn rand32, void *ctx, unsigned sides);

/** How a roll is read. */
typedef enum {
    VOX_ROL_SUM = 0,   /**< add the dice (2d20, 3d6...) */
    VOX_ROL_PERCENT,   /**< 2d10 as d100: tens and units, 0 and 0 = 100 */
    VOX_ROL_POOL,      /**< count the dice >= target (one die per skill point) */
} vox_rol_mode_t;

typedef struct {
    uint8_t mode;      /**< vox_rol_mode_t */
    uint8_t count;     /**< 1..VOX_ROL_MAX_DICE (always 2 for PERCENT) */
    uint8_t sides;     /**< 2..100 (always 10 for PERCENT) */
    uint8_t target;    /**< POOL: success on this value or more; 0 otherwise */
} vox_rol_spec_t;

/** The player's default roll: 2d20 added up. */
vox_rol_spec_t vox_rol_spec_default(void);

/** Mode from the tool argument ("suma", "porcentaje", "reserva"); NULL/unknown = SUM. */
vox_rol_mode_t vox_rol_mode_parse(const char *text);

/**
 * Build a valid spec from model arguments: count clamped to 1..10, sides to 2..100
 * (unknown -> 20 for SUM, 10 for POOL), PERCENT forced to 2d10, POOL target to
 * 2..sides (0 or out of range -> sides - sides / 5: 8 on a d10, 5 on a d6).
 */
vox_rol_spec_t vox_rol_spec_make(vox_rol_mode_t mode, int count, int sides, int target);

/** Roll spec->count dice (1..sides each) into values[0..count-1]. */
void vox_rol_roll(const vox_rol_spec_t *spec, vox_rol_rand32_fn rand32, void *ctx,
                  uint16_t *values);

/** Die highlight on screen. */
typedef enum {
    VOX_ROL_MARK_NONE = 0,
    VOX_ROL_MARK_GOOD,   /**< gold: maximum (SUM) or success (POOL) */
    VOX_ROL_MARK_BAD,    /**< red: a 1 (SUM, POOL) */
} vox_rol_mark_t;

/** What the dice panel shows for a roll (uppercase card charset). */
typedef struct {
    unsigned count;
    char faces[VOX_ROL_MAX_DICE][4];   /**< final text per die, e.g. "17", "40", "7" */
    uint8_t marks[VOX_ROL_MAX_DICE];   /**< vox_rol_mark_t */
    unsigned tumble;                   /**< random 1..tumble while the dice roll */
    char title[32];                    /**< "TIRADA 2D20", "TIRADA D%", "RESERVA 6D10 · 8+" */
    char result[40];                   /**< "TOTAL 29 · CRÍTICO", "RESULTADO 47", "3 ÉXITOS" */
} vox_rol_view_t;

/** Fill the dice panel contents for spec + values. */
void vox_rol_view(const vox_rol_spec_t *spec, const uint16_t *values, vox_rol_view_t *out);

/** Percentile result of a PERCENT roll (values are two d10: 10 counts as 0). */
unsigned vox_rol_percent(const uint16_t *values);

/** Successes of a POOL roll. */
unsigned vox_rol_successes(const vox_rol_spec_t *spec, const uint16_t *values);

/** Role badge text: "2D20", "D%", "6D10". Returns the length (0 if cap too small). */
size_t vox_rol_badge(const vox_rol_spec_t *spec, char *out, size_t cap);

/** Spanish description for the model, e.g. "6d10 de reserva, éxito con 8 o más". */
size_t vox_rol_spec_text(const vox_rol_spec_t *spec, char *out, size_t cap);

/**
 * JSON object answering tirar_dados, e.g.
 * {"tirada":"2d20","modo":"suma","resultados":[14,7],"total":21,"critico":false,
 *  "pifia":false,"mostrado_en_pantalla":true}
 * critico/pifia are reported for d20 sums (any 20 / any 1). Returns the length,
 * or 0 if cap is too small.
 */
size_t vox_rol_roll_json(const vox_rol_spec_t *spec, const uint16_t *values, char *out,
                         size_t cap);

/**
 * Spanish text sent to the model for a roll made with the on-screen button, e.g.
 * "Tirada del jugador con los dados de Co: 2d20, resultados 14 y 7, total 21."
 * Returns the length, or 0 if cap is too small.
 */
size_t vox_rol_player_text(const vox_rol_spec_t *spec, const uint16_t *values, char *out,
                           size_t cap);

#ifdef __cplusplus
}
#endif
