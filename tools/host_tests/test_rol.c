// SPDX-License-Identifier: Apache-2.0
/* PC tests for the 1.6.0 RoleMaster helpers: voices, dice, roll text, secret card. */
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

    int count = 0, sides = 1;
    vox_rol_clamp(&count, &sides);
    CHECK(count == 1 && sides == 20);
    count = 9;
    sides = 500;
    vox_rol_clamp(&count, &sides);
    CHECK(count == 4 && sides == 100);
    count = 2;
    sides = 6;
    vox_rol_clamp(&count, &sides);
    CHECK(count == 2 && sides == 6);
}

static void test_roll_text(void)
{
    char out[256];
    const uint16_t two[] = { 20, 7 };
    CHECK(vox_rol_roll_json(two, 2, 20, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "{\"tirada\":\"2d20\",\"resultados\":[20,7],\"total\":27,"
                      "\"critico\":true,\"pifia\":false,\"mostrado_en_pantalla\":true}") == 0);
    const uint16_t one[] = { 1 };
    CHECK(vox_rol_roll_json(one, 1, 20, out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"pifia\":true") != NULL);
    const uint16_t six[] = { 6, 1, 3 };
    CHECK(vox_rol_roll_json(six, 3, 6, out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"critico\":false,\"pifia\":false") != NULL); /* only d20 */
    CHECK(vox_rol_roll_json(six, 3, 6, out, 10) == 0);

    CHECK(vox_rol_player_text(two, 2, 20, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "Tirada del jugador con los dados de VOX: 2d20, resultados 20 y 7, "
                      "total 27.") == 0);
    CHECK(vox_rol_player_text(six, 3, 6, out, sizeof(out)) > 0);
    CHECK(strstr(out, "resultados 6, 1 y 3, total 10.") != NULL);
    CHECK(vox_rol_player_text(one, 1, 20, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "Tirada del jugador con los dados de VOX: 1d20, resultado 1.") == 0);
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
    test_roll_text();
    test_secret_text();
    if (failures) {
        printf("%d rol host test(s) FAILED\n", failures);
        return 1;
    }
    printf("all rol host tests passed\n");
    return 0;
}
