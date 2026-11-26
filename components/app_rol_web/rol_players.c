// SPDX-License-Identifier: Apache-2.0
#include "rol_players.h"

#include <stdio.h>
#include <string.h>

void rol_players_clear(rol_players_t *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

static char fold_latin1(unsigned char second)
{
    switch (second) {
    case 0xA1: case 0x81: case 0xA0: case 0x80: case 0xA4: case 0x84: return 'a';
    case 0xA9: case 0x89: case 0xA8: case 0x88: case 0xAB: case 0x8B: return 'e';
    case 0xAD: case 0x8D: case 0xAC: case 0x8C: case 0xAF: case 0x8F: return 'i';
    case 0xB3: case 0x93: case 0xB2: case 0x92: case 0xB6: case 0x96: return 'o';
    case 0xBA: case 0x9A: case 0xB9: case 0x99: case 0xBC: case 0x9C: return 'u';
    case 0xB1: case 0x91: return 'n';
    case 0xA7: case 0x87: return 'c';
    default: return 0;
    }
}

size_t rol_players_normalize(const char *name, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    while (*p && n + 1 < cap) {
        char c = 0;
        if (*p < 0x80) {
            c = (char)*p;
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c + 32);
            }
            if (c == '\t' || c == '\r' || c == '\n') {
                c = ' ';
            }
            if ((unsigned char)c < 0x20) {
                c = 0;
            }
            p++;
        } else if (*p == 0xC3 && p[1]) {
            c = fold_latin1(p[1]);
            p += 2;
        } else {
            p++;                       /* other bytes of other scripts: dropped */
            while (*p && (*p & 0xC0) == 0x80) {
                p++;
            }
        }
        if (!c || (c == ' ' && (n == 0 || out[n - 1] == ' '))) {
            continue;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
    return n;
}

static void trim_copy(const char *src, char *dst, size_t cap)
{
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
        src++;
    }
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' ||
                       src[len - 1] == '\n')) {
        len--;
    }
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;                     /* never cut a UTF-8 sequence */
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

rol_join_result_t rol_players_join(rol_players_t *t, const char *name, const char *token)
{
    if (!t || !name || !token || strlen(token) + 1 > ROL_PLAYER_TOKEN_CAP || !token[0]) {
        return ROL_JOIN_BAD_NAME;
    }
    char key[ROL_PLAYER_NAME_CAP];
    if (strlen(name) >= 3 * ROL_PLAYER_NAME_CAP ||
            rol_players_normalize(name, key, sizeof(key)) == 0) {
        return ROL_JOIN_BAD_NAME;
    }
    rol_player_t *free_slot = NULL;
    for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
        rol_player_t *p = &t->players[i];
        if (p->used && strcmp(p->key, key) == 0) {
            return ROL_JOIN_TAKEN;
        }
        if (!p->used && !free_slot) {
            free_slot = p;
        }
    }
    if (!free_slot) {
        return ROL_JOIN_FULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    trim_copy(name, free_slot->name, sizeof(free_slot->name));
    snprintf(free_slot->key, sizeof(free_slot->key), "%s", key);
    snprintf(free_slot->token, sizeof(free_slot->token), "%s", token);
    return ROL_JOIN_OK;
}

const rol_player_t *rol_players_by_token(const rol_players_t *t, const char *token)
{
    if (!t || !token || !token[0]) {
        return NULL;
    }
    for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
        if (t->players[i].used && strcmp(t->players[i].token, token) == 0) {
            return &t->players[i];
        }
    }
    return NULL;
}

bool rol_players_deliver(rol_players_t *t, const char *name, const char *secret)
{
    if (!t || !name || !secret) {
        return false;
    }
    char key[ROL_PLAYER_NAME_CAP];
    if (rol_players_normalize(name, key, sizeof(key)) == 0) {
        return false;
    }
    for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
        rol_player_t *p = &t->players[i];
        if (p->used && strcmp(p->key, key) == 0) {
            snprintf(p->secret, sizeof(p->secret), "%s", secret);
            p->secret_seq++;
            return true;
        }
    }
    return false;
}

size_t rol_players_list(const rol_players_t *t, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t n = 0;
    for (size_t i = 0; t && i < ROL_PLAYERS_MAX; ++i) {
        if (!t->players[i].used) {
            continue;
        }
        const int w = snprintf(out + n, cap - n, "%s%s", n ? ", " : "", t->players[i].name);
        if (w < 0 || (size_t)w >= cap - n) {
            out[n] = '\0';
            break;
        }
        n += (size_t)w;
    }
    return n;
}
