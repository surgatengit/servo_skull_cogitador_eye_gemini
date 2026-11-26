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

/* Entry for `name`: the existing one, or a new reserved one. NULL if the name is
 * invalid or the table is full. *created tells which. */
static rol_player_t *find_or_reserve(rol_players_t *t, const char *name, bool *created)
{
    char key[ROL_PLAYER_NAME_CAP];
    *created = false;
    if (!t || !name || strlen(name) >= 3 * ROL_PLAYER_NAME_CAP ||
            rol_players_normalize(name, key, sizeof(key)) == 0) {
        return NULL;
    }
    rol_player_t *free_slot = NULL;
    for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
        rol_player_t *p = &t->players[i];
        if (p->used && strcmp(p->key, key) == 0) {
            return p;
        }
        if (!p->used && !free_slot) {
            free_slot = p;
        }
    }
    if (!free_slot) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    trim_copy(name, free_slot->name, sizeof(free_slot->name));
    snprintf(free_slot->key, sizeof(free_slot->key), "%s", key);
    *created = true;
    return free_slot;
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
    bool created = false;
    rol_player_t *p = find_or_reserve(t, name, &created);
    if (!p) {
        return ROL_JOIN_FULL;
    }
    const bool rejoined = p->joined;
    if (created || !rejoined) {
        trim_copy(name, p->name, sizeof(p->name)); /* the spelling the player typed */
    }
    snprintf(p->token, sizeof(p->token), "%s", token);
    p->joined = true;
    return rejoined ? ROL_JOIN_REJOINED : ROL_JOIN_OK;
}

const rol_player_t *rol_players_by_name(const rol_players_t *t, const char *name)
{
    char key[ROL_PLAYER_NAME_CAP];
    if (!t || !name || rol_players_normalize(name, key, sizeof(key)) == 0) {
        return NULL;
    }
    for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
        if (t->players[i].used && strcmp(t->players[i].key, key) == 0) {
            return &t->players[i];
        }
    }
    return NULL;
}

const rol_player_t *rol_players_by_token(const rol_players_t *t, const char *token)
{
    if (!t || !token || !token[0]) {
        return NULL;
    }
    for (size_t i = 0; i < ROL_PLAYERS_MAX; ++i) {
        if (t->players[i].used && t->players[i].joined &&
                strcmp(t->players[i].token, token) == 0) {
            return &t->players[i];
        }
    }
    return NULL;
}

/* Copy text into dst (cap bytes) without cutting a UTF-8 sequence. */
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

rol_put_result_t rol_players_deliver(rol_players_t *t, const char *name, const char *secret)
{
    bool created = false;
    rol_player_t *p = secret ? find_or_reserve(t, name, &created) : NULL;
    if (!p) {
        return ROL_PUT_FAILED;
    }
    copy_text(p->secret, sizeof(p->secret), secret);
    p->secret_seq++;
    return p->joined ? ROL_PUT_PHONE : ROL_PUT_PENDING;
}

rol_put_result_t rol_players_set_sheet(rol_players_t *t, const char *name, const char *sheet)
{
    bool created = false;
    rol_player_t *p = sheet ? find_or_reserve(t, name, &created) : NULL;
    if (!p) {
        return ROL_PUT_FAILED;
    }
    copy_text(p->sheet, sizeof(p->sheet), sheet);
    p->sheet_seq++;
    return p->joined ? ROL_PUT_PHONE : ROL_PUT_PENDING;
}

bool rol_players_mark_seen(rol_players_t *t, const char *token, unsigned seq)
{
    rol_player_t *p = (rol_player_t *)rol_players_by_token(t, token);
    if (!p) {
        return false;
    }
    if (seq <= p->secret_seq && seq > p->seen_seq) {
        p->seen_seq = seq;
    }
    return true;
}

bool rol_players_unread(const rol_player_t *p)
{
    return p && p->used && p->secret[0] && p->seen_seq != p->secret_seq;
}

size_t rol_players_count(const rol_players_t *t)
{
    size_t n = 0;
    for (size_t i = 0; t && i < ROL_PLAYERS_MAX; ++i) {
        n += t->players[i].used ? 1U : 0U;
    }
    return n;
}

size_t rol_players_list(const rol_players_t *t, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t n = 0;
    for (size_t i = 0; t && i < ROL_PLAYERS_MAX; ++i) {
        if (!t->players[i].used || !t->players[i].joined) {
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
