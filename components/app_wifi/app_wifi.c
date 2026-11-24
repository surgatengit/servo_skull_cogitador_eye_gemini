// SPDX-License-Identifier: Apache-2.0

#include "app_wifi.h"

#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

static const char *TAG = "app_wifi";

#define BACKOFF_MIN_MS  1000U
#define BACKOFF_MAX_MS  30000U

/* Singleton: the application owns exactly one station. Fields are written on the
 * default event loop task after start; state is an aligned word read elsewhere. */
static struct {
    bool started;
    app_wifi_state_cb_t cb;
    void *ctx;
    volatile app_wifi_state_t state;
    esp_timer_handle_t retry_timer;
    uint32_t backoff_ms;
} s_wifi;

static void set_state(app_wifi_state_t state)
{
    if (s_wifi.state == state) {
        return;
    }
    s_wifi.state = state;
    if (s_wifi.cb) {
        s_wifi.cb(state, s_wifi.ctx);
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect request failed (%s)", esp_err_to_name(err));
    }
}

static void schedule_retry(void)
{
    const uint32_t jitter = esp_random() % (s_wifi.backoff_ms / 4U + 1U);
    const uint32_t delay = s_wifi.backoff_ms + jitter;
    s_wifi.backoff_ms = s_wifi.backoff_ms * 2U > BACKOFF_MAX_MS ? BACKOFF_MAX_MS
                                                                : s_wifi.backoff_ms * 2U;
    (void)esp_timer_stop(s_wifi.retry_timer);
    const esp_err_t err = esp_timer_start_once(s_wifi.retry_timer, (uint64_t)delay * 1000U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "retry timer failed (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "reconnecting in %u ms", (unsigned)delay);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        set_state(APP_WIFI_CONNECTING);
        retry_cb(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *info = data;
        ESP_LOGW(TAG, "disconnected (reason %d)", info ? info->reason : -1);
        set_state(APP_WIFI_CONNECTING);
        schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&ip->ip_info.ip));
        s_wifi.backoff_ms = BACKOFF_MIN_MS;
        set_state(APP_WIFI_CONNECTED);
    }
}

/* Copy validated lengths into the station arrays: 32/64-byte fields need no NUL. */
static esp_err_t apply_credentials(void)
{
    app_config_wifi_t shared;
    esp_err_t err = app_config_load_wifi(&shared);
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(&shared, sizeof(shared));
        return err;
    }
    wifi_config_t config = {0};
    const size_t ssid_len = strnlen(shared.ssid, sizeof(config.sta.ssid));
    const size_t pass_len = strnlen(shared.password, sizeof(config.sta.password));
    memcpy(config.sta.ssid, shared.ssid, ssid_len);
    memcpy(config.sta.password, shared.password, pass_len);
    config.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    mbedtls_platform_zeroize(&shared, sizeof(shared));
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    mbedtls_platform_zeroize(&config, sizeof(config));
    return err;
}

esp_err_t app_wifi_start(app_wifi_state_cb_t cb, void *ctx)
{
    if (s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi.cb = cb;
    s_wifi.ctx = ctx;
    s_wifi.state = APP_WIFI_UNCONFIGURED;
    s_wifi.backoff_ms = BACKOFF_MIN_MS;

    esp_err_t err = esp_netif_init(); /* idempotent; ESP-Iris TCP shares the netif */
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_FAIL;
    }
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&timer_args, &s_wifi.retry_timer);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        set_state(APP_WIFI_FAILED);
        return err;
    }
    s_wifi.started = true;

    err = apply_credentials();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Wi-Fi unconfigured; set it up in Vibe Mode");
        set_state(APP_WIFI_UNCONFIGURED);
        return ESP_OK; /* UI stays available */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shared Wi-Fi configuration unusable (%s)", esp_err_to_name(err));
        set_state(APP_WIFI_FAILED);
        return ESP_OK;
    }
    set_state(APP_WIFI_CONNECTING); /* configured: never report UNCONFIGURED from here on */
    /* Real-time audio streaming: avoid modem sleep latency. */
    err = esp_wifi_start();
    if (err == ESP_OK) {
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
    } else {
        set_state(APP_WIFI_FAILED);
    }
    return err;
}

app_wifi_state_t app_wifi_get_state(void)
{
    return s_wifi.state;
}
