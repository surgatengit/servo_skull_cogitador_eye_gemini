// SPDX-License-Identifier: Apache-2.0

#include "app_vox_session.h"

#include <math.h>
#include <string.h>

#include "app_audio.h"
#include "app_gemini_live.h"
#include "app_secrets.h"
#include "app_wake.h"
#include "app_wifi.h"
#include "esp_heap_caps.h"
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
#define IDLE_TIMEOUT_US         ((int64_t)CONFIG_VOX_IDLE_TIMEOUT_S * 1000 * 1000)
#define PREROLL_FRAMES          (CONFIG_VOX_PREROLL_MS / 40)   /* 40 ms uplink frames */

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
    EV_WAKE,
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
    uint32_t voiced_frames; /* atomic: loud uplink frames sent (capture task) */
    uint32_t voiced_base;   /* silent-model watchdog baselines, set at READY */
    uint32_t transcripts_base;
    uint32_t audio_base;
    bool model_swapped;     /* the watchdog swaps at most once per boot */
    uint32_t mic_forward;   /* atomic: forward mic frames to Gemini */
    uint32_t speaking;      /* atomic: first audio of the turn already queued */
    uint32_t squelch;       /* atomic: drop audio of an interrupted turn */
    /* Wake word and inactivity. */
    bool wake_available;
    int64_t last_activity_us;
    uint32_t wake_feed;     /* atomic: idle mic frames go to app_wake (capture task) */
    /* Pre-roll after "Hola ESP": the capture task writes frames while connecting;
     * the session task drains them at READY after clearing preroll_on and waiting
     * for preroll_busy to drop (single writer, single reader). */
    int16_t *preroll;       /* PREROLL_FRAMES * APP_AUDIO_MIC_FRAME, PSRAM */
    uint32_t preroll_count; /* atomic: frames written (monotonic) */
    uint32_t preroll_on;    /* atomic */
    uint32_t preroll_busy;  /* atomic: writer inside a frame copy */
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

/* The capture callback runs while any consumer needs audio: Gemini uplink,
 * local wake-word listening or the post-wake pre-roll. */
static void update_audio_gate(void)
{
    const bool on = __atomic_load_n(&s.mic_forward, __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&s.wake_feed, __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE);
    app_audio_set_mic_enabled(on);
}

static void set_mic(bool on)
{
    __atomic_store_n(&s.mic_forward, on ? 1U : 0U, __ATOMIC_RELEASE);
    update_audio_gate();
}

/* Local "Hola ESP" listening only while the conversation is closed: no WebSocket
 * exists then, and nothing leaves the device. */
static void set_wake_listening(bool on)
{
    on = on && s.wake_available;
    __atomic_store_n(&s.wake_feed, on ? 1U : 0U, __ATOMIC_RELEASE);
    app_wake_set_active(on);
    update_audio_gate();
}

static void set_preroll(bool on)
{
    if (on) {
        __atomic_store_n(&s.preroll_count, 0U, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s.preroll_on, (on && s.preroll) ? 1U : 0U, __ATOMIC_RELEASE);
    update_audio_gate();
}

/* Send the frames captured since "Hola ESP" before live audio (session task). */
static void flush_preroll(void)
{
    if (!__atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&s.preroll_on, 0U, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s.preroll_busy, __ATOMIC_ACQUIRE)) {
        vTaskDelay(1);
    }
    const uint32_t count = __atomic_load_n(&s.preroll_count, __ATOMIC_ACQUIRE);
    const uint32_t n = count < PREROLL_FRAMES ? count : PREROLL_FRAMES;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t slot = (count - n + i) % PREROLL_FRAMES;
        (void)app_gemini_live_send_audio(s.gl, &s.preroll[slot * APP_AUDIO_MIC_FRAME],
                                         APP_AUDIO_MIC_FRAME);
    }
    ESP_LOGI(TAG, "sent %u pre-roll frames (%u ms)", (unsigned)n, (unsigned)(n * 40));
}

static void enter(session_state_t st)
{
    if (st == s.state) {
        return;
    }
    ESP_LOGI(TAG, "state %d -> %d", (int)s.state, (int)st);
    s.state = st;
    s.last_activity_us = esp_timer_get_time();
    if (st != ST_ERROR) {
        s.error = ERR_NONE;
    }
    if (st != ST_CONNECTING) {
        set_preroll(false);
    }
    set_wake_listening(st == ST_CLOSED);
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
    ESP_LOGI(TAG, "tap in state %d", (int)s.state);
    s.last_activity_us = esp_timer_get_time();
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
            app_gemini_live_stats_t stats;
            app_gemini_live_get_stats(s.gl, &stats);
            s.voiced_base = __atomic_load_n(&s.voiced_frames, __ATOMIC_ACQUIRE);
            s.transcripts_base = stats.rx_input_transcripts;
            s.audio_base = stats.rx_audio_chunks;
            __atomic_store_n(&s.squelch, 0U, __ATOMIC_RELEASE);
            flush_preroll(); /* the question spoken right after "Hola ESP" goes first */
            enter(ST_LISTENING);
        }
        break;
    case APP_GL_EVT_INPUT_TRANSCRIPT:
        s.last_activity_us = esp_timer_get_time();
        if (s.state == ST_LISTENING) {
            enter(ST_PROCESSING);
        }
        break;
    case APP_GL_EVT_TURN_COMPLETE:
        s.last_activity_us = esp_timer_get_time();
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
    const bool idle = s.state == ST_CLOSED || s.state == ST_ERROR;
    if (now - s.last_diag_us < (idle ? 2 * DIAG_PERIOD_US : DIAG_PERIOD_US)) {
        return;
    }
    s.last_diag_us = now;
    /* The panel needs contiguous internal DMA bounce buffers on every transfer:
     * report the largest such block next to the free total. */
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const size_t int_free = heap_caps_get_free_size(dma_caps);
    const size_t int_largest = heap_caps_get_largest_free_block(dma_caps);
    if (idle) {
        ESP_LOGI(TAG, "diag st=%d wake=%d heap{int_dma_free=%u largest=%u}", (int)s.state,
                 (int)__atomic_load_n(&s.wake_feed, __ATOMIC_ACQUIRE), (unsigned)int_free,
                 (unsigned)int_largest);
        return;
    }
    app_audio_mic_stats_t mic;
    app_audio_get_mic_stats(&mic);
    app_gemini_live_stats_t gl;
    app_gemini_live_get_stats(s.gl, &gl);
    const float peak_dbfs = mic.peak ? 20.0f * log10f((float)mic.peak / 32768.0f) : -96.0f;
    ESP_LOGI(TAG, "diag st=%d mic{frames=%u peak=%.0f dBFS slot=%u on=%d} "
             "tx{ok=%u fail=%u notready=%u} rx{msgs=%u audio=%u transcripts=%u} "
             "heap{int_dma_free=%u largest=%u}",
             (int)s.state, (unsigned)mic.frames, (double)peak_dbfs, (unsigned)mic.slot,
             (int)mic.enabled, (unsigned)gl.tx_messages, (unsigned)gl.tx_failures,
             (unsigned)gl.tx_not_ready, (unsigned)gl.rx_messages, (unsigned)gl.rx_audio_chunks,
             (unsigned)gl.rx_input_transcripts, (unsigned)int_free, (unsigned)int_largest);
}

/* A model that completes setup but never transcribes (gemini-3.8-live in the
 * 2026-11-24 tests) defeats the setup-failure fallback. If the user has clearly
 * spoken on this connection and no transcription arrived, reconnect once with the
 * other configured model. Returns true when the conversation was reopened. */
static bool check_silent_model(void)
{
    if (s.model_swapped || (s.state != ST_LISTENING && s.state != ST_PROCESSING)) {
        return false;
    }
    const uint32_t voiced = __atomic_load_n(&s.voiced_frames, __ATOMIC_ACQUIRE) - s.voiced_base;
    if (voiced < (uint32_t)CONFIG_VOX_SILENT_MODEL_VOICED_FRAMES) {
        return false;
    }
    app_gemini_live_stats_t stats;
    app_gemini_live_get_stats(s.gl, &stats);
    if (stats.rx_input_transcripts != s.transcripts_base || stats.rx_audio_chunks != s.audio_base) {
        return false; /* the model is listening or answering */
    }
    s.model_swapped = true;
    ESP_LOGW(TAG, "model %s silent after %u voiced frames; switching model",
             app_gemini_live_active_model(s.gl), (unsigned)voiced);
    set_mic(false);
    (void)app_gemini_live_disconnect(s.gl);
    app_audio_flush_playback();
    if (app_gemini_live_swap_model(s.gl) != ESP_OK) {
        fail(ERR_LINK);
        return true;
    }
    s.state = ST_CLOSED;
    open_conversation();
    return true;
}

static void on_tick(void)
{
    const int64_t now = esp_timer_get_time();
    log_diagnostics(now);
    if (check_silent_model()) {
        return;
    }
    if (s.state == ST_LISTENING && !app_audio_is_playing() &&
            now - s.last_activity_us > IDLE_TIMEOUT_US) {
        ESP_LOGI(TAG, "%d s without activity; back to idle", CONFIG_VOX_IDLE_TIMEOUT_S);
        close_conversation();
        return;
    }
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
                /* No audioStreamEnd here: the server already closed the user's turn,
                 * and the send collided with the incoming audio (ws-client lock). */
                if (s.state == ST_LISTENING || s.state == ST_PROCESSING) {
                    enter(ST_SPEAKING);
                }
                break;
            case EV_WAKE:
                if (s.state == ST_CLOSED) {
                    ESP_LOGI(TAG, "\"Hola ESP\" detected; opening conversation");
                    if (s.config.ui.acknowledge) {
                        s.config.ui.acknowledge(s.config.ui.ui);
                    }
                    set_preroll(true);
                    open_conversation();
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
/* Wake task: never block. */
static void on_wake(void *ctx)
{
    (void)ctx;
    post(EV_WAKE, 0, 0);
}

static void on_mic(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    if (!__atomic_load_n(&s.mic_forward, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE) && samples == APP_AUDIO_MIC_FRAME) {
            __atomic_store_n(&s.preroll_busy, 1U, __ATOMIC_RELEASE);
            if (__atomic_load_n(&s.preroll_on, __ATOMIC_ACQUIRE)) {
                const uint32_t n = __atomic_load_n(&s.preroll_count, __ATOMIC_RELAXED);
                memcpy(&s.preroll[(n % PREROLL_FRAMES) * APP_AUDIO_MIC_FRAME], pcm,
                       samples * sizeof(int16_t));
                __atomic_store_n(&s.preroll_count, n + 1U, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&s.preroll_busy, 0U, __ATOMIC_RELEASE);
        } else if (__atomic_load_n(&s.wake_feed, __ATOMIC_ACQUIRE)) {
            app_wake_feed(pcm, samples);
        }
        return;
    }
    if (app_gemini_live_send_audio(s.gl, pcm, samples) == ESP_OK) {
        int peak = 0;
        for (size_t i = 0; i < samples; ++i) {
            const int v = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
            peak = v > peak ? v : peak;
        }
        if (peak >= CONFIG_VOX_VOICED_PEAK_THRESHOLD) {
            __atomic_add_fetch(&s.voiced_frames, 1U, __ATOMIC_RELEASE);
        }
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
#if CONFIG_VOX_WAKE_WORD
    /* Optional: without the model partition VOX keeps working with touch only. */
    s.preroll = heap_caps_calloc((size_t)PREROLL_FRAMES * APP_AUDIO_MIC_FRAME, sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.wake_available = app_wake_init(on_wake, NULL) == ESP_OK;
    if (!s.wake_available) {
        ESP_LOGW(TAG, "wake word unavailable; touch only");
    }
#endif
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
    set_wake_listening(true); /* starts CLOSED: arm local listening */
    ESP_LOGI(TAG, "VOX M-01 ready (model %s, fallback %s); tap%s to talk", primary, fallback,
             s.wake_available ? " or say \"Hola ESP\"" : "");
    return ESP_OK;

fail_gl:
    (void)app_gemini_live_destroy(&s.gl);
fail_queue:
    vQueueDelete(s.queue);
    s.queue = NULL;
    return err;
}
