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
    APP_VOX_VIEW_IDLE = 0,     /**< cyan: conversation closed */
    APP_VOX_VIEW_LISTENING,    /**< red: mic streaming to Gemini */
    APP_VOX_VIEW_PROCESSING,   /**< amber: connecting or waiting for the model */
    APP_VOX_VIEW_SPEAKING,     /**< green: model voice playing */
    APP_VOX_VIEW_ERROR,        /**< violet: actionable error (see status text) */
} app_vox_view_t;

/** UI sink supplied by the composition root. Called from the session task. */
typedef struct {
    void (*set_view)(void *ui, app_vox_view_t view);
    void (*set_status)(void *ui, const char *text);   /**< "" hides the line */
    void (*acknowledge)(void *ui);                    /**< optional: wake word heard */
    /* Optional photo counter and gallery (1.5.0). position is 1-based (oldest = 1). */
    void (*set_photo_count)(void *ui, unsigned count);
    void (*show_photo)(void *ui, const uint8_t *jpeg, size_t size, unsigned position,
                       unsigned count);
    void (*hide_gallery)(void *ui);
    void *ui;
} app_vox_session_ui_t;

/** Gallery controls from the UI (same values as app_vox_gallery_action_t). */
typedef enum {
    APP_VOX_SESSION_GALLERY_OPEN = 0,
    APP_VOX_SESSION_GALLERY_CLOSE,
    APP_VOX_SESSION_GALLERY_PREV,
    APP_VOX_SESSION_GALLERY_NEXT,
    APP_VOX_SESSION_GALLERY_TOUCH,
    APP_VOX_SESSION_GALLERY_DELETE_ALL,
} app_vox_session_gallery_t;

typedef struct {
    app_vox_session_ui_t ui;
    int volume;            /**< 0..100, 0 = default */
    float mic_gain_db;     /**< 0 = default */
} app_vox_session_config_t;

/** Start audio, the Gemini client and the session task (singleton).
 * Requires nvs_flash_init(), ESP-Iris startup and app_wifi_start(). */
esp_err_t app_vox_session_start(const app_vox_session_config_t *config);

/** Screen tap: opens/closes the conversation, or interrupts VOX while speaking.
 * Non-blocking; safe from the GSP framework task. */
void app_vox_session_tap(void);

/** Photo counter / gallery control. Non-blocking; safe from the GSP framework task. */
void app_vox_session_gallery(app_vox_session_gallery_t action);

/** Wi-Fi state notification (app_wifi_state_t value). Non-blocking. */
void app_vox_session_wifi_changed(int app_wifi_state);

/** Secrets (API key/model) changed via provisioning RPC. Non-blocking. */
void app_vox_session_secrets_changed(void);

/** Output loudness for the eye (0..100), lock-free. */
uint8_t app_vox_session_output_level(void);

#ifdef __cplusplus
}
#endif
