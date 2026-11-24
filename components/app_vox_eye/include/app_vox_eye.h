// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
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

/** Output loudness 0..100 sampled at the animation rate on the render task.
 * Must be lock-free and non-blocking. */
typedef uint8_t (*app_vox_eye_level_fn_t)(void *ctx);

typedef struct {
    app_vox_eye_tap_cb_t on_tap;     /**< Optional; NULL ignores taps. */
    void *tap_ctx;                   /**< Passed to on_tap; must outlive the controller. */
    app_vox_eye_level_fn_t level;    /**< Optional speaking level source. */
    void *level_ctx;                 /**< Passed to level; must outlive the controller. */
    uint32_t seed;                   /**< Non-zero random seed for gaze and blinks. */
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

/** Set the bottom status line (uppercase Spanish, digits, " -·.:/"); NULL or "" hides it.
 * Callable from any application task; the GSP setter is queued. */
esp_gsp_err_t app_vox_eye_set_status(app_vox_eye_handle_t handle, const char *utf8);

/** Copy diagnostics; call on the UI loop or after dispatch stops. */
esp_gsp_err_t app_vox_eye_read(app_vox_eye_handle_t handle, app_vox_eye_snapshot_t *out_snapshot);

/** Detach the event callback, delete the timer and free the controller.
 * Quiesce GSP dispatch first; retains the handle if cleanup fails. */
esp_gsp_err_t app_vox_eye_destroy(app_vox_eye_handle_t *handle);

#ifdef __cplusplus
}
#endif
