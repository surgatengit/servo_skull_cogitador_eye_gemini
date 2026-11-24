// SPDX-License-Identifier: Apache-2.0
/* VOX M-01 GEMINI DIRECT composition root: initialization, wiring and startup
 * order only. Behavior lives in the app_* components. */
#include "app_display.h"
#include "app_secrets.h"
#include "app_vox_eye.h"
#include "app_vox_session.h"
#include "app_wifi.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_random.h"
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
    const app_vox_eye_config_t eye = {
        .on_tap = eye_tapped,
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
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when anything below fails. */
    iris_ota_support_start();

    ESP_ERROR_CHECK_WITHOUT_ABORT(app_secrets_init(secrets_changed, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_wifi_start(wifi_changed, NULL));

    const bool eye_ready = start_eye() == ESP_OK;
    const app_vox_session_config_t session = {
        .ui = {
            .set_view = eye_ready ? ui_set_view : NULL,
            .set_status = eye_ready ? ui_set_status : NULL,
            .ui = application.eye,
        },
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_vox_session_start(&session));
    ESP_LOGI(TAG, "VOX M-01 GEMINI DIRECT started");
}
