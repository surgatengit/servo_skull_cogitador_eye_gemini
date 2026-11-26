// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Player table for the role-mode phone page (pure C, host-tested). Not
 * thread-safe: app_rol_web serializes every call with its mutex.
 *
 * A player entry exists once a phone joins with a name, or earlier, when the
 * master gives that name a secret or a character sheet (or a saved game is
 * loaded): the entry is then "reserved" and the first phone that joins with the
 * name claims it. Joining again with a taken name moves the player to the new
 * phone (lost or changed phone); the old phone loses access. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROL_PLAYERS_MAX        6
#define ROL_PLAYER_NAME_CAP    32   /* as typed, UTF-8, trimmed */
#define ROL_PLAYER_TOKEN_CAP   33   /* 32 hex chars */
#define ROL_PLAYER_SECRET_CAP  640
#define ROL_PLAYER_SHEET_CAP   1024

typedef struct {
    bool used;
    bool joined;                        /* a phone holds this entry (token set) */
    char name[ROL_PLAYER_NAME_CAP];
    char key[ROL_PLAYER_NAME_CAP];      /* normalized name for matching */
    char token[ROL_PLAYER_TOKEN_CAP];   /* "" while reserved */
    char secret[ROL_PLAYER_SECRET_CAP]; /* "" until the master sends one */
    unsigned secret_seq;                /* bumps on every new secret */
    unsigned seen_seq;                  /* secret_seq the phone has revealed */
    char sheet[ROL_PLAYER_SHEET_CAP];   /* character sheet, "" if none */
    unsigned sheet_seq;                 /* bumps on every sheet change */
} rol_player_t;

typedef struct rol_players {
    rol_player_t players[ROL_PLAYERS_MAX];
} rol_players_t;

typedef enum {
    ROL_JOIN_OK = 0,
    ROL_JOIN_REJOINED,   /* the name was already on a phone: moved to this one */
    ROL_JOIN_BAD_NAME,   /* empty or too long after trimming */
    ROL_JOIN_FULL,
} rol_join_result_t;

typedef enum {
    ROL_PUT_PHONE = 0,   /* stored for a player whose phone is connected */
    ROL_PUT_PENDING,     /* stored for a name with no phone yet: shown when it joins */
    ROL_PUT_FAILED,      /* bad name or table full */
} rol_put_result_t;

/** Forget everybody. */
void rol_players_clear(rol_players_t *t);

/** Lower-case ASCII, Spanish accents folded (á->a, ñ->n), spaces trimmed and collapsed. */
size_t rol_players_normalize(const char *name, char *out, size_t cap);

/** Register a phone: token (32 hex chars, caller-made random) becomes its credential.
 * Claims a reserved entry with the same name, or moves a joined one (REJOINED). */
rol_join_result_t rol_players_join(rol_players_t *t, const char *name, const char *token);

/** Player owning token, or NULL. */
const rol_player_t *rol_players_by_token(const rol_players_t *t, const char *token);

/** Player whose normalized name matches, or NULL. */
const rol_player_t *rol_players_by_name(const rol_players_t *t, const char *name);

/** Store a secret for `name`, reserving an entry if nobody joined as that yet. */
rol_put_result_t rol_players_deliver(rol_players_t *t, const char *name, const char *secret);

/** Store a character sheet for `name` (same rules as deliver). */
rol_put_result_t rol_players_set_sheet(rol_players_t *t, const char *name, const char *sheet);

/** The phone with `token` has revealed secret number `seq`. False if unknown token. */
bool rol_players_mark_seen(rol_players_t *t, const char *token, unsigned seq);

/** True when the player has a secret the phone has not revealed yet. */
bool rol_players_unread(const rol_player_t *p);

/** Number of used entries. */
size_t rol_players_count(const rol_players_t *t);

/** Comma-separated names of the players with a phone ("" if none). Returns the length. */
size_t rol_players_list(const rol_players_t *t, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
