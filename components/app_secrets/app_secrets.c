// SPDX-License-Identifier: Apache-2.0

#include "app_secrets.h"

#include <stdio.h>
#include <string.h>

#include "esp_iris.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

static const char *TAG = "app_secrets";

/* Application-owned default "nvs" partition; never Vibe Mode's sysmeta/shared. */
#define SECRETS_NAMESPACE   "vox_secret"
#define KEY_API             "api_key"
#define KEY_MODEL           "model"
#define API_KEY_MIN_LEN     30
#define API_KEY_MAX_LEN     (APP_SECRETS_API_KEY_CAPACITY - 1)

static const char *const ALLOWED_MODELS[] = {
    "gemini-3.8-live",
    "gemini-3.1-flash-live-preview",
};

/* Written once in app_secrets_init() before RPC registration; read-only afterwards. */
static struct {
    app_secrets_changed_cb_t on_changed;
    void *ctx;
} s_notify;

bool app_secrets_key_is_valid(const char *key, size_t len)
{
    if (!key || len < API_KEY_MIN_LEN || len > API_KEY_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = key[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool app_secrets_model_is_valid(const char *model, size_t len)
{
    if (!model) {
        return false;
    }
    for (size_t i = 0; i < sizeof(ALLOWED_MODELS) / sizeof(ALLOWED_MODELS[0]); ++i) {
        if (strlen(ALLOWED_MODELS[i]) == len && memcmp(ALLOWED_MODELS[i], model, len) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t load_string(const char *key, char *out, size_t capacity)
{
    if (!out || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, capacity);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SECRETS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_INITIALIZED ? err : ESP_ERR_NVS_NOT_FOUND;
    }
    size_t len = capacity;
    err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(out, capacity);
    }
    return err;
}

static esp_err_t store_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SECRETS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = value ? nvs_set_str(handle, key, value) : nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND && !value) {
        err = ESP_OK; /* erasing a missing key is a no-op */
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_secrets_load_api_key(char *out, size_t capacity)
{
    if (capacity < APP_SECRETS_API_KEY_CAPACITY) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = load_string(KEY_API, out, capacity);
    if (err == ESP_OK && !app_secrets_key_is_valid(out, strlen(out))) {
        mbedtls_platform_zeroize(out, capacity);
        err = ESP_ERR_INVALID_STATE;
    }
    return err;
}

bool app_secrets_has_api_key(void)
{
    char key[APP_SECRETS_API_KEY_CAPACITY];
    const bool present = app_secrets_load_api_key(key, sizeof(key)) == ESP_OK;
    mbedtls_platform_zeroize(key, sizeof(key));
    return present;
}

esp_err_t app_secrets_load_model(char *out, size_t capacity)
{
    esp_err_t err = load_string(KEY_MODEL, out, capacity);
    if (err == ESP_OK && !app_secrets_model_is_valid(out, strlen(out))) {
        memset(out, 0, capacity);
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    return err;
}

static void notify_changed(void)
{
    if (s_notify.on_changed) {
        s_notify.on_changed(s_notify.ctx);
    }
}

static esp_err_t reply(uint8_t *response, size_t capacity, size_t *size, const char *text)
{
    const size_t len = strlen(text);
    if (len > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response, text, len);
    *size = len;
    return ESP_OK;
}

/* Runs on the ESP-Iris service task. Never echoes or logs key material. */
static esp_err_t rpc_handler(const esp_iris_rpc_request_t *request, uint8_t *response,
                             size_t response_capacity, size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    *response_size = 0;
    switch (request->method_id) {
    case APP_SECRETS_RPC_SET_API_KEY: {
        char key[APP_SECRETS_API_KEY_CAPACITY] = {0};
        const size_t len = request->payload_size;
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (len < sizeof(key) && app_secrets_key_is_valid((const char *)request->payload, len)) {
            memcpy(key, request->payload, len);
            err = store_string(KEY_API, key);
        }
        mbedtls_platform_zeroize(key, sizeof(key));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "API key rejected or not stored (%s)", esp_err_to_name(err));
            return reply(response, response_capacity, response_size, "error");
        }
        ESP_LOGI(TAG, "API key provisioned");
        notify_changed();
        return reply(response, response_capacity, response_size, "ok");
    }
    case APP_SECRETS_RPC_CLEAR_API_KEY: {
        const esp_err_t err = store_string(KEY_API, NULL);
        ESP_LOGI(TAG, "API key cleared (%s)", esp_err_to_name(err));
        notify_changed();
        return reply(response, response_capacity, response_size, err == ESP_OK ? "ok" : "error");
    }
    case APP_SECRETS_RPC_SET_MODEL: {
        const size_t len = request->payload_size;
        char model[APP_SECRETS_MODEL_CAPACITY] = {0};
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (len == 0) {
            err = store_string(KEY_MODEL, NULL);
        } else if (len < sizeof(model) &&
                   app_secrets_model_is_valid((const char *)request->payload, len)) {
            memcpy(model, request->payload, len);
            err = store_string(KEY_MODEL, model);
        }
        if (err != ESP_OK) {
            return reply(response, response_capacity, response_size, "error");
        }
        ESP_LOGI(TAG, "Model override %s", len ? model : "cleared");
        notify_changed();
        return reply(response, response_capacity, response_size, "ok");
    }
    case APP_SECRETS_RPC_STATUS: {
        char model[APP_SECRETS_MODEL_CAPACITY];
        const bool has_model = app_secrets_load_model(model, sizeof(model)) == ESP_OK;
        char text[128];
        snprintf(text, sizeof(text), "{\"api_key\":%s,\"model\":\"%s\"}",
                 app_secrets_has_api_key() ? "true" : "false", has_model ? model : "");
        return reply(response, response_capacity, response_size, text);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t app_secrets_init(app_secrets_changed_cb_t on_changed, void *ctx)
{
    s_notify.on_changed = on_changed;
    s_notify.ctx = ctx;
    static const uint16_t methods[] = {
        APP_SECRETS_RPC_SET_API_KEY, APP_SECRETS_RPC_CLEAR_API_KEY,
        APP_SECRETS_RPC_STATUS, APP_SECRETS_RPC_SET_MODEL,
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        const esp_err_t err = esp_iris_rpc_register(APP_SECRETS_RPC_SERVICE, methods[i],
                                                    rpc_handler, NULL);
        if (err != ESP_OK) {
            for (size_t j = 0; j < i; ++j) {
                (void)esp_iris_rpc_unregister(APP_SECRETS_RPC_SERVICE, methods[j]);
            }
            return err;
        }
    }
    ESP_LOGI(TAG, "Provisioning RPC ready (service 0x%04X); key %s",
             (unsigned)APP_SECRETS_RPC_SERVICE,
             app_secrets_has_api_key() ? "present" : "missing");
    return ESP_OK;
}
