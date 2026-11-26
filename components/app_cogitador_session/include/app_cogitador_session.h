// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the UI should show; maps 1:1 to the eye colors. */
typedef enum {
    APP_COGITADOR_VIEW_IDLE = 0,     /**< cyan: conversation closed */
    APP_COGITADOR_VIEW_LISTENING,    /**< red: mic streaming to Gemini */
    APP_COGITADOR_VIEW_PROCESSING,   /**< amber: connecting or waiting for the model */
    APP_COGITADOR_VIEW_SPEAKING,     /**< green: model voice playing */
    APP_COGITADOR_VIEW_ERROR,        /**< violet: actionable error (see status text) */
} app_cogitador_view_t;

#define APP_COGITADOR_SESSION_MAX_DICE 10

/** A roll for the dice panel (texts uppercase, card charset). */
typedef struct {
    unsigned count;                             /**< 1..APP_COGITADOR_SESSION_MAX_DICE */
    char faces[APP_COGITADOR_SESSION_MAX_DICE][4];    /**< final text per die */
    uint8_t marks[APP_COGITADOR_SESSION_MAX_DICE];    /**< 0 normal, 1 gold (max/success), 2 red (1) */
    unsigned tumble;                            /**< random 1..tumble while rolling */
    char title[32];                             /**< "TIRADA 2D20", "RESERVA 6D10 · 8+" */
    char result[40];                            /**< "TOTAL 29", "RESULTADO 47", "3 ÉXITOS" */
} app_cogitador_session_roll_t;

/** UI sink supplied by the composition root. Called from the session task. */
typedef struct {
    void (*set_view)(void *ui, app_cogitador_view_t view);
    void (*set_status)(void *ui, const char *text);   /**< "" hides the line */
    void (*acknowledge)(void *ui);                    /**< optional: wake word heard */
    /* Optional photo counter and gallery (1.5.0). position is 1-based (oldest = 1). */
    void (*set_photo_count)(void *ui, unsigned count);
    void (*show_photo)(void *ui, const uint8_t *jpeg, size_t size, unsigned position,
                       unsigned count);
    void (*hide_gallery)(void *ui);
    /* Optional RoleMaster controls (1.6.0). */
    void (*set_voice_initial)(void *ui, char initial);
    void (*set_role_mode)(void *ui, bool enabled);
    void (*set_role_badge)(void *ui, const char *text);  /**< "2D20", "D%", "6D10" */
    void (*show_roll)(void *ui, const app_cogitador_session_roll_t *roll);
    void (*show_secret)(void *ui, const char *player, const char *text);
    void *ui;
} app_cogitador_session_ui_t;

/** Gallery controls from the UI (same values as app_cogitador_gallery_action_t). */
typedef enum {
    APP_COGITADOR_SESSION_GALLERY_OPEN = 0,
    APP_COGITADOR_SESSION_GALLERY_CLOSE,
    APP_COGITADOR_SESSION_GALLERY_PREV,
    APP_COGITADOR_SESSION_GALLERY_NEXT,
    APP_COGITADOR_SESSION_GALLERY_TOUCH,
    APP_COGITADOR_SESSION_GALLERY_DELETE_ALL,
} app_cogitador_session_gallery_t;

/** Bezel buttons from the UI (same values as app_cogitador_eye_action_t). */
typedef enum {
    APP_COGITADOR_SESSION_ACTION_VOICE_NEXT = 0, /**< voice badge: next voice */
    APP_COGITADOR_SESSION_ACTION_ROLE_ROLL,      /**< dice badge (role mode): the prepared player roll */
    APP_COGITADOR_SESSION_ACTION_CLOSE_HOLD,     /**< role mode: eye held, leave the game */
} app_cogitador_session_action_t;

typedef struct {
    app_cogitador_session_ui_t ui;
    int volume;            /**< 0..100, 0 = default */
    float mic_gain_db;     /**< 0 = default */
} app_cogitador_session_config_t;

/** Start audio, the Gemini client and the session task (singleton).
 * Requires nvs_flash_init(), ESP-Iris startup and app_wifi_start(). */
esp_err_t app_cogitador_session_start(const app_cogitador_session_config_t *config);

/** Screen tap: opens/closes the conversation, or interrupts Cogitador while speaking.
 * Non-blocking; safe from the GSP framework task. */
void app_cogitador_session_tap(void);

/** Photo counter / gallery control. Non-blocking; safe from the GSP framework task. */
void app_cogitador_session_gallery(app_cogitador_session_gallery_t action);

/** Bezel button. Non-blocking; safe from the GSP framework task. */
void app_cogitador_session_action(app_cogitador_session_action_t action);

/** Wi-Fi state notification (app_wifi_state_t value). Non-blocking. */
void app_cogitador_session_wifi_changed(int app_wifi_state);

/** Secrets (API key/model/voice) changed via provisioning RPC. Non-blocking. */
void app_cogitador_session_secrets_changed(void);

/** Output loudness for the eye (0..100), lock-free. */
uint8_t app_cogitador_session_output_level(void);

#ifdef __cplusplus
}
#endif
