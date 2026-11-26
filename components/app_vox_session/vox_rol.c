// SPDX-License-Identifier: Apache-2.0
#include "vox_rol.h"

#include <stdio.h>
#include <string.h>

/* Genders as listed by Google Cloud (docs.cloud.google.com/text-to-speech/docs/chirp3-hd);
 * styles from the Gemini speech-generation voice table. */
static const vox_rol_voice_t VOICES[] = {
    { "Charon",  "CHICO" },
    { "Algenib", "CHICO GRAVE" },
    { "Kore",    "CHICA FIRME" },
    { "Sulafat", "CHICA CÁLIDA" },
};

size_t vox_rol_voice_count(void)
{
    return sizeof(VOICES) / sizeof(VOICES[0]);
}

const vox_rol_voice_t *vox_rol_voice_at(size_t i)
{
    return i < vox_rol_voice_count() ? &VOICES[i] : NULL;
}

int vox_rol_voice_index(const char *name)
{
    if (!name) {
        return -1;
    }
    for (size_t i = 0; i < vox_rol_voice_count(); ++i) {
        if (strcmp(VOICES[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const vox_rol_voice_t *vox_rol_voice_next(const char *name)
{
    const int i = vox_rol_voice_index(name);
    return &VOICES[i < 0 ? 0 : ((size_t)i + 1U) % vox_rol_voice_count()];
}

unsigned vox_rol_die(vox_rol_rand32_fn rand32, void *ctx, unsigned sides)
{
    if (sides <= 1U || !rand32) {
        return 1U;
    }
    /* Reject the top partial bucket so every face has the same probability. */
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % sides);
    uint32_t r;
    do {
        r = rand32(ctx);
    } while (r >= limit);
    return 1U + (unsigned)(r % sides);
}

void vox_rol_clamp(int *count, int *sides)
{
    if (count) {
        *count = *count < 1 ? 1 : (*count > VOX_ROL_MAX_DICE ? VOX_ROL_MAX_DICE : *count);
    }
    if (sides) {
        *sides = *sides < VOX_ROL_MIN_SIDES ? 20
                 : (*sides > VOX_ROL_MAX_SIDES ? VOX_ROL_MAX_SIDES : *sides);
    }
}

static unsigned total_of(const uint16_t *values, unsigned count)
{
    unsigned total = 0;
    for (unsigned i = 0; i < count; ++i) {
        total += values[i];
    }
    return total;
}

size_t vox_rol_roll_json(const uint16_t *values, unsigned count, unsigned sides, char *out,
                         size_t cap)
{
    if (!values || !out || cap == 0 || count == 0) {
        return 0;
    }
    bool crit = false;
    bool fumble = false;
    char list[48] = "";
    size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
        const int n = snprintf(list + used, sizeof(list) - used, "%s%u", i ? "," : "",
                               (unsigned)values[i]);
        if (n < 0 || (size_t)n >= sizeof(list) - used) {
            return 0;
        }
        used += (size_t)n;
        crit = crit || (sides == 20U && values[i] == 20U);
        fumble = fumble || (sides == 20U && values[i] == 1U);
    }
    const int n = snprintf(out, cap,
                           "{\"tirada\":\"%ud%u\",\"resultados\":[%s],\"total\":%u,"
                           "\"critico\":%s,\"pifia\":%s,\"mostrado_en_pantalla\":true}",
                           count, sides, list, total_of(values, count), crit ? "true" : "false",
                           fumble ? "true" : "false");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t vox_rol_player_text(const uint16_t *values, unsigned count, unsigned sides, char *out,
                           size_t cap)
{
    if (!values || !out || cap == 0 || count == 0) {
        return 0;
    }
    char list[64] = "";
    size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
        const char *sep = i == 0 ? "" : (i + 1 == count ? " y " : ", ");
        const int n = snprintf(list + used, sizeof(list) - used, "%s%u", sep, (unsigned)values[i]);
        if (n < 0 || (size_t)n >= sizeof(list) - used) {
            return 0;
        }
        used += (size_t)n;
    }
    int n;
    if (count == 1) {
        n = snprintf(out, cap, "Tirada del jugador con los dados de VOX: 1d%u, resultado %s.",
                     sides, list);
    } else {
        n = snprintf(out, cap,
                     "Tirada del jugador con los dados de VOX: %ud%u, resultados %s, total %u.",
                     count, sides, list, total_of(values, count));
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
