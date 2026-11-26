// SPDX-License-Identifier: Apache-2.0
/* PC tests for the Google Search grounding path: groundingMetadata parsing
 * (components/app_gemini_live/gl_grounding.c) and status-line formatting
 * (components/app_cogitador_session/cogitador_status_text.c). Build and run with
 * tools/host_tests/run.ps1. Source files are UTF-8. */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gl_grounding.h"
#include "cogitador_status_text.h"

static int failures;

#define CHECK_STR(actual, expected) check_str(__LINE__, (actual), (expected))
#define CHECK_INT(actual, expected) check_int(__LINE__, (long)(actual), (long)(expected))

static void check_str(int line, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0) {
        printf("FAIL line %d: got \"%s\", expected \"%s\"\n", line, actual, expected);
        failures++;
    }
}

static void check_int(int line, long actual, long expected)
{
    if (actual != expected) {
        printf("FAIL line %d: got %ld, expected %ld\n", line, actual, expected);
        failures++;
    }
}

static size_t fmt(const char *prefix, const char *text, char *out, size_t cap)
{
    return cogitador_status_text(prefix, text, strlen(text), out, cap, 24);
}

static void test_status_text(void)
{
    char out[64];
    CHECK_INT(fmt("FUENTE: ", "rae.es", out, sizeof(out)), 14);
    CHECK_STR(out, "FUENTE: RAE.ES");
    fmt("GOOGLE: ", "año ñu pingüino", out, sizeof(out));
    CHECK_STR(out, "GOOGLE: AÑO ÑU PINGUINO");
    fmt("", "¿Qué tiempo hace en Cádiz?", out, sizeof(out));
    CHECK_STR(out, "QUÉ TIEMPO HACE EN CÁDIZ");
    fmt("", "  a,,,b  ", out, sizeof(out));
    CHECK_STR(out, "A B");
    fmt("", "emoji \xF0\x9F\x98\x80 ok", out, sizeof(out));
    CHECK_STR(out, "EMOJI OK");
    /* Cut at a word boundary with "...", never above 24 characters. */
    CHECK_INT(fmt("GOOGLE: ", "precio del oro hoy en euros", out, sizeof(out)), 21);
    CHECK_STR(out, "GOOGLE: PRECIO DEL...");
    /* One long word: cut inside it. */
    CHECK_INT(fmt("", "supercalifragilisticoespialidoso", out, sizeof(out)), 24);
    CHECK_STR(out, "SUPERCALIFRAGILISTICO...");
    /* Accented characters count as one character but two bytes. */
    CHECK_INT(fmt("", "áéíóúñáéíóúñáéíóúñáéíóúñáéí", out, sizeof(out)), 24);
    CHECK_STR(out, "ÁÉÍÓÚÑÁÉÍÓÚÑÁÉÍÓÚÑÁÉÍ...");
    /* Small output buffer: always NUL-terminated, no split UTF-8 sequence. */
    cogitador_status_text("", "ñañaña", strlen("ñañaña"), out, 6, 24);
    CHECK_INT((long)strlen(out) < 6, 1);
    CHECK_INT(((unsigned char)out[strlen(out) - 1] & 0xC0) != 0xC0, 1);
    /* text_len limits the input (entries are not NUL-terminated in lists). */
    cogitador_status_text("", "uno\ndos", 3, out, sizeof(out), 24);
    CHECK_STR(out, "UNO");
}

static void merge_json(gl_grounding_t *g, const char *json, size_t expect_new_queries)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        printf("FAIL: invalid test JSON\n");
        failures++;
        return;
    }
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    const cJSON *metadata = cJSON_GetObjectItemCaseSensitive(content, "groundingMetadata");
    CHECK_INT(gl_grounding_merge(g, metadata), expect_new_queries);
    cJSON_Delete(root);
}

static void test_grounding(void)
{
    gl_grounding_t g;
    CHECK_INT(gl_grounding_init(&g), ESP_OK);
    /* Shape of groundingMetadata in the Gemini API reference. */
    merge_json(&g,
               "{\"serverContent\":{\"groundingMetadata\":{"
               "\"webSearchQueries\":[\"tiempo en Madrid mañana\",\"AEMET Madrid\"],"
               "\"searchEntryPoint\":{\"renderedContent\":\"<style>.x{}</style><div>...</div>\"},"
               "\"groundingChunks\":["
               "{\"web\":{\"uri\":\"https://vertexaisearch.cloud.google.com/grounding-api-redirect/AbC\","
               "\"title\":\"aemet.es\"}},"
               "{\"web\":{\"uri\":\"https://vertexaisearch.cloud.google.com/x\",\"title\":\"www.ElTiempo.es\"}},"
               "{\"web\":{\"uri\":\"https://vertexaisearch.cloud.google.com/y\",\"title\":\"aemet.es\"}}],"
               "\"groundingSupports\":[{\"segment\":{\"startIndex\":0,\"endIndex\":10},"
               "\"groundingChunkIndices\":[0]}]}}}",
               2);
    /* A second message of the same answer: duplicates are ignored. */
    merge_json(&g,
               "{\"serverContent\":{\"groundingMetadata\":{"
               "\"webSearchQueries\":[\"AEMET Madrid\"],"
               "\"groundingChunks\":[{\"web\":{\"title\":\"Wikipedia | La enciclopedia\"}},"
               "{\"web\":{\"domain\":\"rae.es\",\"title\":\"Diccionario\"}},"
               "{\"retrievedContext\":{\"uri\":\"gs://x\"}}]}}}",
               0);
    char out[256];
    CHECK_INT(gl_grounding_copy(&g, true, out, sizeof(out)), 2);
    CHECK_STR(out, "tiempo en Madrid mañana\nAEMET Madrid");
    CHECK_INT(gl_grounding_copy(&g, false, out, sizeof(out)), 4);
    CHECK_STR(out, "aemet.es\neltiempo.es\nwikipedia\nrae.es");
    /* Whole entries only when the caller's buffer is small. */
    CHECK_INT(gl_grounding_copy(&g, false, out, 22), 2);
    CHECK_STR(out, "aemet.es\neltiempo.es");
    /* Metadata without the optional lists, or not an object, is harmless. */
    merge_json(&g, "{\"serverContent\":{\"groundingMetadata\":{}}}", 0);
    CHECK_INT(gl_grounding_merge(&g, NULL), 0);
    gl_grounding_clear(&g);
    CHECK_INT(gl_grounding_copy(&g, true, out, sizeof(out)), 0);
    CHECK_STR(out, "");
    /* Very long query: cut below the entry limit at a UTF-8 boundary. */
    char longq[400] = "{\"serverContent\":{\"groundingMetadata\":{\"webSearchQueries\":[\"";
    for (int i = 0; i < 70; ++i) {
        strcat(longq, "ñ");
    }
    strcat(longq, "\"]}}}");
    merge_json(&g, longq, 1);
    CHECK_INT(gl_grounding_copy(&g, true, out, sizeof(out)), 1);
    CHECK_INT((long)strlen(out), 120);  /* 60 whole two-byte characters, ENTRY_MAX 120 */
    gl_grounding_deinit(&g);
}

int main(void)
{
    test_status_text();
    test_grounding();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all grounding host tests passed\n");
    return 0;
}
