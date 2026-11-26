// SPDX-License-Identifier: Apache-2.0

#include "app_gemini_live.h"
#include "gl_grounding.h"

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
#define GL_IMAGE_TIMEOUT_MS    3000           /* one ~32 KB realtimeInput.video message */
#define GL_TOOL_TIMEOUT_MS     1000
#define GL_RECONNECT_MS        2000
#define GL_FAILURES_TO_FALLBACK 2
#define GL_FAILURES_TO_GIVE_UP  5

static const char AUDIO_PREFIX[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
static const char AUDIO_SUFFIX[] = "\"}}}";
static const char STREAM_END[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
static const char IMAGE_PREFIX[] = "{\"realtimeInput\":{\"video\":{\"mimeType\":\"image/jpeg\",\"data\":\"";
static const char IMAGE_SUFFIX[] = "\"}}}";

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
    bool google_search;
    char *function_declarations;  /* JSON array, or NULL */
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
    gl_grounding_t grounding;   /* queries and sources of the current answer */
    /* Latest usageMetadata (WebSocket task only), logged once per finished turn. */
    struct {
        bool seen;
        int prompt, response, tool_prompt, thoughts, total;
    } usage;
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
    /* No thinkingConfig is sent: 3.1 Flash Live keeps its default level and 3.8
     * Live rejects the field. */
    cJSON *decls = gl->function_declarations ? cJSON_Parse(gl->function_declarations) : NULL;
    if (gl->function_declarations && !cJSON_IsArray(decls)) {
        ESP_LOGE(TAG, "function declarations are not a JSON array; not declared");
        cJSON_Delete(decls);
        decls = NULL;
    }
    if (gl->google_search || decls) {
        cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
        if (gl->google_search) {
            /* Native Grounding with Google Search: runs on Google's side, so it
             * needs no toolResponse from the device. */
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddObjectToObject(tool, "googleSearch");
            cJSON_AddItemToArray(tools, tool);
        }
        if (decls) {
            /* Device functions: calls arrive as toolCall and are answered with
             * toolResponse (app_gemini_live_send_tool_response). */
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddItemToObject(tool, "functionDeclarations", decls);
            cJSON_AddItemToArray(tools, tool);
        }
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
    bool activity = false;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *inline_data = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
        if (!cJSON_IsString(data)) {
            /* Text, thought summaries, or the code the model runs to search
             * (executableCode / codeExecutionResult): never played, but they
             * prove the turn is in progress. Logged without content. */
            activity = true;
            __atomic_add_fetch(&gl->stats.rx_other_parts, 1U, __ATOMIC_RELAXED);
            ESP_LOGD(TAG, "non-audio part (%s)",
                     cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(part, "thought")) ? "thought" :
                     cJSON_GetObjectItemCaseSensitive(part, "executableCode") ? "executableCode" :
                     cJSON_GetObjectItemCaseSensitive(part, "codeExecutionResult") ?
                     "codeExecutionResult" :
                     cJSON_GetObjectItemCaseSensitive(part, "text") ? "text" : "other");
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
    const cJSON *grounding = cJSON_GetObjectItemCaseSensitive(content, "groundingMetadata");
    if (cJSON_IsObject(grounding)) {
        __atomic_add_fetch(&gl->stats.rx_grounding, 1U, __ATOMIC_RELAXED);
        const size_t queries = gl_grounding_merge(&gl->grounding, grounding);
        __atomic_add_fetch(&gl->stats.search_queries, (uint32_t)queries, __ATOMIC_RELAXED);
        emit(gl, APP_GL_EVT_GROUNDING, 0);
    } else if (activity) {
        emit(gl, APP_GL_EVT_MODEL_ACTIVITY, 0);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(content, "turnComplete"))) {
        /* Live models that keep working after a spoken filler send turnComplete
         * with interactionStatus IN_PROGRESS; only IDLE (or no status) ends it. */
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(content, "interactionStatus");
        const bool in_progress = cJSON_IsString(status) &&
                                 strcmp(status->valuestring, "IN_PROGRESS") == 0;
        if (!in_progress && gl->usage.seen) {
            ESP_LOGI(TAG, "turn usage: prompt=%d response=%d tool=%d thoughts=%d total=%d",
                     gl->usage.prompt, gl->usage.response, gl->usage.tool_prompt,
                     gl->usage.thoughts, gl->usage.total);
            gl->usage.seen = false;
        }
        const app_gemini_live_event_t ev = { .type = APP_GL_EVT_TURN_COMPLETE,
                                             .in_progress = in_progress };
        gl->on_event(&ev, gl->ctx);
    }
}

static int usage_field(const cJSON *usage, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(usage, name);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

/* Token accounting (prompt, tool-use prompt from search results, thinking):
 * kept for one INFO line per turn so costs can be followed in the logs. */
static void handle_usage(struct app_gemini_live *gl, const cJSON *usage)
{
    gl->usage.seen = true;
    gl->usage.prompt = usage_field(usage, "promptTokenCount");
    gl->usage.response = usage_field(usage, "responseTokenCount");
    gl->usage.tool_prompt = usage_field(usage, "toolUsePromptTokenCount");
    gl->usage.thoughts = usage_field(usage, "thoughtsTokenCount");
    gl->usage.total = usage_field(usage, "totalTokenCount");
}

/* Each functionCall goes to the owner as APP_GL_EVT_TOOL_CALL (id, name, args
 * JSON). Google Search runs on the server and never appears here. */
static void handle_tool_call(struct app_gemini_live *gl, const cJSON *call)
{
    __atomic_add_fetch(&gl->stats.tool_calls, 1U, __ATOMIC_RELAXED);
    const cJSON *fc;
    cJSON_ArrayForEach(fc, cJSON_GetObjectItemCaseSensitive(call, "functionCalls")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(fc, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(fc, "name");
        if (!cJSON_IsString(name)) {
            continue;
        }
        char *args = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(fc, "args"));
        ESP_LOGI(TAG, "toolCall %s", name->valuestring);
        const app_gemini_live_event_t ev = {
            .type = APP_GL_EVT_TOOL_CALL,
            .tool_id = cJSON_IsString(id) ? id->valuestring : "",
            .tool_name = name->valuestring,
            .text = args ? args : "{}",
        };
        gl->on_event(&ev, gl->ctx);
        cJSON_free(args);
    }
    emit(gl, APP_GL_EVT_MODEL_ACTIVITY, 0);
}

static void handle_tool_cancel(struct app_gemini_live *gl, const cJSON *cancel)
{
    const cJSON *id;
    cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(cancel, "ids")) {
        if (cJSON_IsString(id)) {
            ESP_LOGI(TAG, "toolCallCancellation");
            const app_gemini_live_event_t ev = { .type = APP_GL_EVT_TOOL_CANCEL,
                                                 .tool_id = id->valuestring };
            gl->on_event(&ev, gl->ctx);
        }
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
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usageMetadata");
    if (cJSON_IsObject(usage)) {
        handle_usage(gl, usage);
    }
    const cJSON *tool_call = cJSON_GetObjectItemCaseSensitive(root, "toolCall");
    if (tool_call) {
        handle_tool_call(gl, tool_call);
    }
    const cJSON *tool_cancel = cJSON_GetObjectItemCaseSensitive(root, "toolCallCancellation");
    if (tool_cancel) {
        handle_tool_cancel(gl, tool_cancel);
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
    if (d->op_code == 0x8) {
        /* Close frame: 2-byte status code, then the server's reason text (it
         * explains rejected setups, e.g. code 1011 with an unsupported tool). */
        if (d->data_len > 2) {
            ESP_LOGW(TAG, "server close reason: %.*s", d->data_len - 2, d->data_ptr + 2);
        }
        return;
    }
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
        ESP_LOGI(TAG, "connected; setup %s (model %s%s%s)", sent >= 0 ? "sent" : "FAILED",
                 app_gemini_live_active_model(gl), gl->google_search ? ", google search" : "",
                 gl->resume_handle[0] ? ", resuming" : "");
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
    gl->google_search = config->google_search;
    gl->function_declarations = dup_or_null(config->function_declarations);
    gl->lock = xSemaphoreCreateMutex();
    const esp_err_t grounding_err = gl_grounding_init(&gl->grounding);
    gl->rx = heap_caps_malloc(GL_RX_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gl->pcm = heap_caps_malloc(GL_PCM_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* PSRAM: internal DMA RAM is reserved for the display path. */
    gl->tx = heap_caps_malloc(GL_TX_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gl->model || !gl->lock || !gl->rx || !gl->pcm || !gl->tx || grounding_err != ESP_OK ||
            (config->fallback_model && !gl->fallback_model) ||
            (config->voice && !gl->voice) || (config->language && !gl->language) ||
            (config->system_instruction && !gl->system_instruction) ||
            (config->function_declarations && !gl->function_declarations)) {
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
    gl->usage.seen = false;
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
        .user_agent = "cogitador-m01/1.0",
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

static esp_err_t send_locked_timeout(struct app_gemini_live *gl, const char *text, size_t len,
                                     uint32_t timeout_ms)
{
    if (!gl->client || !gl->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const int sent = esp_websocket_client_send_text(gl->client, text, (int)len,
                                                    pdMS_TO_TICKS(timeout_ms));
    if (sent < 0) {
        if (__atomic_add_fetch(&gl->stats.tx_failures, 1U, __ATOMIC_RELAXED) == 1U) {
            ESP_LOGW(TAG, "first send failure (%u bytes)", (unsigned)len);
        }
        return ESP_FAIL;
    }
    __atomic_add_fetch(&gl->stats.tx_messages, 1U, __ATOMIC_RELAXED);
    return ESP_OK;
}

static esp_err_t send_locked(struct app_gemini_live *gl, const char *text, size_t len)
{
    return send_locked_timeout(gl, text, len, GL_SEND_TIMEOUT_MS);
}

esp_err_t app_gemini_live_send_image(app_gemini_live_handle_t gl, const uint8_t *jpeg, size_t size)
{
    if (!gl || !jpeg || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t prefix = sizeof(IMAGE_PREFIX) - 1;
    const size_t suffix = sizeof(IMAGE_SUFFIX) - 1;
    const size_t b64_cap = 4 * ((size + 2) / 3) + 1;
    char *msg = psram_malloc(prefix + b64_cap + suffix + 1);
    if (!msg) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(msg, IMAGE_PREFIX, prefix);
    size_t b64 = 0;
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (mbedtls_base64_encode((unsigned char *)msg + prefix, b64_cap, &b64, jpeg, size) == 0) {
        memcpy(msg + prefix + b64, IMAGE_SUFFIX, suffix);
        xSemaphoreTake(gl->lock, portMAX_DELAY);
        err = send_locked_timeout(gl, msg, prefix + b64 + suffix, GL_IMAGE_TIMEOUT_MS);
        xSemaphoreGive(gl->lock);
    }
    psram_free(msg);
    ESP_LOGI(TAG, "image %u B (%u B message) %s", (unsigned)size, (unsigned)(prefix + b64 + suffix),
             err == ESP_OK ? "sent" : esp_err_to_name(err));
    return err;
}

esp_err_t app_gemini_live_send_tool_response(app_gemini_live_handle_t gl, const char *tool_id,
                                             const char *tool_name, const char *response_json)
{
    if (!gl || !tool_id || !tool_name || !response_json) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *response = cJSON_Parse(response_json);
    if (!cJSON_IsObject(response)) {
        cJSON_Delete(response);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *tr = cJSON_AddObjectToObject(root, "toolResponse");
    cJSON *list = cJSON_AddArrayToObject(tr, "functionResponses");
    cJSON *fr = cJSON_CreateObject();
    cJSON_AddStringToObject(fr, "id", tool_id);
    cJSON_AddStringToObject(fr, "name", tool_name);
    cJSON_AddItemToObject(fr, "response", response);
    cJSON_AddItemToArray(list, fr);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(gl->lock, portMAX_DELAY);
    const esp_err_t err = send_locked_timeout(gl, text, strlen(text), GL_TOOL_TIMEOUT_MS);
    xSemaphoreGive(gl->lock);
    cJSON_free(text);
    ESP_LOGI(TAG, "toolResponse %s %s", tool_name, err == ESP_OK ? "sent" : esp_err_to_name(err));
    return err;
}

esp_err_t app_gemini_live_send_text(app_gemini_live_handle_t gl, const char *utf8)
{
    if (!gl || !utf8 || !utf8[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gl->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* One complete user turn in the conversation history (clientContent). */
    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_AddObjectToObject(root, "clientContent");
    cJSON *turns = cJSON_AddArrayToObject(content, "turns");
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *parts = cJSON_AddArrayToObject(turn, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", utf8);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(turns, turn);
    cJSON_AddBoolToObject(content, "turnComplete", true);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(gl->lock, portMAX_DELAY);
    const esp_err_t err = send_locked_timeout(gl, text, strlen(text), GL_TOOL_TIMEOUT_MS);
    xSemaphoreGive(gl->lock);
    cJSON_free(text);
    ESP_LOGI(TAG, "clientContent text %s", err == ESP_OK ? "sent" : esp_err_to_name(err));
    return err;
}

esp_err_t app_gemini_live_set_voice(app_gemini_live_handle_t gl, const char *voice)
{
    if (!gl || !voice || !voice[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gl->client) {
        return ESP_ERR_INVALID_STATE; /* the voice is fixed by the setup message */
    }
    char *copy = dup_or_null(voice);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    psram_free(gl->voice);
    gl->voice = copy;
    ESP_LOGI(TAG, "next conversations use voice %s", voice);
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
    out->rx_other_parts = __atomic_load_n(&gl->stats.rx_other_parts, __ATOMIC_RELAXED);
    out->rx_grounding = __atomic_load_n(&gl->stats.rx_grounding, __ATOMIC_RELAXED);
    out->search_queries = __atomic_load_n(&gl->stats.search_queries, __ATOMIC_RELAXED);
    out->tool_calls = __atomic_load_n(&gl->stats.tool_calls, __ATOMIC_RELAXED);
}

size_t app_gemini_live_get_sources(app_gemini_live_handle_t gl, char *out, size_t cap)
{
    return gl ? gl_grounding_copy(&gl->grounding, false, out, cap) : 0;
}

size_t app_gemini_live_get_queries(app_gemini_live_handle_t gl, char *out, size_t cap)
{
    return gl ? gl_grounding_copy(&gl->grounding, true, out, cap) : 0;
}

void app_gemini_live_clear_sources(app_gemini_live_handle_t gl)
{
    if (gl) {
        gl_grounding_clear(&gl->grounding);
    }
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
    psram_free(gl->function_declarations);
    if (gl->grounding.lock) {
        gl_grounding_deinit(&gl->grounding);
    }
    heap_caps_free(gl->rx);
    heap_caps_free(gl->pcm);
    heap_caps_free(gl->tx);
    free(gl);
    *handle = NULL;
    return ESP_OK;
}
