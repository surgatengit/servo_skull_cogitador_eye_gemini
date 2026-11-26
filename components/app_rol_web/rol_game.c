// SPDX-License-Identifier: Apache-2.0
#include "rol_game.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "rol_players.h"

/* Copy without cutting a UTF-8 sequence. */
static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_str(char *dst, size_t cap, const cJSON *item)
{
    copy_text(dst, cap, cJSON_IsString(item) ? item->valuestring : "");
}

static int get_int(const cJSON *obj, const char *name, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : fallback;
}

char *rol_game_build(const rol_game_meta_t *meta, const char *summary,
                     const struct rol_players *players)
{
    if (!meta) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *dice = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    if (!root || !dice || !list) {
        cJSON_Delete(root);
        cJSON_Delete(dice);
        cJSON_Delete(list);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "vox_partida", ROL_GAME_VERSION);
    cJSON_AddStringToObject(root, "titulo", meta->title);
    cJSON_AddStringToObject(root, "juego", meta->game);
    /* The summary is the master's own text; keep it within the document budget. */
    char *cut = NULL;
    const char *text = summary ? summary : "";
    if (strlen(text) >= ROL_GAME_SUMMARY_CAP) {
        cut = cJSON_malloc(ROL_GAME_SUMMARY_CAP);
        if (cut) {
            copy_text(cut, ROL_GAME_SUMMARY_CAP, text);
            text = cut;
        }
    }
    cJSON_AddStringToObject(root, "resumen", text); /* copied */
    cJSON_free(cut);
    cJSON_AddStringToObject(dice, "modo", meta->dice_mode[0] ? meta->dice_mode : "suma");
    cJSON_AddNumberToObject(dice, "cantidad", meta->dice_count);
    cJSON_AddNumberToObject(dice, "caras", meta->dice_sides);
    cJSON_AddNumberToObject(dice, "exito_con", meta->dice_target);
    cJSON_AddItemToObject(root, "dados", dice);
    for (size_t i = 0; players && i < ROL_PLAYERS_MAX; ++i) {
        const rol_player_t *p = &players->players[i];
        if (!p->used) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            break;
        }
        cJSON_AddStringToObject(item, "nombre", p->name);
        cJSON_AddStringToObject(item, "ficha", p->sheet);
        cJSON_AddStringToObject(item, "secreto", p->secret);
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddItemToObject(root, "jugadores", list);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && strlen(json) >= ROL_GAME_MAX_BYTES) {
        cJSON_free(json);
        return NULL;
    }
    return json;
}

static cJSON *parse_checked(const char *json, size_t len)
{
    if (!json || len == 0 || len >= ROL_GAME_MAX_BYTES) {
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "vox_partida");
    const cJSON *players = cJSON_GetObjectItemCaseSensitive(root, "jugadores");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) ||
            (int)version->valuedouble != ROL_GAME_VERSION ||
            (players && !cJSON_IsArray(players))) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

bool rol_game_valid(const char *json, size_t len)
{
    cJSON *root = parse_checked(json, len);
    cJSON_Delete(root);
    return root != NULL;
}

bool rol_game_restore(const char *json, rol_game_meta_t *meta, struct rol_players *players,
                      bool with_secrets)
{
    cJSON *root = parse_checked(json, json ? strlen(json) : 0);
    if (!root) {
        return false;
    }
    if (meta) {
        memset(meta, 0, sizeof(*meta));
        copy_str(meta->title, sizeof(meta->title), cJSON_GetObjectItemCaseSensitive(root, "titulo"));
        copy_str(meta->game, sizeof(meta->game), cJSON_GetObjectItemCaseSensitive(root, "juego"));
        const cJSON *dice = cJSON_GetObjectItemCaseSensitive(root, "dados");
        copy_str(meta->dice_mode, sizeof(meta->dice_mode),
                 cJSON_GetObjectItemCaseSensitive(dice, "modo"));
        meta->dice_count = get_int(dice, "cantidad", 2);
        meta->dice_sides = get_int(dice, "caras", 20);
        meta->dice_target = get_int(dice, "exito_con", 0);
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "jugadores");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "nombre");
        if (!players || !cJSON_IsString(name)) {
            continue;
        }
        const cJSON *sheet = cJSON_GetObjectItemCaseSensitive(item, "ficha");
        const cJSON *secret = cJSON_GetObjectItemCaseSensitive(item, "secreto");
        /* Every saved player gets an entry, even without a sheet. */
        (void)rol_players_set_sheet(players, name->valuestring,
                                    cJSON_IsString(sheet) ? sheet->valuestring : "");
        if (with_secrets && cJSON_IsString(secret) && secret->valuestring[0]) {
            (void)rol_players_deliver(players, name->valuestring, secret->valuestring);
        }
    }
    cJSON_Delete(root);
    return true;
}
