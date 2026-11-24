// SPDX-License-Identifier: Apache-2.0

#include "app_vox_session.h"

#include <math.h>
#include <string.h>

#include "app_audio.h"
#include "app_gemini_live.h"
#include "app_secrets.h"
#include "app_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"

static const char *TAG = "vox_session";

#define SESSION_TASK_STACK      6144
#define SESSION_TASK_PRIO       8
#define SESSION_TASK_CORE       0
#define SESSION_QUEUE_LEN       24
#define SESSION_TICK_MS         50
#define PROCESSING_TIMEOUT_US   (15 * 1000 * 1000)
#define SPEAKING_STALL_US       (8 * 1000 * 1000)
#define CONNECTING_TIMEOUT_US   (45 * 1000 * 1000)
#define SQUELCH_MAX_US          (10 * 1000 * 1000)
#define DIAG_PERIOD_US          (5 * 1000 * 1000)
#define PLAY_TIMEOUT_MS         1500

static const char PERSONA[] =
    "Eres VOX M-01, un servocráneo del Adeptus Mechanicus al servicio de tu operador. "
    "Hablas siempre en español de España, con tono solemne, ceremonial y preciso, como un "
    "archivo vivo de la Forja. De vez en cuando, sin abusar, haces referencias al Omnissiah "
    "o al Dios Máquina. Ante todo eres útil: respondes con claridad y exactitud a cualquier "
    "pregunta legítima, incluidas operaciones matemáticas sencillas, datos, explicaciones y "
    "conversación cotidiana. Tus respuestas habladas son breves, normalmente de dos a cuatro "
    "frases, salvo que se te pida más detalle. Si no sabes algo, lo reconoces y no inventas.";

typedef enum {
    ST_CLOSED = 0,
    ST_CONNECTING,
    ST_LISTENING,
    ST_PROCESSING,
    ST_SPEAKING,
    ST_ERROR,
} session_state_t;

typedef enum {
    ERR_NONE = 0,
    ERR_NO_WIFI,
    ERR_WIFI_UNCONFIGURED,
    ERR_NO_KEY,
    ERR_KEY_REJECTED,
    ERR_LINK,
    ERR_INTERNAL,
} session_error_t;

typedef enum {
    EV_TAP = 0,
    EV_WIFI,
    EV_SECRETS,
    EV_GL,
    EV_AUDIO_START,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    int value;      /* wifi state or GL event type */
    int code;       /* GL close code */
} session_event_t;

/* Singleton. Session-task-only fields: state, error, timers, config.
 * Cross-task flags (WebSocket/mic tasks) use __atomic builtins. */
static struct {
    bool started;
    app_vox_session_config_t config;
    QueueHandle_t queue;
    app_gemini_live_handle_t gl;
    session_state_t state;
    session_error_t error;
    int wifi_state;
    int64_t processing_since;
    int64_t speaking_since;
    int64_t connecting_since;
    int64_t squelch_since;
    int64_t last_diag_us;
    bool turn_done;
    uint32_t mic_forward;   /* atomic: forward mic frames to Gemini */
    uint32_t speaking;      /* atomic: first audio of the turn already queued */
    uint32_t squelch;       /* atomic: drop audio of an interrupted turn */
} s;

static void post(event_kind_t kind, int value, int code)
{
    if (!s.queue) {
        return;
    }
    const session_event_t ev = { .kind = kind, .value = value, .code = code };
    if (xQueueSend(s.queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full; dropped kind %d", (int)kind);
    }
}

static app_vox_view_t view_for(session_state_t st)
{
    switch (st) {
    case ST_CLOSED:     return APP_VOX_VIEW_IDLE;
    case ST_CONNECTING: return APP_VOX_VIEW_PROCESSING;
    case ST_LISTENING:  return APP_VOX_VIEW_LISTENING;
    case ST_PROCESSING: return APP_VOX_VIEW_PROCESSING;
    case ST_SPEAKING:   return APP_VOX_VIEW_SPEAKING;
    case ST_ERROR:
    default:            return APP_VOX_VIEW_ERROR;
    }
}

static const char *status_for(session_error_t err)
{
    switch (err) {
    case ERR_NO_WIFI:           return "SIN WI-FI";
    case ERR_WIFI_UNCONFIGURED: return "CONFIGURA WI-FI EN VIBE";
    case ERR_NO_KEY:            return "SIN CLAVE";
    case ERR_KEY_REJECTED:      return "CLAVE RECHAZADA";
    case ERR_LINK:              return "ERROR DE CONEXIÓN";
    case ERR_INTERNAL:          return "ERROR INTERNO";
    case ERR_NONE:
    default:                    return "";
    }
}

static void set_mic(bool on)
{
    __atomic_store_n(&s.mic_forward, on ? 1U : 0U, __ATOMIC_RELEASE);
    app_audio_set_mic_enabled(on);
}

static void enter(session_state_t st)
{
    if (st == s.state) {
        return;
    }
    ESP_LOGI(TAG, "state %d -> %d", (int)s.state, (int)st);
    s.state = st;
    if (st != ST_ERROR) {
        s.error = ERR_NONE;
    }
    set_mic(st == ST_LISTENING || st == ST_PROCESSING);
    if (st == ST_CONNECTING) {
        s.connecting_since = esp_timer_get_time();
    }
    if (st == ST_PROCESSING) {
        s.processing_since = esp_timer_get_time();
        /* The user started a new turn: never keep dropping replies to it. */
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    }
    if (st == ST_SPEAKING) {
        s.speaking_since = esp_timer_get_time();
        s.turn_done = false;
    } else {
        __atomic_store_n(&s.speaking, 0U, __ATOMIC_RELEASE);
    }
    if (s.config.ui.set_view) {
        s.config.ui.set_view(s.config.ui.ui, view_for(st));
    }
    if (s.config.ui.set_status) {
        s.config.ui.set_status(s.config.ui.ui, status_for(s.error));
    }
}

static void fail(session_error_t err)
{
    s.error = err;
    s.state = ST_CLOSED; /* force enter() to repaint */
    enter(ST_ERROR);
}

static void close_conversation(void)
{
    set_mic(false);
    (void)app_gemini_live_disconnect(s.gl);
    app_audio_flush_playback();
    __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    enter(ST_CLOSED);
}

static void open_conversation(void)
{
    if (s.wifi_state == APP_WIFI_UNCONFIGURED) {
        fail(ERR_WIFI_UNCONFIGURED);
        return;
    }
    if (s.wifi_state != APP_WIFI_CONNECTED) {
        fail(ERR_NO_WIFI);
        return;
    }
    char key[APP_SECRETS_API_KEY_CAPACITY];
    esp_err_t err = app_secrets_load_api_key(key, sizeof(key));
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(key, sizeof(key));
        ESP_LOGW(TAG, "API key unavailable (%s); provision it with tools/provision_key.py",
                 esp_err_to_name(err));
        fail(ERR_NO_KEY);
        return;
    }
    __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    enter(ST_CONNECTING);
    err = app_gemini_live_connect(s.gl, key);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed (%s)", esp_err_to_name(err));
        fail(ERR_LINK);
    }
}

static void on_tap(void)
{
    switch (s.state) {
    case ST_CLOSED:
        open_conversation();
        break;
    case ST_SPEAKING:
        /* Interrupt VOX without closing: drop queued speech and listen again. */
        __atomic_store_n(&s.squelch, 1U, __ATOMIC_RELEASE);
        s.squelch_since = esp_timer_get_time();
        app_audio_flush_playback();
        enter(ST_LISTENING);
        break;
    case ST_ERROR:
        /* Acknowledge the error; disconnect is a no-op when no link exists. */
        (void)app_gemini_live_disconnect(s.gl);
        enter(ST_CLOSED);
        break;
    case ST_CONNECTING:
    case ST_LISTENING:
    case ST_PROCESSING:
    default:
        close_conversation();
        break;
    }
}

static void on_gl(int type, int code)
{
    const bool open = s.state != ST_CLOSED && s.state != ST_ERROR;
    switch ((app_gemini_live_event_type_t)type) {
    case APP_GL_EVT_READY:
        if (open) {
            __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_INPUT_TRANSCRIPT:
        if (s.state == ST_LISTENING) {
            enter(ST_PROCESSING);
        }
        break;
    case APP_GL_EVT_TURN_COMPLETE:
        s.turn_done = true;
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
        if (s.state == ST_PROCESSING) {
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_INTERRUPTED:
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
        if (s.state == ST_SPEAKING) {
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_DISCONNECTED:
        if (open) {
            app_audio_flush_playback();
            enter(ST_CONNECTING); /* client reconnects and resumes automatically */
        }
        break;
    case APP_GL_EVT_FAILED:
        ESP_LOGE(TAG, "Gemini Live unavailable (code %d)", code);
        (void)app_gemini_live_disconnect(s.gl);
        app_audio_flush_playback();
        fail(code == 401 || code == 403 ? ERR_KEY_REJECTED : ERR_LINK);
        break;
    default:
        break;
    }
}

/* One line every DIAG_PERIOD_US while a conversation is open: enough to tell a
 * silent microphone, failing sends and a silent server apart. */
static void log_diagnostics(int64_t now)
{
    if (s.state == ST_CLOSED || s.state == ST_ERROR || now - s.last_diag_us < DIAG_PERIOD_US) {
        return;
    }
    s.last_diag_us = now;
    app_audio_mic_stats_t mic;
    app_audio_get_mic_stats(&mic);
    app_gemini_live_stats_t gl;
    app_gemini_live_get_stats(s.gl, &gl);
    const float peak_dbfs = mic.peak ? 20.0f * log10f((float)mic.peak / 32768.0f) : -96.0f;
    ESP_LOGI(TAG, "diag st=%d mic{frames=%u peak=%.0f dBFS slot=%u on=%d} "
             "tx{ok=%u fail=%u notready=%u} rx{msgs=%u audio=%u transcripts=%u}",
             (int)s.state, (unsigned)mic.frames, (double)peak_dbfs, (unsigned)mic.slot,
             (int)mic.enabled, (unsigned)gl.tx_messages, (unsigned)gl.tx_failures,
             (unsigned)gl.tx_not_ready, (unsigned)gl.rx_messages, (unsigned)gl.rx_audio_chunks,
             (unsigned)gl.rx_input_transcripts);
}

static void on_tick(void)
{
    const int64_t now = esp_timer_get_time();
    log_diagnostics(now);
    if (__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE) && now - s.squelch_since > SQUELCH_MAX_US) {
        /* The interrupted turn never reported completion: stop discarding audio. */
        __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
    }
    if (s.state == ST_CONNECTING && now - s.connecting_since > CONNECTING_TIMEOUT_US) {
        ESP_LOGW(TAG, "connection not ready after %d s; giving up",
                 (int)(CONNECTING_TIMEOUT_US / 1000000));
        (void)app_gemini_live_disconnect(s.gl);
        app_audio_flush_playback();
        fail(ERR_LINK);
        return;
    }
    if (s.state == ST_SPEAKING) {
        const bool playing = app_audio_is_playing();
        if (!playing && (s.turn_done || now - s.speaking_since > SPEAKING_STALL_US)) {
            enter(ST_LISTENING);
        }
    } else if (s.state == ST_PROCESSING && now - s.processing_since > PROCESSING_TIMEOUT_US) {
        ESP_LOGW(TAG, "no response; listening again");
        enter(ST_LISTENING);
    }
}

static void session_task(void *arg)
{
    (void)arg;
    session_event_t ev;
    for (;;) {
        if (xQueueReceive(s.queue, &ev, pdMS_TO_TICKS(SESSION_TICK_MS)) == pdTRUE) {
            switch (ev.kind) {
            case EV_TAP:
                on_tap();
                break;
            case EV_WIFI:
                s.wifi_state = ev.value;
                if (s.state == ST_ERROR &&
                        (s.error == ERR_NO_WIFI || s.error == ERR_WIFI_UNCONFIGURED) &&
                        ev.value == APP_WIFI_CONNECTED) {
                    enter(ST_CLOSED);
                }
                break;
            case EV_SECRETS:
                if (s.state == ST_ERROR && (s.error == ERR_NO_KEY || s.error == ERR_KEY_REJECTED) &&
                        app_secrets_has_api_key()) {
                    enter(ST_CLOSED);
                }
                break;
            case EV_GL:
                on_gl(ev.value, ev.code);
                break;
            case EV_AUDIO_START:
                if (s.state == ST_LISTENING || s.state == ST_PROCESSING) {
                    enter(ST_SPEAKING);
                    (void)app_gemini_live_send_audio_stream_end(s.gl);
                }
                break;
            }
        }
        on_tick();
    }
}

/* WebSocket task: keep it short; audio goes straight to the playback ring. */
static void gl_event(const app_gemini_live_event_t *event, void *ctx)
{
    (void)ctx;
    switch (event->type) {
    case APP_GL_EVT_AUDIO:
        if (__atomic_load_n(&s.squelch, __ATOMIC_ACQUIRE) || event->samples == 0) {
            return;
        }
        (void)app_audio_play(event->pcm, event->samples, PLAY_TIMEOUT_MS);
        if (!__atomic_exchange_n(&s.speaking, 1U, __ATOMIC_ACQ_REL)) {
            post(EV_AUDIO_START, 0, 0);
        }
        return;
    case APP_GL_EVT_INTERRUPTED:
        app_audio_flush_playback();
        break;
    case APP_GL_EVT_INPUT_TRANSCRIPT:
        ESP_LOGD(TAG, "user: %s", event->text);
        break;
    case APP_GL_EVT_OUTPUT_TRANSCRIPT:
        ESP_LOGD(TAG, "vox: %s", event->text);
        return;
    default:
        break;
    }
    post(EV_GL, (int)event->type, event->close_code);
}

/* Capture task: forward 40 ms frames while listening. */
static void on_mic(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    if (__atomic_load_n(&s.mic_forward, __ATOMIC_ACQUIRE)) {
        (void)app_gemini_live_send_audio(s.gl, pcm, samples);
    }
}

void app_vox_session_tap(void)
{
    post(EV_TAP, 0, 0);
}

void app_vox_session_wifi_changed(int app_wifi_state)
{
    post(EV_WIFI, app_wifi_state, 0);
}

void app_vox_session_secrets_changed(void)
{
    post(EV_SECRETS, 0, 0);
}

uint8_t app_vox_session_output_level(void)
{
    return app_audio_output_level();
}

esp_err_t app_vox_session_start(const app_vox_session_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    s.config = *config;
    s.state = ST_CLOSED;
    s.queue = xQueueCreate(SESSION_QUEUE_LEN, sizeof(session_event_t));
    if (!s.queue) {
        return ESP_ERR_NO_MEM;
    }
    /* Read after the queue exists: later changes arrive as EV_WIFI events. */
    s.wifi_state = app_wifi_get_state();

    /* Model: provisioned override first, the other approved model as fallback. */
    char override[APP_SECRETS_MODEL_CAPACITY];
    const char *primary = CONFIG_VOX_GEMINI_MODEL;
    const char *fallback = CONFIG_VOX_GEMINI_FALLBACK_MODEL;
    if (app_secrets_load_model(override, sizeof(override)) == ESP_OK) {
        primary = override;
        if (strcmp(override, CONFIG_VOX_GEMINI_MODEL) != 0) {
            fallback = CONFIG_VOX_GEMINI_MODEL;
        }
    }
    const app_gemini_live_config_t gl_config = {
        .model = primary,
        .fallback_model = fallback,
        .voice = CONFIG_VOX_GEMINI_VOICE,
        .language = CONFIG_VOX_GEMINI_LANGUAGE,
        .system_instruction = PERSONA,
        .on_event = gl_event,
    };
    esp_err_t err = app_gemini_live_create(&gl_config, &s.gl);
    if (err != ESP_OK) {
        goto fail_queue;
    }
    const app_audio_config_t audio = {
        .on_mic = on_mic,
        .mic_gain_db = config->mic_gain_db > 0.0f ? config->mic_gain_db : (float)CONFIG_VOX_MIC_GAIN_DB,
        .volume = config->volume > 0 ? config->volume : CONFIG_VOX_SPEAKER_VOLUME,
    };
    err = app_audio_start(&audio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable (%s)", esp_err_to_name(err));
        goto fail_gl;
    }
    if (xTaskCreatePinnedToCore(session_task, "vox_session", SESSION_TASK_STACK, NULL,
                                SESSION_TASK_PRIO, NULL, SESSION_TASK_CORE) != pdPASS) {
        /* Audio tasks keep running on silence; nothing else references gl yet. */
        err = ESP_ERR_NO_MEM;
        goto fail_gl;
    }
    s.started = true;
    if (s.config.ui.set_view) {
        s.config.ui.set_view(s.config.ui.ui, APP_VOX_VIEW_IDLE);
    }
    ESP_LOGI(TAG, "VOX M-01 ready (model %s, fallback %s); tap to talk", primary, fallback);
    return ESP_OK;

fail_gl:
    (void)app_gemini_live_destroy(&s.gl);
fail_queue:
    vQueueDelete(s.queue);
    s.queue = NULL;
    return err;
}
