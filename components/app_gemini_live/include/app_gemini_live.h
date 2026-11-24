// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Gemini Live client; contracts: ../README.md. */
typedef struct app_gemini_live *app_gemini_live_handle_t;

typedef enum {
    APP_GL_EVT_CONNECTING = 0,     /**< TLS/WebSocket handshake started. */
    APP_GL_EVT_READY,              /**< setupComplete received; audio may flow. */
    APP_GL_EVT_AUDIO,              /**< pcm/samples: mono 24 kHz model speech. */
    APP_GL_EVT_INPUT_TRANSCRIPT,   /**< text: partial transcription of the user. */
    APP_GL_EVT_OUTPUT_TRANSCRIPT,  /**< text: partial transcription of the model. */
    APP_GL_EVT_INTERRUPTED,        /**< Server VAD detected barge-in. */
    APP_GL_EVT_TURN_COMPLETE,      /**< Model finished its turn. */
    APP_GL_EVT_GO_AWAY,            /**< Server will close soon; client resumes automatically. */
    APP_GL_EVT_DISCONNECTED,       /**< Link lost; automatic reconnect pending. */
    APP_GL_EVT_FAILED,             /**< Repeated failures; caller should disconnect. */
} app_gemini_live_event_type_t;

typedef struct {
    app_gemini_live_event_type_t type;
    const int16_t *pcm;            /**< APP_GL_EVT_AUDIO only; valid during the callback. */
    size_t samples;
    const char *text;              /**< Transcripts; valid during the callback. */
    int close_code;                /**< RFC 6455 close code or HTTP handshake status. */
} app_gemini_live_event_t;

/** Runs on the WebSocket task. May block briefly (playback backpressure);
 * must never call app_gemini_live_disconnect()/destroy(). */
typedef void (*app_gemini_live_cb_t)(const app_gemini_live_event_t *event, void *ctx);

typedef struct {
    const char *model;              /**< e.g. "gemini-3.8-live" (copied). */
    const char *fallback_model;     /**< Used after repeated setup failures (copied, optional). */
    const char *voice;              /**< Prebuilt voice name, e.g. "Charon" (copied). */
    const char *language;           /**< BCP-47, e.g. "es-ES" (copied). */
    const char *system_instruction; /**< Persona (copied). */
    app_gemini_live_cb_t on_event;  /**< Required. */
    void *ctx;
} app_gemini_live_config_t;

/** Allocate buffers (PSRAM) and copy configuration. */
esp_err_t app_gemini_live_create(const app_gemini_live_config_t *config,
                                 app_gemini_live_handle_t *out_handle);

/** Open a fresh conversation. api_key is copied into the connection URI only;
 * callers should zeroize their copy afterwards. Session task only. */
esp_err_t app_gemini_live_connect(app_gemini_live_handle_t handle, const char *api_key);

/** Close the conversation and free the connection. Never from the event callback. */
esp_err_t app_gemini_live_disconnect(app_gemini_live_handle_t handle);

/** Send one mono 16 kHz PCM frame (<= 1600 samples). Drops when not ready. Any task. */
esp_err_t app_gemini_live_send_audio(app_gemini_live_handle_t handle, const int16_t *pcm,
                                     size_t samples);

/** Signal a paused microphone (automatic VAD). Any task. */
esp_err_t app_gemini_live_send_audio_stream_end(app_gemini_live_handle_t handle);

bool app_gemini_live_is_ready(app_gemini_live_handle_t handle);

/** Cumulative counters since create (diagnostics, any task, lock-free). */
typedef struct {
    uint32_t tx_messages;          /**< text frames sent (setup excluded) */
    uint32_t tx_failures;          /**< send_text errors/timeouts */
    uint32_t tx_not_ready;         /**< audio frames dropped before setupComplete */
    uint32_t rx_messages;          /**< complete server messages */
    uint32_t rx_audio_chunks;      /**< inlineData parts decoded */
    uint32_t rx_input_transcripts; /**< inputTranscription updates */
} app_gemini_live_stats_t;
void app_gemini_live_get_stats(app_gemini_live_handle_t handle, app_gemini_live_stats_t *out);

/** Model currently used for setup (valid until the next connect). */
const char *app_gemini_live_active_model(app_gemini_live_handle_t handle);

/** Disconnect if needed and free everything; clears *handle. */
esp_err_t app_gemini_live_destroy(app_gemini_live_handle_t *handle);

#ifdef __cplusplus
}
#endif
