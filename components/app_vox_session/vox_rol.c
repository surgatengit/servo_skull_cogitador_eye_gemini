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

vox_rol_spec_t vox_rol_spec_default(void)
{
    const vox_rol_spec_t spec = { VOX_ROL_SUM, 2, 20, 0 };
    return spec;
}

static bool word_is(const char *text, const char *word)
{
    size_t i = 0;
    for (; text[i] && word[i]; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != word[i]) {
            return false;
        }
    }
    return text[i] == '\0' && word[i] == '\0';
}

vox_rol_mode_t vox_rol_mode_parse(const char *text)
{
    if (!text) {
        return VOX_ROL_SUM;
    }
    if (word_is(text, "porcentaje") || word_is(text, "porcentual") || word_is(text, "d100")) {
        return VOX_ROL_PERCENT;
    }
    if (word_is(text, "reserva")) {
        return VOX_ROL_POOL;
    }
    return VOX_ROL_SUM;
}

vox_rol_spec_t vox_rol_spec_make(vox_rol_mode_t mode, int count, int sides, int target)
{
    vox_rol_spec_t spec = { (uint8_t)mode, 2, 10, 0 };
    if (mode == VOX_ROL_PERCENT) {
        return spec;
    }
    if (mode != VOX_ROL_POOL) {
        spec.mode = VOX_ROL_SUM;
    }
    const int fallback = spec.mode == VOX_ROL_POOL ? 10 : 20;
    count = count < 1 ? 1 : (count > VOX_ROL_MAX_DICE ? VOX_ROL_MAX_DICE : count);
    sides = sides < VOX_ROL_MIN_SIDES ? fallback
            : (sides > VOX_ROL_MAX_SIDES ? VOX_ROL_MAX_SIDES : sides);
    spec.count = (uint8_t)count;
    spec.sides = (uint8_t)sides;
    if (spec.mode == VOX_ROL_POOL) {
        spec.target = (uint8_t)((target >= 2 && target <= sides) ? target : sides - sides / 5);
    }
    return spec;
}

void vox_rol_roll(const vox_rol_spec_t *spec, vox_rol_rand32_fn rand32, void *ctx,
                  uint16_t *values)
{
    for (unsigned i = 0; i < spec->count; ++i) {
        values[i] = (uint16_t)vox_rol_die(rand32, ctx, spec->sides);
    }
}

static unsigned total_of(const vox_rol_spec_t *spec, const uint16_t *values)
{
    unsigned total = 0;
    for (unsigned i = 0; i < spec->count; ++i) {
        total += values[i];
    }
    return total;
}

static unsigned count_equal(const vox_rol_spec_t *spec, const uint16_t *values, unsigned v)
{
    unsigned n = 0;
    for (unsigned i = 0; i < spec->count; ++i) {
        n += values[i] == v ? 1U : 0U;
    }
    return n;
}

unsigned vox_rol_percent(const uint16_t *values)
{
    const unsigned value = (values[0] % 10U) * 10U + values[1] % 10U;
    return value == 0U ? 100U : value;
}

unsigned vox_rol_successes(const vox_rol_spec_t *spec, const uint16_t *values)
{
    unsigned n = 0;
    for (unsigned i = 0; i < spec->count; ++i) {
        n += values[i] >= spec->target ? 1U : 0U;
    }
    return n;
}

/* "14, 7 y 3" (prose) or "14,7,3" (JSON). Returns false when it does not fit. */
static bool list_values(const vox_rol_spec_t *spec, const uint16_t *values, bool prose,
                        char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < spec->count; ++i) {
        const char *sep = i == 0 ? "" : (!prose ? "," : (i + 1U == spec->count ? " y " : ", "));
        const int n = snprintf(out + used, cap - used, "%s%u", sep, (unsigned)values[i]);
        if (n < 0 || (size_t)n >= cap - used) {
            return false;
        }
        used += (size_t)n;
    }
    return true;
}

static size_t finish(int n, size_t cap)
{
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

void vox_rol_view(const vox_rol_spec_t *spec, const uint16_t *values, vox_rol_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->count = spec->count;
    out->tumble = spec->sides;
    if (spec->mode == VOX_ROL_PERCENT) {
        snprintf(out->faces[0], sizeof(out->faces[0]), "%u0", (unsigned)(values[0] % 10U));
        snprintf(out->faces[1], sizeof(out->faces[1]), "%u", (unsigned)(values[1] % 10U));
        snprintf(out->title, sizeof(out->title), "TIRADA D%%");
        snprintf(out->result, sizeof(out->result), "RESULTADO %u", vox_rol_percent(values));
        return;
    }
    bool any_max = false;
    bool any_one = false;
    for (unsigned i = 0; i < spec->count; ++i) {
        const unsigned v = values[i];
        snprintf(out->faces[i], sizeof(out->faces[i]), "%u", v % 1000U); /* <= 100 */
        const bool good = spec->mode == VOX_ROL_POOL ? v >= spec->target : v == spec->sides;
        out->marks[i] = good ? VOX_ROL_MARK_GOOD
                             : (v == 1U ? VOX_ROL_MARK_BAD : VOX_ROL_MARK_NONE);
        any_max = any_max || v == spec->sides;
        any_one = any_one || v == 1U;
    }
    if (spec->mode == VOX_ROL_POOL) {
        const unsigned ok = vox_rol_successes(spec, values);
        snprintf(out->title, sizeof(out->title), "RESERVA %uD%u · %u+", (unsigned)spec->count,
                 (unsigned)spec->sides, (unsigned)spec->target);
        if (ok == 0U) {
            snprintf(out->result, sizeof(out->result), "NINGÚN ÉXITO");
        } else {
            snprintf(out->result, sizeof(out->result), "%u %s", ok,
                     ok == 1U ? "ÉXITO" : "ÉXITOS");
        }
        return;
    }
    snprintf(out->title, sizeof(out->title), "TIRADA %uD%u", (unsigned)spec->count,
             (unsigned)spec->sides);
    const char *mark = "";
    if (spec->sides == 20U && any_max) {
        mark = "CRÍTICO";
    } else if (spec->sides == 20U && any_one) {
        mark = "PIFIA";
    }
    if (spec->count > 1U) {
        snprintf(out->result, sizeof(out->result), "TOTAL %u%s%s", total_of(spec, values),
                 mark[0] ? " · " : "", mark);
    } else {
        snprintf(out->result, sizeof(out->result), "%s", mark[0] ? mark : " ");
    }
}

size_t vox_rol_badge(const vox_rol_spec_t *spec, char *out, size_t cap)
{
    if (!spec || !out || cap == 0) {
        return 0;
    }
    if (spec->mode == VOX_ROL_PERCENT) {
        return finish(snprintf(out, cap, "D%%"), cap);
    }
    if (spec->count == 1U) {
        return finish(snprintf(out, cap, "D%u", (unsigned)spec->sides), cap);
    }
    return finish(snprintf(out, cap, "%uD%u", (unsigned)spec->count, (unsigned)spec->sides),
                  cap);
}

size_t vox_rol_spec_text(const vox_rol_spec_t *spec, char *out, size_t cap)
{
    if (!spec || !out || cap == 0) {
        return 0;
    }
    switch (spec->mode) {
    case VOX_ROL_PERCENT:
        return finish(snprintf(out, cap, "d100 (dos d10: decenas y unidades)"), cap);
    case VOX_ROL_POOL:
        return finish(snprintf(out, cap, "%ud%u de reserva, éxito con %u o más",
                               (unsigned)spec->count, (unsigned)spec->sides,
                               (unsigned)spec->target), cap);
    default:
        return finish(snprintf(out, cap, "%ud%u%s", (unsigned)spec->count,
                               (unsigned)spec->sides, spec->count > 1U ? " sumados" : ""),
                      cap);
    }
}

size_t vox_rol_roll_json(const vox_rol_spec_t *spec, const uint16_t *values, char *out,
                         size_t cap)
{
    if (!spec || !values || !out || cap == 0 || spec->count == 0) {
        return 0;
    }
    if (spec->mode == VOX_ROL_PERCENT) {
        return finish(snprintf(out, cap,
                               "{\"tirada\":\"d100\",\"modo\":\"porcentaje\",\"decenas\":%u,"
                               "\"unidades\":%u,\"resultado\":%u,\"mostrado_en_pantalla\":true}",
                               (unsigned)(values[0] % 10U), (unsigned)(values[1] % 10U),
                               vox_rol_percent(values)), cap);
    }
    char list[48];
    if (!list_values(spec, values, false, list, sizeof(list))) {
        return 0;
    }
    if (spec->mode == VOX_ROL_POOL) {
        return finish(snprintf(out, cap,
                               "{\"tirada\":\"%ud%u\",\"modo\":\"reserva\",\"exito_con\":%u,"
                               "\"resultados\":[%s],\"exitos\":%u,\"unos\":%u,\"maximos\":%u,"
                               "\"mostrado_en_pantalla\":true}",
                               (unsigned)spec->count, (unsigned)spec->sides,
                               (unsigned)spec->target, list, vox_rol_successes(spec, values),
                               count_equal(spec, values, 1U),
                               count_equal(spec, values, spec->sides)), cap);
    }
    const bool d20 = spec->sides == 20U;
    return finish(snprintf(out, cap,
                           "{\"tirada\":\"%ud%u\",\"modo\":\"suma\",\"resultados\":[%s],"
                           "\"total\":%u,\"critico\":%s,\"pifia\":%s,"
                           "\"mostrado_en_pantalla\":true}",
                           (unsigned)spec->count, (unsigned)spec->sides, list,
                           total_of(spec, values),
                           d20 && count_equal(spec, values, 20U) ? "true" : "false",
                           d20 && count_equal(spec, values, 1U) ? "true" : "false"), cap);
}

size_t vox_rol_player_text(const vox_rol_spec_t *spec, const uint16_t *values, char *out,
                           size_t cap)
{
    if (!spec || !values || !out || cap == 0 || spec->count == 0) {
        return 0;
    }
    if (spec->mode == VOX_ROL_PERCENT) {
        return finish(snprintf(out, cap,
                               "Tirada porcentual del jugador con los dados de Co: decenas %u, "
                               "unidades %u, resultado %u.",
                               (unsigned)(values[0] % 10U), (unsigned)(values[1] % 10U),
                               vox_rol_percent(values)), cap);
    }
    char list[64];
    if (!list_values(spec, values, true, list, sizeof(list))) {
        return 0;
    }
    const char *noun = spec->count == 1U ? "resultado" : "resultados";
    if (spec->mode == VOX_ROL_POOL) {
        const unsigned ok = vox_rol_successes(spec, values);
        const unsigned ones = count_equal(spec, values, 1U);
        const unsigned tops = count_equal(spec, values, spec->sides);
        return finish(snprintf(out, cap,
                               "Tirada de reserva del jugador con los dados de Co: %ud%u, "
                               "éxito con %u o más; %s %s; %u %s, %u %s y %u %s (%u).",
                               (unsigned)spec->count, (unsigned)spec->sides,
                               (unsigned)spec->target, noun, list,
                               ok, ok == 1U ? "éxito" : "éxitos",
                               ones, ones == 1U ? "uno" : "unos",
                               tops, tops == 1U ? "máximo" : "máximos",
                               (unsigned)spec->sides), cap);
    }
    if (spec->count == 1U) {
        return finish(snprintf(out, cap, "Tirada del jugador con los dados de Co: 1d%u, "
                               "resultado %s.", (unsigned)spec->sides, list), cap);
    }
    return finish(snprintf(out, cap,
                           "Tirada del jugador con los dados de Co: %ud%u, resultados %s, "
                           "total %u.", (unsigned)spec->count, (unsigned)spec->sides, list,
                           total_of(spec, values)), cap);
}

