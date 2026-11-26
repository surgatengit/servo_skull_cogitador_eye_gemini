// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque mechanical-eye controller; contracts: ../README.md. */
typedef struct app_vox_eye *app_vox_eye_handle_t;

/** Visual states. Colors: idle cyan, listening red, processing amber,
 * speaking green, error violet. */
typedef enum {
    APP_VOX_EYE_IDLE = 0,
    APP_VOX_EYE_LISTENING,
    APP_VOX_EYE_PROCESSING,
    APP_VOX_EYE_SPEAKING,
    APP_VOX_EYE_ERROR,
    APP_VOX_EYE_STATE_COUNT,
} app_vox_eye_state_t;

/** Tap notification. Runs on the GSP framework task: return quickly, never block. */
typedef void (*app_vox_eye_tap_cb_t)(void *ctx);

/** Photo counter and gallery controls (1.5.0). */
typedef enum {
    APP_VOX_GALLERY_OPEN = 0,   /**< photo counter tapped */
    APP_VOX_GALLERY_CLOSE,      /**< X tapped */
    APP_VOX_GALLERY_PREV,       /**< "<" tapped (older photo) */
    APP_VOX_GALLERY_NEXT,       /**< ">" tapped (newer photo) */
    APP_VOX_GALLERY_TOUCH,      /**< any other touch while the gallery is shown */
    APP_VOX_GALLERY_DELETE_ALL, /**< X held for 1.5 s: erase every stored photo */
} app_vox_gallery_action_t;

/** Gallery notification. Runs on the GSP framework task: return quickly, never block. */
typedef void (*app_vox_gallery_cb_t)(app_vox_gallery_action_t action, void *ctx);

/** Bezel buttons added in 1.6.0 (RoleMaster). */
typedef enum {
    APP_VOX_EYE_ACTION_VOICE_NEXT = 0, /**< voice badge (top right) tapped */
    APP_VOX_EYE_ACTION_ROLE_ROLL,      /**< dice badge (role mode, right) tapped */
    APP_VOX_EYE_ACTION_CLOSE_HOLD,     /**< role mode: eye held ~1.5 s (leave the game) */
} app_vox_eye_action_t;

/** Bezel button notification. Runs on the GSP framework task: never block. */
typedef void (*app_vox_eye_action_cb_t)(app_vox_eye_action_t action, void *ctx);

/** Most dice shown at once by app_vox_eye_show_roll() (two rows of 5). */
#define APP_VOX_EYE_MAX_DICE 10

/** Die highlight. */
typedef enum {
    APP_VOX_EYE_DIE_NORMAL = 0,
    APP_VOX_EYE_DIE_GOOD,      /**< gold: maximum or success */
    APP_VOX_EYE_DIE_BAD,       /**< red: a 1 */
} app_vox_eye_die_mark_t;

/** A roll as shown on the dice panel (texts in the card charset, uppercase). */
typedef struct {
    unsigned count;                         /**< 1..APP_VOX_EYE_MAX_DICE */
    char faces[APP_VOX_EYE_MAX_DICE][4];    /**< final text per die ("17", "40") */
    uint8_t marks[APP_VOX_EYE_MAX_DICE];    /**< app_vox_eye_die_mark_t */
    unsigned tumble;                        /**< random 1..tumble while rolling (2..100) */
    char title[32];                         /**< "TIRADA 2D20" */
    char result[40];                        /**< "TOTAL 29 · CRÍTICO" */
} app_vox_eye_roll_t;

/** Output loudness 0..100 sampled at the animation rate on the render task.
 * Must be lock-free and non-blocking. */
typedef uint8_t (*app_vox_eye_level_fn_t)(void *ctx);

typedef struct {
    app_vox_eye_tap_cb_t on_tap;     /**< Optional; NULL ignores taps. */
    void *tap_ctx;                   /**< Passed to on_tap; must outlive the controller. */
    app_vox_eye_level_fn_t level;    /**< Optional speaking level source. */
    void *level_ctx;                 /**< Passed to level; must outlive the controller. */
    uint32_t seed;                   /**< Non-zero random seed for gaze and blinks. */
    app_vox_gallery_cb_t on_gallery; /**< Optional; NULL ignores the photo controls. */
    void *gallery_ctx;               /**< Passed to on_gallery; must outlive the controller. */
    app_vox_eye_action_cb_t on_action; /**< Optional; NULL ignores the bezel buttons. */
    void *action_ctx;                /**< Passed to on_action; must outlive the controller. */
} app_vox_eye_config_t;

/** Diagnostic value copy. */
typedef struct {
    app_vox_eye_state_t state;       /**< State currently drawn. */
    uint32_t frames;                 /**< Animation ticks executed. */
    uint32_t blinks;                 /**< Completed blinks. */
    uint32_t taps;                   /**< Taps received. */
    esp_gsp_err_t last_error;        /**< Most recent setter error, or ESP_GSP_OK. */
} app_vox_eye_snapshot_t;

/** Create the controller, claim the UI event callback and start the animation timer.
 * ui/config/out_handle must be non-NULL; ui must outlive the controller. */
esp_gsp_err_t app_vox_eye_create(esp_gsp_handle_t ui, const app_vox_eye_config_t *config,
                                 app_vox_eye_handle_t *out_handle);

/** Request a visual state from any task; applied on the next animation tick. */
void app_vox_eye_set_state(app_vox_eye_handle_t handle, app_vox_eye_state_t state);

/** Request an acknowledgement blink (e.g. wake word heard) from any task. */
void app_vox_eye_blink(app_vox_eye_handle_t handle);

/** Set the bottom status line (uppercase Spanish, digits, " -·.:/"); NULL or "" hides it.
 * Callable from any application task; the GSP setter is queued. */
esp_gsp_err_t app_vox_eye_set_status(app_vox_eye_handle_t handle, const char *utf8);

/** Photo counter on the bezel (top left): hidden when count is 0. Any task. */
esp_gsp_err_t app_vox_eye_set_photo_count(app_vox_eye_handle_t handle, unsigned count);

/**
 * Show the gallery with photo `position` of `count` (1-based, shown as
 * "position / count"). jpeg/size are copied by GSP before returning; NULL keeps
 * the current picture (used by the PC preview). The eye animation pauses while
 * the gallery is shown. Any task.
 */
esp_gsp_err_t app_vox_eye_show_photo(app_vox_eye_handle_t handle, const uint8_t *jpeg, size_t size,
                                     unsigned position, unsigned count);

/** Hide the gallery and resume the eye. Any task. */
esp_gsp_err_t app_vox_eye_hide_gallery(app_vox_eye_handle_t handle);

/** Voice badge letter (A-Z, usually the voice's initial). Any task. */
esp_gsp_err_t app_vox_eye_set_voice_initial(app_vox_eye_handle_t handle, char initial);

/** Show or hide the role-mode dice badge. Any task. */
esp_gsp_err_t app_vox_eye_set_role_mode(app_vox_eye_handle_t handle, bool enabled);

/** Role badge label ("2D20", "D%", "6D10"; digits, D and % only). Any task. */
esp_gsp_err_t app_vox_eye_set_role_badge(app_vox_eye_handle_t handle, const char *text);

/**
 * Show a roll: the render task animates ~0.8 s of tumbling numbers, then the
 * faces with their marks, the result line, and hides it after ~6 s or on a tap.
 * Up to 5 dice in one row, 6..10 in two. *roll is copied. A newer roll
 * replaces the current one. Single producer task.
 */
esp_gsp_err_t app_vox_eye_show_roll(app_vox_eye_handle_t handle, const app_vox_eye_roll_t *roll);

/**
 * Show a private card "SOLO PARA <player>" with `text` (UTF-8; mapped to the
 * card charset and word-wrapped) until it is tapped. The eye pauses meanwhile.
 * Any task.
 */
esp_gsp_err_t app_vox_eye_show_secret(app_vox_eye_handle_t handle, const char *player,
                                      const char *text);

/** Copy diagnostics; call on the UI loop or after dispatch stops. */
esp_gsp_err_t app_vox_eye_read(app_vox_eye_handle_t handle, app_vox_eye_snapshot_t *out_snapshot);

/** Detach the event callback, delete the timer and free the controller.
 * Quiesce GSP dispatch first; retains the handle if cleanup fails. */
esp_gsp_err_t app_vox_eye_destroy(app_vox_eye_handle_t *handle);

#ifdef __cplusplus
}
#endif
