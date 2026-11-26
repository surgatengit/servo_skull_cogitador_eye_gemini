// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Player table for the role-mode phone page (pure C, host-tested). Not
 * thread-safe: app_rol_web serializes every call with its mutex. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROL_PLAYERS_MAX        6
#define ROL_PLAYER_NAME_CAP    32   /* as typed, UTF-8, trimmed */
#define ROL_PLAYER_TOKEN_CAP   33   /* 32 hex chars */
#define ROL_PLAYER_SECRET_CAP  640

typedef struct {
    bool used;
    char name[ROL_PLAYER_NAME_CAP];
    char key[ROL_PLAYER_NAME_CAP];      /* normalized name for matching */
    char token[ROL_PLAYER_TOKEN_CAP];
    char secret[ROL_PLAYER_SECRET_CAP]; /* "" until the master sends one */
    unsigned secret_seq;                /* bumps on every new secret */
} rol_player_t;

typedef struct {
    rol_player_t players[ROL_PLAYERS_MAX];
} rol_players_t;

typedef enum {
    ROL_JOIN_OK = 0,
    ROL_JOIN_BAD_NAME,   /* empty or too long after trimming */
    ROL_JOIN_TAKEN,      /* someone already joined with that name */
    ROL_JOIN_FULL,
} rol_join_result_t;

/** Forget everybody. */
void rol_players_clear(rol_players_t *t);

/** Lower-case ASCII, Spanish accents folded (á->a, ñ->n), spaces trimmed and collapsed. */
size_t rol_players_normalize(const char *name, char *out, size_t cap);

/** Register a phone: token (32 hex chars, caller-made random) becomes its credential. */
rol_join_result_t rol_players_join(rol_players_t *t, const char *name, const char *token);

/** Player owning token, or NULL. */
const rol_player_t *rol_players_by_token(const rol_players_t *t, const char *token);

/** Store a secret for the player whose normalized name matches; false if nobody joined as that. */
bool rol_players_deliver(rol_players_t *t, const char *name, const char *secret);

/** Comma-separated names of the joined players ("" if none). Returns the length. */
size_t rol_players_list(const rol_players_t *t, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
