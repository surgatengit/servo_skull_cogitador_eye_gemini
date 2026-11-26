// SPDX-License-Identifier: Apache-2.0
/* PC tests for the 1.6.0 RoleMaster helpers: voices, dice and roll modes, roll text,
 * secret card, phone players. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rol_players.h"
#include "secret_text.h"
#include "vox_rol.h"

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            failures++;                                                             \
        }                                                                           \
    } while (0)

static uint32_t lcg_state = 12345u;

static uint32_t lcg(void *ctx)
{
    (void)ctx;
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

static uint32_t seq_values[8];
static int seq_pos;

static uint32_t scripted(void *ctx)
{
    (void)ctx;
    return seq_values[seq_pos++];
}

static void test_voices(void)
{
    CHECK(vox_rol_voice_count() == 4);
    CHECK(vox_rol_voice_index("Charon") == 0);
    CHECK(vox_rol_voice_index("Sulafat") == 3);
    CHECK(vox_rol_voice_index("charon") == -1);
    CHECK(vox_rol_voice_index(NULL) == -1);
    CHECK(strcmp(vox_rol_voice_next("Charon")->name, "Algenib") == 0);
    CHECK(strcmp(vox_rol_voice_next("Sulafat")->name, "Charon") == 0);   /* wraps */
    CHECK(strcmp(vox_rol_voice_next("Puck")->name, "Charon") == 0);      /* USB-set voice */
    for (size_t i = 0; i < vox_rol_voice_count(); ++i) {
        CHECK(strlen(vox_rol_voice_at(i)->label) <= 13); /* CÁLIDA has a 2-byte Á */
    }
    CHECK(vox_rol_voice_at(4) == NULL);
}

static void test_dice(void)
{
    /* Range and rough fairness over many rolls. */
    unsigned hist[21] = {0};
    for (int i = 0; i < 200000; ++i) {
        const unsigned v = vox_rol_die(lcg, NULL, 20);
        CHECK(v >= 1 && v <= 20);
        if (v >= 1 && v <= 20) {
            hist[v]++;
        }
    }
    for (int f = 1; f <= 20; ++f) {
        CHECK(hist[f] > 9000 && hist[f] < 11000); /* expected 10000 each */
    }
    /* Rejection: values in the biased top bucket are skipped. */
    seq_pos = 0;
    seq_values[0] = 0xFFFFFFFFu;        /* >= limit for 20 sides: rejected */
    seq_values[1] = 39;                 /* 39 % 20 = 19 -> 20 */
    CHECK(vox_rol_die(scripted, NULL, 20) == 20);
    CHECK(seq_pos == 2);
    CHECK(vox_rol_die(lcg, NULL, 1) == 1);
}

static void test_specs(void)
{
    vox_rol_spec_t s = vox_rol_spec_default();
    CHECK(s.mode == VOX_ROL_SUM && s.count == 2 && s.sides == 20 && s.target == 0);

    CHECK(vox_rol_mode_parse(NULL) == VOX_ROL_SUM);
    CHECK(vox_rol_mode_parse("suma") == VOX_ROL_SUM);
    CHECK(vox_rol_mode_parse("Porcentaje") == VOX_ROL_PERCENT);
    CHECK(vox_rol_mode_parse("porcentual") == VOX_ROL_PERCENT);
    CHECK(vox_rol_mode_parse("RESERVA") == VOX_ROL_POOL);
    CHECK(vox_rol_mode_parse("reservas") == VOX_ROL_SUM);
    CHECK(vox_rol_mode_parse("otro") == VOX_ROL_SUM);

    s = vox_rol_spec_make(VOX_ROL_SUM, 0, 1, 0);           /* defaults */
    CHECK(s.count == 1 && s.sides == 20 && s.target == 0);
    s = vox_rol_spec_make(VOX_ROL_SUM, 99, 500, 7);        /* clamps; no target */
    CHECK(s.count == 10 && s.sides == 100 && s.target == 0);
    s = vox_rol_spec_make(VOX_ROL_PERCENT, 5, 20, 3);      /* always 2d10 */
    CHECK(s.mode == VOX_ROL_PERCENT && s.count == 2 && s.sides == 10 && s.target == 0);
    s = vox_rol_spec_make(VOX_ROL_POOL, 6, 0, 0);          /* d10, success on 8+ */
    CHECK(s.mode == VOX_ROL_POOL && s.count == 6 && s.sides == 10 && s.target == 8);
    s = vox_rol_spec_make(VOX_ROL_POOL, 12, 6, 0);         /* d6: 5+ */
    CHECK(s.count == 10 && s.sides == 6 && s.target == 5);
    s = vox_rol_spec_make(VOX_ROL_POOL, 4, 10, 6);
    CHECK(s.target == 6);
    s = vox_rol_spec_make(VOX_ROL_POOL, 4, 10, 11);        /* above the sides -> default */
    CHECK(s.target == 8);
    s = vox_rol_spec_make((vox_rol_mode_t)9, 3, 6, 0);     /* unknown mode -> sum */
    CHECK(s.mode == VOX_ROL_SUM && s.count == 3 && s.sides == 6);

    uint16_t values[VOX_ROL_MAX_DICE];
    s = vox_rol_spec_make(VOX_ROL_POOL, 10, 10, 8);
    lcg_state = 99u;
    vox_rol_roll(&s, lcg, NULL, values);
    for (int i = 0; i < 10; ++i) {
        CHECK(values[i] >= 1 && values[i] <= 10);
    }

    char badge[8];
    s = vox_rol_spec_default();
    CHECK(vox_rol_badge(&s, badge, sizeof(badge)) == 4 && strcmp(badge, "2D20") == 0);
    s = vox_rol_spec_make(VOX_ROL_PERCENT, 0, 0, 0);
    CHECK(vox_rol_badge(&s, badge, sizeof(badge)) == 2 && strcmp(badge, "D%") == 0);
    s = vox_rol_spec_make(VOX_ROL_POOL, 10, 10, 0);
    CHECK(vox_rol_badge(&s, badge, sizeof(badge)) == 5 && strcmp(badge, "10D10") == 0);
    s = vox_rol_spec_make(VOX_ROL_SUM, 1, 100, 0);
    CHECK(vox_rol_badge(&s, badge, sizeof(badge)) == 4 && strcmp(badge, "D100") == 0);
    CHECK(vox_rol_badge(&s, badge, 3) == 0);

    char text[96];
    s = vox_rol_spec_make(VOX_ROL_POOL, 6, 10, 0);
    CHECK(vox_rol_spec_text(&s, text, sizeof(text)) > 0);
    CHECK(strcmp(text, "6d10 de reserva, éxito con 8 o más") == 0);
    s = vox_rol_spec_default();
    CHECK(vox_rol_spec_text(&s, text, sizeof(text)) > 0 && strcmp(text, "2d20 sumados") == 0);
}

static void test_view(void)
{
    vox_rol_view_t v;
    vox_rol_spec_t s = vox_rol_spec_default();
    const uint16_t crit[] = { 20, 7 };
    vox_rol_view(&s, crit, &v);
    CHECK(v.count == 2 && v.tumble == 20);
    CHECK(strcmp(v.faces[0], "20") == 0 && strcmp(v.faces[1], "7") == 0);
    CHECK(v.marks[0] == VOX_ROL_MARK_GOOD && v.marks[1] == VOX_ROL_MARK_NONE);
    CHECK(strcmp(v.title, "TIRADA 2D20") == 0);
    CHECK(strcmp(v.result, "TOTAL 27 · CRÍTICO") == 0);

    s = vox_rol_spec_make(VOX_ROL_SUM, 1, 20, 0);
    const uint16_t fumble[] = { 1 };
    vox_rol_view(&s, fumble, &v);
    CHECK(strcmp(v.result, "PIFIA") == 0 && v.marks[0] == VOX_ROL_MARK_BAD);
    s = vox_rol_spec_make(VOX_ROL_SUM, 1, 6, 0);
    const uint16_t four[] = { 4 };
    vox_rol_view(&s, four, &v);
    CHECK(strcmp(v.result, " ") == 0);

    s = vox_rol_spec_make(VOX_ROL_PERCENT, 0, 0, 0);
    const uint16_t p47[] = { 4, 7 };
    vox_rol_view(&s, p47, &v);
    CHECK(strcmp(v.faces[0], "40") == 0 && strcmp(v.faces[1], "7") == 0);
    CHECK(strcmp(v.title, "TIRADA D%") == 0 && strcmp(v.result, "RESULTADO 47") == 0);
    const uint16_t p100[] = { 10, 10 };                    /* 0 and 0 = 100 */
    vox_rol_view(&s, p100, &v);
    CHECK(strcmp(v.faces[0], "00") == 0 && strcmp(v.faces[1], "0") == 0);
    CHECK(strcmp(v.result, "RESULTADO 100") == 0);
    const uint16_t p05[] = { 10, 5 };
    CHECK(vox_rol_percent(p05) == 5);
    const uint16_t p90[] = { 9, 10 };
    CHECK(vox_rol_percent(p90) == 90);

    s = vox_rol_spec_make(VOX_ROL_POOL, 6, 10, 8);
    const uint16_t pool[] = { 9, 3, 10, 8, 1, 5 };
    CHECK(vox_rol_successes(&s, pool) == 3);
    vox_rol_view(&s, pool, &v);
    CHECK(v.count == 6);
    CHECK(v.marks[0] == VOX_ROL_MARK_GOOD && v.marks[1] == VOX_ROL_MARK_NONE &&
          v.marks[2] == VOX_ROL_MARK_GOOD && v.marks[3] == VOX_ROL_MARK_GOOD &&
          v.marks[4] == VOX_ROL_MARK_BAD && v.marks[5] == VOX_ROL_MARK_NONE);
    CHECK(strcmp(v.title, "RESERVA 6D10 · 8+") == 0);
    CHECK(strcmp(v.result, "3 ÉXITOS") == 0);
    const uint16_t none[] = { 2, 3, 4, 5, 6, 7 };
    vox_rol_view(&s, none, &v);
    CHECK(strcmp(v.result, "NINGÚN ÉXITO") == 0);
    s = vox_rol_spec_make(VOX_ROL_POOL, 1, 10, 8);
    const uint16_t single[] = { 8 };
    vox_rol_view(&s, single, &v);
    CHECK(strcmp(v.result, "1 ÉXITO") == 0);
}

static void test_roll_text(void)
{
    char out[320];
    vox_rol_spec_t s = vox_rol_spec_default();
    const uint16_t two[] = { 20, 7 };
    CHECK(vox_rol_roll_json(&s, two, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "{\"tirada\":\"2d20\",\"modo\":\"suma\",\"resultados\":[20,7],"
                      "\"total\":27,\"critico\":true,\"pifia\":false,"
                      "\"mostrado_en_pantalla\":true}") == 0);
    vox_rol_spec_t one_d20 = vox_rol_spec_make(VOX_ROL_SUM, 1, 20, 0);
    const uint16_t one[] = { 1 };
    CHECK(vox_rol_roll_json(&one_d20, one, out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"pifia\":true") != NULL);
    vox_rol_spec_t d6 = vox_rol_spec_make(VOX_ROL_SUM, 3, 6, 0);
    const uint16_t six[] = { 6, 1, 3 };
    CHECK(vox_rol_roll_json(&d6, six, out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"critico\":false,\"pifia\":false") != NULL); /* only d20 */
    CHECK(vox_rol_roll_json(&d6, six, out, 10) == 0);

    vox_rol_spec_t pct = vox_rol_spec_make(VOX_ROL_PERCENT, 0, 0, 0);
    const uint16_t p47[] = { 4, 7 };
    CHECK(vox_rol_roll_json(&pct, p47, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "{\"tirada\":\"d100\",\"modo\":\"porcentaje\",\"decenas\":4,"
                      "\"unidades\":7,\"resultado\":47,\"mostrado_en_pantalla\":true}") == 0);
    vox_rol_spec_t pool = vox_rol_spec_make(VOX_ROL_POOL, 6, 10, 8);
    const uint16_t dice[] = { 9, 3, 10, 8, 1, 5 };
    CHECK(vox_rol_roll_json(&pool, dice, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "{\"tirada\":\"6d10\",\"modo\":\"reserva\",\"exito_con\":8,"
                      "\"resultados\":[9,3,10,8,1,5],\"exitos\":3,\"unos\":1,\"maximos\":1,"
                      "\"mostrado_en_pantalla\":true}") == 0);
    vox_rol_spec_t big = vox_rol_spec_make(VOX_ROL_SUM, 10, 100, 0);
    const uint16_t hundreds[] = { 100, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
    CHECK(vox_rol_roll_json(&big, hundreds, out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"total\":1000") != NULL);

    CHECK(vox_rol_player_text(&s, two, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "Tirada del jugador con los dados de VOX: 2d20, resultados 20 y 7, "
                      "total 27.") == 0);
    CHECK(vox_rol_player_text(&d6, six, out, sizeof(out)) > 0);
    CHECK(strstr(out, "resultados 6, 1 y 3, total 10.") != NULL);
    CHECK(vox_rol_player_text(&one_d20, one, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "Tirada del jugador con los dados de VOX: 1d20, resultado 1.") == 0);
    CHECK(vox_rol_player_text(&pct, p47, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "Tirada porcentual del jugador con los dados de VOX: decenas 4, "
                      "unidades 7, resultado 47.") == 0);
    CHECK(vox_rol_player_text(&pool, dice, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "Tirada de reserva del jugador con los dados de VOX: 6d10, éxito con 8 "
                      "o más; resultados 9, 3, 10, 8, 1 y 5; 3 éxitos, 1 uno y 1 máximo "
                      "(10).") == 0);
    CHECK(vox_rol_player_text(&big, hundreds, out, sizeof(out)) > 0);
    CHECK(vox_rol_player_text(&big, hundreds, out, 200) > 0); /* fits the session buffer */
}

static void test_secret_text(void)
{
    char clean[256];
    char wrapped[256];
    secret_text_sanitize("Ana", clean, sizeof(clean), true);
    CHECK(strcmp(clean, "ANA") == 0);
    secret_text_sanitize("Íñigo", clean, sizeof(clean), true);
    CHECK(strcmp(clean, "ÍÑIGO") == 0);
    secret_text_sanitize("Eres el \xE2\x80\x9C" "traidor\xE2\x80\x9D \xE2\x80\x94 nadie lo sabe\xE2\x80\xA6 \xF0\x9F\x94\x92",
                         clean, sizeof(clean), false);
    CHECK(strcmp(clean, "Eres el \"traidor\" - nadie lo sabe...") == 0);
    secret_text_sanitize("  dos\n\nlíneas  ", clean, sizeof(clean), false);
    CHECK(strcmp(clean, "dos líneas") == 0);
    secret_text_sanitize("¿Por qué? ¡Ya!", clean, sizeof(clean), false);
    CHECK(strcmp(clean, "¿Por qué? ¡Ya!") == 0);

    const size_t lines = secret_text_wrap("uno dos tres cuatro cinco seis", 10, 5, wrapped,
                                          sizeof(wrapped));
    CHECK(lines == 4); /* "tres cuatro" is 11 glyphs: it does not fit in 10 */
    CHECK(strcmp(wrapped, "uno dos\ntres\ncuatro\ncinco seis") == 0);
    CHECK(secret_text_wrap("uno dos tres cuatro cinco seis", 11, 5, wrapped,
                           sizeof(wrapped)) == 3);
    CHECK(strcmp(wrapped, "uno dos\ntres cuatro\ncinco seis") == 0);
    /* Accented letters count as one glyph. */
    CHECK(secret_text_wrap("ááááá ééééé", 11, 3, wrapped, sizeof(wrapped)) == 1);
    /* Long words split, extra lines dropped. */
    CHECK(secret_text_wrap("abcdefghijkl", 5, 2, wrapped, sizeof(wrapped)) == 2);
    CHECK(strcmp(wrapped, "abcde\nfghij") == 0);
    CHECK(secret_text_wrap("", 5, 2, wrapped, sizeof(wrapped)) == 0);
    CHECK(wrapped[0] == '\0');
}

static void test_players(void)
{
    static rol_players_t t;
    char key[64];
    char list[128];
    rol_players_clear(&t);
    CHECK(rol_players_normalize("  Íñigo   Pérez ", key, sizeof(key)) > 0);
    CHECK(strcmp(key, "inigo perez") == 0);
    CHECK(rol_players_normalize("   ", key, sizeof(key)) == 0);

    CHECK(rol_players_join(&t, "Ana", "aaaa") == ROL_JOIN_OK);
    CHECK(rol_players_join(&t, "  ANA ", "bbbb") == ROL_JOIN_TAKEN);
    CHECK(rol_players_join(&t, "Íñigo", "cccc") == ROL_JOIN_OK);
    CHECK(rol_players_join(&t, "", "dddd") == ROL_JOIN_BAD_NAME);
    CHECK(rol_players_join(&t, "Luis", "") == ROL_JOIN_BAD_NAME);
    rol_players_list(&t, list, sizeof(list));
    CHECK(strcmp(list, "Ana, Íñigo") == 0);

    const rol_player_t *ana = rol_players_by_token(&t, "aaaa");
    CHECK(ana && strcmp(ana->name, "Ana") == 0 && ana->secret[0] == '\0');
    CHECK(rol_players_by_token(&t, "zzzz") == NULL);
    CHECK(rol_players_by_token(&t, "") == NULL);

    CHECK(rol_players_deliver(&t, "ana", "Eres el traidor."));
    CHECK(rol_players_deliver(&t, "Inigo", "Tienes la llave."));   /* accents ignored */
    CHECK(!rol_players_deliver(&t, "Marta", "nadie"));
    CHECK(ana && strcmp(ana->secret, "Eres el traidor.") == 0 && ana->secret_seq == 1);
    CHECK(rol_players_deliver(&t, "ANA", "Cambio de planes."));
    CHECK(ana && ana->secret_seq == 2);

    for (int i = 0; i < 4; ++i) {
        char name[8];
        char token[8];
        snprintf(name, sizeof(name), "J%d", i);
        snprintf(token, sizeof(token), "t%d", i);
        CHECK(rol_players_join(&t, name, token) == ROL_JOIN_OK);
    }
    CHECK(rol_players_join(&t, "Extra", "eeee") == ROL_JOIN_FULL);
    rol_players_clear(&t);
    CHECK(rol_players_by_token(&t, "aaaa") == NULL);
}

int main(void)
{
    test_players();
    test_voices();
    test_dice();
    test_specs();
    test_view();
    test_roll_text();
    test_secret_text();
    if (failures) {
        printf("%d rol host test(s) FAILED\n", failures);
        return 1;
    }
    printf("all rol host tests passed\n");
    return 0;
}
