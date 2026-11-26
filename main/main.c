// SPDX-License-Identifier: Apache-2.0
/* VOX M-01 GEMINI DIRECT composition root: initialization, wiring and startup
 * order only. Behavior lives in the app_* components. */
#include <string.h>

#include "app_display.h"
#include "app_secrets.h"
#include "app_vox_eye.h"
#include "app_vox_session.h"
#include "app_wifi.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "vox_m01";

/* The composition root retains owned handles, never component internals. */
static struct {
    esp_gsp_handle_t ui;
    app_vox_eye_handle_t eye;
} application;

static const app_vox_eye_state_t EYE_FOR_VIEW[] = {
    [APP_VOX_VIEW_IDLE] = APP_VOX_EYE_IDLE,
    [APP_VOX_VIEW_LISTENING] = APP_VOX_EYE_LISTENING,
    [APP_VOX_VIEW_PROCESSING] = APP_VOX_EYE_PROCESSING,
    [APP_VOX_VIEW_SPEAKING] = APP_VOX_EYE_SPEAKING,
    [APP_VOX_VIEW_ERROR] = APP_VOX_EYE_ERROR,
};

static void ui_set_view(void *ui, app_vox_view_t view)
{
    app_vox_eye_set_state((app_vox_eye_handle_t)ui, EYE_FOR_VIEW[view]);
}

static void ui_set_status(void *ui, const char *text)
{
    (void)app_vox_eye_set_status((app_vox_eye_handle_t)ui, text);
}

static void ui_acknowledge(void *ui)
{
    app_vox_eye_blink((app_vox_eye_handle_t)ui);
}

static void ui_set_photo_count(void *ui, unsigned count)
{
    (void)app_vox_eye_set_photo_count((app_vox_eye_handle_t)ui, count);
}

static void ui_show_photo(void *ui, const uint8_t *jpeg, size_t size, unsigned position,
                          unsigned count)
{
    (void)app_vox_eye_show_photo((app_vox_eye_handle_t)ui, jpeg, size, position, count);
}

static void ui_hide_gallery(void *ui)
{
    (void)app_vox_eye_hide_gallery((app_vox_eye_handle_t)ui);
}

static void ui_set_voice_initial(void *ui, char initial)
{
    (void)app_vox_eye_set_voice_initial((app_vox_eye_handle_t)ui, initial);
}

static void ui_set_role_mode(void *ui, bool enabled)
{
    (void)app_vox_eye_set_role_mode((app_vox_eye_handle_t)ui, enabled);
}

static void ui_set_role_badge(void *ui, const char *text)
{
    (void)app_vox_eye_set_role_badge((app_vox_eye_handle_t)ui, text);
}

_Static_assert(APP_VOX_SESSION_MAX_DICE == APP_VOX_EYE_MAX_DICE, "dice panel size");

static void ui_show_roll(void *ui, const app_vox_session_roll_t *roll)
{
    app_vox_eye_roll_t view = {
        .count = roll->count,
        .tumble = roll->tumble,
    };
    memcpy(view.faces, roll->faces, sizeof(view.faces));
    memcpy(view.marks, roll->marks, sizeof(view.marks));
    memcpy(view.title, roll->title, sizeof(view.title));
    memcpy(view.result, roll->result, sizeof(view.result));
    (void)app_vox_eye_show_roll((app_vox_eye_handle_t)ui, &view);
}

static void ui_show_secret(void *ui, const char *player, const char *text)
{
    (void)app_vox_eye_show_secret((app_vox_eye_handle_t)ui, player, text);
}

/* GSP framework task: only enqueue. The enums share their values. */
static void eye_action(app_vox_eye_action_t action, void *ctx)
{
    (void)ctx;
    app_vox_session_action((app_vox_session_action_t)action);
}

/* GSP framework task: only enqueue. The enums share their values. */
static void eye_gallery(app_vox_gallery_action_t action, void *ctx)
{
    (void)ctx;
    app_vox_session_gallery((app_vox_session_gallery_t)action);
}

static void eye_tapped(void *ctx)
{
    (void)ctx;
    app_vox_session_tap();
}

static uint8_t eye_level(void *ctx)
{
    (void)ctx;
    return app_vox_session_output_level();
}

static void wifi_changed(app_wifi_state_t state, void *ctx)
{
    (void)ctx;
    app_vox_session_wifi_changed((int)state);
}

static void secrets_changed(void *ctx)
{
    (void)ctx;
    app_vox_session_secrets_changed();
}

/* esp_restart() runs this before the reset (ESP-Iris triggers restarts for
 * `iris restart` and System Update). The camera component's README requires
 * waiting for LCD/PPA completion before a software reset: a panel transfer cut
 * in flight leaves the DVP camera unable to deliver frames until a power cycle
 * (test 13). Pause the GSP render task and retire the presenter fences. */
static void quiesce_display_on_restart(void)
{
    if (!application.ui) {
        return;
    }
    esp_gsp_esp_lcd_pause_t *pause = NULL;
    const esp_err_t err = esp_gsp_esp_lcd_pause(application.ui, 300, &pause);
    esp_rom_printf("vox_m01: display quiesced before restart (%s)\n", esp_err_to_name(err));
}

static esp_err_t start_eye(void)
{
    esp_gsp_config_t config;
    esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return err;
    }
    esp_gsp_esp_lcd_config_t display;
    err = app_display_init(&display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", err);
        return err;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_register_shutdown_handler(quiesce_display_on_restart));
    const app_vox_eye_config_t eye = {
        .on_tap = eye_tapped,
        .on_gallery = eye_gallery,
        .on_action = eye_action,
        .level = eye_level,
        .seed = esp_random() | 1U,
    };
    /* GSP retains the callback context; the eye lives until reboot. */
    return app_vox_eye_create(application.ui, &eye, &application.eye) == ESP_GSP_OK ? ESP_OK
                                                                                    : ESP_FAIL;
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    /* The app-owned "nvs" partition may hold data from a previously installed
     * Idea. Never erase it: report the fault and keep Recovery reachable. */
    const esp_err_t nvs_err = nvs_flash_init();
    /* Recovery must remain reachable even when anything below fails. */
    iris_ota_support_start();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "app NVS unavailable (%s); not erasing it", esp_err_to_name(nvs_err));
        if (start_eye() == ESP_OK) {
            app_vox_eye_set_state(application.eye, APP_VOX_EYE_ERROR);
            (void)app_vox_eye_set_status(application.eye, "ERROR DE NVS");
        }
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(app_secrets_init(secrets_changed, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_wifi_start(wifi_changed, NULL));

    const bool eye_ready = start_eye() == ESP_OK;
    const app_vox_session_config_t session = {
        .ui = {
            .set_view = eye_ready ? ui_set_view : NULL,
            .set_status = eye_ready ? ui_set_status : NULL,
            .acknowledge = eye_ready ? ui_acknowledge : NULL,
            .set_photo_count = eye_ready ? ui_set_photo_count : NULL,
            .show_photo = eye_ready ? ui_show_photo : NULL,
            .hide_gallery = eye_ready ? ui_hide_gallery : NULL,
            .set_voice_initial = eye_ready ? ui_set_voice_initial : NULL,
            .set_role_mode = eye_ready ? ui_set_role_mode : NULL,
            .set_role_badge = eye_ready ? ui_set_role_badge : NULL,
            .show_roll = eye_ready ? ui_show_roll : NULL,
            .show_secret = eye_ready ? ui_show_secret : NULL,
            .ui = application.eye,
        },
    };
    const esp_err_t err = app_vox_session_start(&session);
    if (err != ESP_OK) {
        /* Keep the eye and Recovery alive; show the fault instead of a silent idle eye. */
        ESP_LOGE(TAG, "session unavailable (%s)", esp_err_to_name(err));
        if (eye_ready) {
            app_vox_eye_set_state(application.eye, APP_VOX_EYE_ERROR);
            (void)app_vox_eye_set_status(application.eye, "ERROR INTERNO");
        }
        return;
    }
    ESP_LOGI(TAG, "VOX M-01 GEMINI DIRECT started");
}
