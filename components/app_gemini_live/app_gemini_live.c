// SPDX-License-Identifier: Apache-2.0

#include "app_gemini_live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

static const char *TAG = "gemini_live";

#define GL_ENDPOINT \
    "wss://generativelanguage.googleapis.com/ws/" \
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="
#define GL_URI_CAPACITY        512
#define GL_RX_CAPACITY         (256 * 1024)   /* one reassembled server message */
#define GL_PCM_CAPACITY        (192 * 1024)   /* decoded audio of one message */
#define GL_TX_CAPACITY         6144           /* prefix + base64(1600 samples) + suffix */
#define GL_MAX_FRAME_SAMPLES   1600
#define GL_HANDLE_CAPACITY     512
#define GL_WS_BUFFER           4096           /* rx+tx in internal RAM; messages are reassembled */
#define GL_WS_TASK_STACK       8192
#define GL_WS_TASK_PRIO        10
#define GL_WS_TASK_CORE        0
#define GL_SEND_TIMEOUT_MS     200
#define GL_RECONNECT_MS        2000
#define GL_FAILURES_TO_FALLBACK 2
#define GL_FAILURES_TO_GIVE_UP  5

static const char AUDIO_PREFIX[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
static const char AUDIO_SUFFIX[] = "\"}}}";
static const char STREAM_END[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";

/* Ownership: config strings, buffers and the client belong to the instance.
 * `lock` serializes senders against disconnect; the WebSocket task never takes
 * it (it uses the client from event data), so stop/destroy cannot deadlock.
 * RX state is touched only by the WebSocket task. */
struct app_gemini_live {
    char *model;
    char *fallback_model;
    char *voice;
    char *language;
    char *system_instruction;
    app_gemini_live_cb_t on_event;
    void *ctx;

    SemaphoreHandle_t lock;
    esp_websocket_client_handle_t client;
    volatile bool ready;
    bool use_fallback;       /* model of the current connection */
    bool prefer_fallback;    /* starting model for new connections (sticky per boot) */
    int failures;            /* connection attempts ended before setupComplete */
    bool got_setup;          /* setupComplete seen on the current connection */
    int last_http_status;    /* handshake status of the latest failed attempt */
    app_gemini_live_stats_t stats; /* diagnostics; updated with __atomic builtins */

    char *rx;
    size_t rx_len;
    bool rx_overflow;
    uint8_t *pcm;
    char *tx;
    char resume_handle[GL_HANDLE_CAPACITY];
};

static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void psram_free(void *p)
{
    heap_caps_free(p);
}

static char *dup_or_null(const char *s)
{
    if (!s) {
        return NULL;
    }
    const size_t n = strlen(s) + 1;
    char *d = psram_malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void emit(struct app_gemini_live *gl, app_gemini_live_event_type_t type, int code)
{
    const app_gemini_live_event_t event = { .type = type, .close_code = code };
    gl->on_event(&event, gl->ctx);
}

const char *app_gemini_live_active_model(app_gemini_live_handle_t gl)
{
    if (!gl) {
        return "";
    }
    return (gl->use_fallback && gl->fallback_model) ? gl->fallback_model : gl->model;
}

/* Build the setup message with cJSON so the persona is escaped correctly. */
static char *build_setup(struct app_gemini_live *gl)
{
    char model_path[96];
    snprintf(model_path, sizeof(model_path), "models/%s", app_gemini_live_active_model(gl));
    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", model_path);

    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    cJSON *speech = cJSON_AddObjectToObject(gen, "speechConfig");
    cJSON *voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", gl->voice ? gl->voice : "Charon");
    cJSON_AddStringToObject(speech, "languageCode", gl->language ? gl->language : "es-ES");

    if (gl->system_instruction) {
        cJSON *instr = cJSON_AddObjectToObject(setup, "systemInstruction");
        cJSON *parts = cJSON_AddArrayToObject(instr, "parts");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", gl->system_instruction);
        cJSON_AddItemToArray(parts, part);
    }
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");
    cJSON *rt = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(rt, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", false);
    cJSON *cwc = cJSON_AddObjectToObject(setup, "contextWindowCompression");
    cJSON_AddObjectToObject(cwc, "slidingWindow");
    cJSON *resume = cJSON_AddObjectToObject(setup, "sessionResumption");
    if (gl->resume_handle[0]) {
        cJSON_AddStringToObject(resume, "handle", gl->resume_handle);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static void handle_server_content(struct app_gemini_live *gl, const cJSON *content)
{
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(content, "interrupted"))) {
        emit(gl, APP_GL_EVT_INTERRUPTED, 0);
    }
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(content, "inputTranscription");
    const cJSON *input_text = cJSON_GetObjectItemCaseSensitive(input, "text");
    if (cJSON_IsString(input_text) && input_text->valuestring[0]) {
        __atomic_add_fetch(&gl->stats.rx_input_transcripts, 1U, __ATOMIC_RELAXED);
        const app_gemini_live_event_t ev = { .type = APP_GL_EVT_INPUT_TRANSCRIPT,
                                             .text = input_text->valuestring };
        gl->on_event(&ev, gl->ctx);
    }
    const cJSON *output = cJSON_GetObjectItemCaseSensitive(content, "outputTranscription");
    const cJSON *output_text = cJSON_GetObjectItemCaseSensitive(output, "text");
    if (cJSON_IsString(output_text) && output_text->valuestring[0]) {
        const app_gemini_live_event_t ev = { .type = APP_GL_EVT_OUTPUT_TRANSCRIPT,
                                             .text = output_text->valuestring };
        gl->on_event(&ev, gl->ctx);
    }
    const cJSON *turn = cJSON_GetObjectItemCaseSensitive(content, "modelTurn");
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(turn, "parts");
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *inline_data = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
        if (!cJSON_IsString(data)) {
            continue;
        }
        size_t decoded = 0;
        const size_t len = strlen(data->valuestring);
        if (mbedtls_base64_decode(gl->pcm, GL_PCM_CAPACITY, &decoded,
                                  (const unsigned char *)data->valuestring, len) != 0) {
            ESP_LOGW(TAG, "audio chunk rejected (base64 %u bytes)", (unsigned)len);
            continue;
        }
        __atomic_add_fetch(&gl->stats.rx_audio_chunks, 1U, __ATOMIC_RELAXED);
        const app_gemini_live_event_t ev = {
            .type = APP_GL_EVT_AUDIO,
            .pcm = (const int16_t *)gl->pcm,
            .samples = decoded / sizeof(int16_t),
        };
        gl->on_event(&ev, gl->ctx);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(content, "turnComplete"))) {
        emit(gl, APP_GL_EVT_TURN_COMPLETE, 0);
    }
}

static void handle_message(struct app_gemini_live *gl, esp_websocket_client_handle_t client)
{
    __atomic_add_fetch(&gl->stats.rx_messages, 1U, __ATOMIC_RELAXED);
    cJSON *root = cJSON_ParseWithLength(gl->rx, gl->rx_len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable server message (%u bytes)", (unsigned)gl->rx_len);
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete")) {
        gl->got_setup = true;
        gl->failures = 0;
        gl->ready = true;
        ESP_LOGI(TAG, "session ready (model %s)", app_gemini_live_active_model(gl));
        emit(gl, APP_GL_EVT_READY, 0);
    }
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (content) {
        handle_server_content(gl, content);
    }
    const cJSON *update = cJSON_GetObjectItemCaseSensitive(root, "sessionResumptionUpdate");
    if (update) {
        const cJSON *handle = cJSON_GetObjectItemCaseSensitive(update, "newHandle");
        const bool resumable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(update, "resumable"));
        if (resumable && cJSON_IsString(handle) &&
                strlen(handle->valuestring) < sizeof(gl->resume_handle)) {
            strcpy(gl->resume_handle, handle->valuestring);
        }
    }
    const cJSON *go_away = cJSON_GetObjectItemCaseSensitive(root, "goAway");
    if (go_away) {
        const cJSON *left = cJSON_GetObjectItemCaseSensitive(go_away, "timeLeft");
        ESP_LOGI(TAG, "goAway (timeLeft %s); will resume", cJSON_IsString(left) ? left->valuestring : "?");
        emit(gl, APP_GL_EVT_GO_AWAY, 0);
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(error, "message");
        ESP_LOGW(TAG, "server error: %s", cJSON_IsString(msg) ? msg->valuestring : "(no message)");
    }
    (void)client;
    cJSON_Delete(root);
}

static void on_data(struct app_gemini_live *gl, const esp_websocket_event_data_t *d)
{
    /* Text (1) and binary (2) frames both carry JSON; 0 continues a fragmented message. */
    if (d->op_code != 0x0 && d->op_code != 0x1 && d->op_code != 0x2) {
        return;
    }
    if (d->op_code != 0x0 && d->payload_offset == 0) {
        gl->rx_len = 0;
        gl->rx_overflow = false;
    }
    if (d->data_len > 0) {
        if (gl->rx_len + (size_t)d->data_len >= GL_RX_CAPACITY) {
            gl->rx_overflow = true;
        } else {
            memcpy(gl->rx + gl->rx_len, d->data_ptr, (size_t)d->data_len);
            gl->rx_len += (size_t)d->data_len;
        }
    }
    const bool frame_done = d->payload_offset + d->data_len >= d->payload_len;
    if (!frame_done || !d->fin) {
        return;
    }
    if (gl->rx_overflow) {
        ESP_LOGW(TAG, "server message exceeded %u bytes; dropped", (unsigned)GL_RX_CAPACITY);
    } else if (gl->rx_len > 0) {
        gl->rx[gl->rx_len] = '\0';
        handle_message(gl, d->client);
    }
    gl->rx_len = 0;
    gl->rx_overflow = false;
}

/* Called once per ended connection attempt (DISCONNECTED/CLOSED). A session that
 * had reached setupComplete gets a fresh retry budget; later failed attempts count,
 * so an unreachable server ends in FAILED instead of reconnecting forever. */
static void note_failure(struct app_gemini_live *gl, int code)
{
    if (gl->got_setup) {
        gl->got_setup = false;
        gl->failures = 0;
        return;
    }
    if (gl->last_http_status) {
        code = gl->last_http_status;
    }
    gl->failures++;
    if (gl->failures >= GL_FAILURES_TO_FALLBACK && !gl->use_fallback && gl->fallback_model) {
        gl->use_fallback = true;
        ESP_LOGW(TAG, "switching to fallback model %s", gl->fallback_model);
    }
    if (gl->failures >= GL_FAILURES_TO_GIVE_UP) {
        emit(gl, APP_GL_EVT_FAILED, code);
    }
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    struct app_gemini_live *gl = arg;
    const esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        emit(gl, APP_GL_EVT_CONNECTING, 0);
        break;
    case WEBSOCKET_EVENT_CONNECTED: {
        gl->got_setup = false;
        gl->last_http_status = 0;
        gl->rx_len = 0;
        char *setup = build_setup(gl);
        if (!setup) {
            ESP_LOGE(TAG, "setup allocation failed");
            break;
        }
        const int sent = esp_websocket_client_send_text(d->client, setup, (int)strlen(setup),
                                                        pdMS_TO_TICKS(2000));
        cJSON_free(setup);
        ESP_LOGI(TAG, "connected; setup %s (model %s%s)", sent >= 0 ? "sent" : "FAILED",
                 app_gemini_live_active_model(gl), gl->resume_handle[0] ? ", resuming" : "");
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        on_data(gl, d);
        break;
    case WEBSOCKET_EVENT_ERROR: {
        const int status = d->error_handle.esp_ws_handshake_status_code;
        ESP_LOGW(TAG, "error type %d, handshake HTTP %d, tls 0x%x", d->error_handle.error_type,
                 status, (unsigned)d->error_handle.esp_tls_last_esp_err);
        if (status > 0) {
            gl->last_http_status = status; /* counted when the attempt ends */
        }
        if (status == 409 && gl->resume_handle[0]) {
            /* 409 Conflict: the server still holds the session we tried to resume
             * (seen in the Google AI Studio error chart after fast reconnects).
             * Start a fresh session on the next attempt instead of colliding again. */
            gl->resume_handle[0] = '\0';
            ESP_LOGW(TAG, "resume rejected (409); next attempt opens a fresh session");
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: {
        const bool was_ready = gl->ready;
        gl->ready = false;
        ESP_LOGW(TAG, "%s (close code %d)", id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected",
                 d->close_status_code);
        note_failure(gl, d->close_status_code);
        if (was_ready || id == WEBSOCKET_EVENT_DISCONNECTED) {
            emit(gl, APP_GL_EVT_DISCONNECTED, d->close_status_code);
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t app_gemini_live_create(const app_gemini_live_config_t *config,
                                 app_gemini_live_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!config || !config->model || !config->on_event) {
        return ESP_ERR_INVALID_ARG;
    }
    static bool hooks_installed;
    if (!hooks_installed) {
        /* Large server messages: keep cJSON trees in PSRAM. Installed once at startup. */
        cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = psram_free };
        cJSON_InitHooks(&hooks);
        /* The connection URI carries the API key: keep transport debug logs quiet. */
        esp_log_level_set("websocket_client", ESP_LOG_WARN);
        esp_log_level_set("transport_ws", ESP_LOG_WARN);
        esp_log_level_set("trans_tcp", ESP_LOG_WARN);
        esp_log_level_set("transport_base", ESP_LOG_WARN);
        hooks_installed = true;
    }
    struct app_gemini_live *gl = calloc(1, sizeof(*gl));
    if (!gl) {
        return ESP_ERR_NO_MEM;
    }
    gl->on_event = config->on_event;
    gl->ctx = config->ctx;
    gl->model = dup_or_null(config->model);
    gl->fallback_model = dup_or_null(config->fallback_model);
    gl->voice = dup_or_null(config->voice);
    gl->language = dup_or_null(config->language);
    gl->system_instruction = dup_or_null(config->system_instruction);
    gl->lock = xSemaphoreCreateMutex();
    gl->rx = heap_caps_malloc(GL_RX_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gl->pcm = heap_caps_malloc(GL_PCM_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* PSRAM: internal DMA RAM is reserved for the display path. */
    gl->tx = heap_caps_malloc(GL_TX_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gl->model || !gl->lock || !gl->rx || !gl->pcm || !gl->tx ||
            (config->fallback_model && !gl->fallback_model) ||
            (config->voice && !gl->voice) || (config->language && !gl->language) ||
            (config->system_instruction && !gl->system_instruction)) {
        app_gemini_live_handle_t tmp = gl;
        (void)app_gemini_live_destroy(&tmp);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = gl;
    return ESP_OK;
}

esp_err_t app_gemini_live_connect(app_gemini_live_handle_t gl, const char *api_key)
{
    if (!gl || !api_key || !api_key[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gl->client) {
        return ESP_ERR_INVALID_STATE;
    }
    gl->resume_handle[0] = '\0';  /* every tap opens a fresh conversation */
    gl->failures = 0;
    gl->got_setup = false;
    gl->last_http_status = 0;
    gl->use_fallback = gl->prefer_fallback && gl->fallback_model;
    gl->ready = false;

    char *uri = heap_caps_calloc(1, GL_URI_CAPACITY, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!uri) {
        return ESP_ERR_NO_MEM;
    }
    const int n = snprintf(uri, GL_URI_CAPACITY, "%s%s", GL_ENDPOINT, api_key);
    if (n <= 0 || n >= GL_URI_CAPACITY) {
        mbedtls_platform_zeroize(uri, GL_URI_CAPACITY);
        heap_caps_free(uri);
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = GL_WS_BUFFER,
        .task_stack = GL_WS_TASK_STACK,
        .task_prio = GL_WS_TASK_PRIO,
        .task_name = "gemini_ws",
        .task_core_id_set = true,
        .task_core_id = GL_WS_TASK_CORE,
        .reconnect_timeout_ms = GL_RECONNECT_MS,
        .network_timeout_ms = 10000,
        .enable_close_reconnect = true,   /* resume after goAway / server close */
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,
        .user_agent = "vox-m01/1.0",
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    /* The client keeps its own copy of the URI; erase ours immediately. */
    mbedtls_platform_zeroize(uri, GL_URI_CAPACITY);
    heap_caps_free(uri);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_handler, gl);
    if (err == ESP_OK) {
        xSemaphoreTake(gl->lock, portMAX_DELAY);
        gl->client = client;
        xSemaphoreGive(gl->lock);
        err = esp_websocket_client_start(client);
    }
    if (err != ESP_OK) {
        xSemaphoreTake(gl->lock, portMAX_DELAY);
        gl->client = NULL;
        xSemaphoreGive(gl->lock);
        esp_websocket_client_destroy(client);
        return err;
    }
    ESP_LOGI(TAG, "connecting (model %s)", app_gemini_live_active_model(gl));
    return ESP_OK;
}

esp_err_t app_gemini_live_disconnect(app_gemini_live_handle_t gl)
{
    if (!gl) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(gl->lock, portMAX_DELAY);
    esp_websocket_client_handle_t client = gl->client;
    gl->client = NULL;
    gl->ready = false;
    xSemaphoreGive(gl->lock);
    if (!client) {
        return ESP_OK;
    }
    /* stop (not close): enable_close_reconnect would otherwise reconnect. */
    (void)esp_websocket_client_stop(client);
    const esp_err_t err = esp_websocket_client_destroy(client);
    gl->resume_handle[0] = '\0';
    ESP_LOGI(TAG, "conversation closed");
    return err;
}

static esp_err_t send_locked(struct app_gemini_live *gl, const char *text, size_t len)
{
    if (!gl->client || !gl->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const int sent = esp_websocket_client_send_text(gl->client, text, (int)len,
                                                    pdMS_TO_TICKS(GL_SEND_TIMEOUT_MS));
    if (sent < 0) {
        if (__atomic_add_fetch(&gl->stats.tx_failures, 1U, __ATOMIC_RELAXED) == 1U) {
            ESP_LOGW(TAG, "first send failure (%u bytes)", (unsigned)len);
        }
        return ESP_FAIL;
    }
    __atomic_add_fetch(&gl->stats.tx_messages, 1U, __ATOMIC_RELAXED);
    return ESP_OK;
}

void app_gemini_live_get_stats(app_gemini_live_handle_t gl, app_gemini_live_stats_t *out)
{
    if (!gl || !out) {
        return;
    }
    out->tx_messages = __atomic_load_n(&gl->stats.tx_messages, __ATOMIC_RELAXED);
    out->tx_failures = __atomic_load_n(&gl->stats.tx_failures, __ATOMIC_RELAXED);
    out->tx_not_ready = __atomic_load_n(&gl->stats.tx_not_ready, __ATOMIC_RELAXED);
    out->rx_messages = __atomic_load_n(&gl->stats.rx_messages, __ATOMIC_RELAXED);
    out->rx_audio_chunks = __atomic_load_n(&gl->stats.rx_audio_chunks, __ATOMIC_RELAXED);
    out->rx_input_transcripts = __atomic_load_n(&gl->stats.rx_input_transcripts, __ATOMIC_RELAXED);
}

esp_err_t app_gemini_live_send_audio(app_gemini_live_handle_t gl, const int16_t *pcm,
                                     size_t samples)
{
    if (!gl || !pcm || samples == 0 || samples > GL_MAX_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gl->ready) {
        __atomic_add_fetch(&gl->stats.tx_not_ready, 1U, __ATOMIC_RELAXED);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(gl->lock, portMAX_DELAY);
    const size_t prefix = sizeof(AUDIO_PREFIX) - 1;
    const size_t suffix = sizeof(AUDIO_SUFFIX) - 1;
    memcpy(gl->tx, AUDIO_PREFIX, prefix);
    size_t b64 = 0;
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (mbedtls_base64_encode((unsigned char *)gl->tx + prefix, GL_TX_CAPACITY - prefix - suffix,
                              &b64, (const unsigned char *)pcm, samples * sizeof(int16_t)) == 0) {
        memcpy(gl->tx + prefix + b64, AUDIO_SUFFIX, suffix);
        err = send_locked(gl, gl->tx, prefix + b64 + suffix);
    }
    xSemaphoreGive(gl->lock);
    return err;
}

esp_err_t app_gemini_live_send_audio_stream_end(app_gemini_live_handle_t gl)
{
    if (!gl) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(gl->lock, portMAX_DELAY);
    const esp_err_t err = send_locked(gl, STREAM_END, sizeof(STREAM_END) - 1);
    xSemaphoreGive(gl->lock);
    return err;
}

bool app_gemini_live_is_ready(app_gemini_live_handle_t gl)
{
    return gl && gl->ready;
}

esp_err_t app_gemini_live_swap_model(app_gemini_live_handle_t gl)
{
    if (!gl || !gl->fallback_model) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (gl->client) {
        return ESP_ERR_INVALID_STATE; /* takes effect on the next connect */
    }
    gl->prefer_fallback = !gl->use_fallback;
    ESP_LOGW(TAG, "next conversations use %s",
             gl->prefer_fallback ? gl->fallback_model : gl->model);
    return ESP_OK;
}

esp_err_t app_gemini_live_destroy(app_gemini_live_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    struct app_gemini_live *gl = *handle;
    if (!gl) {
        return ESP_OK;
    }
    if (gl->lock) {
        (void)app_gemini_live_disconnect(gl);
        vSemaphoreDelete(gl->lock);
    }
    psram_free(gl->model);
    psram_free(gl->fallback_model);
    psram_free(gl->voice);
    psram_free(gl->language);
    psram_free(gl->system_instruction);
    heap_caps_free(gl->rx);
    heap_caps_free(gl->pcm);
    heap_caps_free(gl->tx);
    free(gl);
    *handle = NULL;
    return ESP_OK;
}
