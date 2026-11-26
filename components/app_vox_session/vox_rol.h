// SPDX-License-Identifier: Apache-2.0
#pragma once

/* RoleMaster helpers (1.6.0): voice table, fair dice and roll text.
 * Pure C, host-tested (tools/host_tests). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOX_ROL_MAX_DICE   4
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

/** Clamp a requested roll to 1..VOX_ROL_MAX_DICE dice of 2..100 sides. */
void vox_rol_clamp(int *count, int *sides);

/**
 * JSON object answering tirar_dados, e.g.
 * {"tirada":"2d20","resultados":[14,7],"total":21,"critico":false,"pifia":false}
 * critico/pifia are reported for d20 (any 20 / any 1). Returns the length, or
 * 0 if cap is too small.
 */
size_t vox_rol_roll_json(const uint16_t *values, unsigned count, unsigned sides, char *out,
                         size_t cap);

/**
 * Spanish text sent to the model for a roll made with the on-screen button,
 * e.g. "Tirada del jugador con los dados de VOX: 2d20, resultados 14 y 7, total 21."
 * Returns the length, or 0 if cap is too small.
 */
size_t vox_rol_player_text(const uint16_t *values, unsigned count, unsigned sides, char *out,
                           size_t cap);

#ifdef __cplusplus
}
#endif
