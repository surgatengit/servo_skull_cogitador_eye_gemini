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
    APP_GL_EVT_MODEL_ACTIVITY,     /**< Non-audio model output (text, thought, code, tool
                                        call, generationComplete): the turn is in progress. */
    APP_GL_EVT_GROUNDING,          /**< groundingMetadata received; sources updated
                                        (see app_gemini_live_get_sources()). */
    APP_GL_EVT_TOOL_CALL,          /**< tool_id/tool_name/text(args JSON): a declared function
                                        to run; answer with app_gemini_live_send_tool_response(). */
    APP_GL_EVT_TOOL_CANCEL,        /**< tool_id: the server cancelled that pending call. */
} app_gemini_live_event_type_t;

typedef struct {
    app_gemini_live_event_type_t type;
    const int16_t *pcm;            /**< APP_GL_EVT_AUDIO only; valid during the callback. */
    size_t samples;
    const char *text;              /**< Transcripts; valid during the callback. */
    int close_code;                /**< RFC 6455 close code or HTTP handshake status. */
    bool in_progress;              /**< TURN_COMPLETE only: interactionStatus IN_PROGRESS,
                                        more output follows (fillers while searching). */
    const char *tool_id;           /**< TOOL_CALL / TOOL_CANCEL; valid during the callback. */
    const char *tool_name;         /**< TOOL_CALL; valid during the callback. */
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
    bool google_search;             /**< Declare the native googleSearch tool in setup. */
    const char *function_declarations; /**< JSON array of functionDeclarations (copied,
                                            optional); calls arrive as APP_GL_EVT_TOOL_CALL. */
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

/** Send one JPEG still as realtimeInput video (base64; message built in PSRAM).
 * Blocks up to ~3 s while it is written; the caller should pause its own audio
 * uplink meanwhile. Any task except the event callback. */
esp_err_t app_gemini_live_send_image(app_gemini_live_handle_t handle, const uint8_t *jpeg,
                                     size_t size);

/** Answer a TOOL_CALL: response_json is the JSON object for functionResponses[].response
 * (for example {"result":"ok"}). Any task except the event callback. */
esp_err_t app_gemini_live_send_tool_response(app_gemini_live_handle_t handle, const char *tool_id,
                                             const char *tool_name, const char *response_json);

/** Send one complete user text turn (clientContent, turnComplete). The model
 * answers it like spoken input. ESP_ERR_INVALID_STATE before setupComplete.
 * Any task except the event callback. */
esp_err_t app_gemini_live_send_text(app_gemini_live_handle_t handle, const char *utf8);

/** Prebuilt voice for later connections (copied). Call while disconnected:
 * ESP_ERR_INVALID_STATE otherwise. Session task only. */
esp_err_t app_gemini_live_set_voice(app_gemini_live_handle_t handle, const char *voice);

/** Signal a paused microphone (automatic VAD). Any task. */
esp_err_t app_gemini_live_send_audio_stream_end(app_gemini_live_handle_t handle);

bool app_gemini_live_is_ready(app_gemini_live_handle_t handle);

/** Make the other configured model the starting model of later connections
 * (sticky until reboot). Call while disconnected; ESP_ERR_NOT_SUPPORTED without
 * a fallback model. Session task only. */
esp_err_t app_gemini_live_swap_model(app_gemini_live_handle_t handle);

/** Cumulative counters since create (diagnostics, any task, lock-free). */
typedef struct {
    uint32_t tx_messages;          /**< text frames sent (setup excluded) */
    uint32_t tx_failures;          /**< send_text errors/timeouts */
    uint32_t tx_not_ready;         /**< audio frames dropped before setupComplete */
    uint32_t rx_messages;          /**< complete server messages */
    uint32_t rx_audio_chunks;      /**< inlineData parts decoded */
    uint32_t rx_input_transcripts; /**< inputTranscription updates */
    uint32_t rx_other_parts;       /**< non-audio modelTurn parts (text, thought, code) */
    uint32_t rx_grounding;         /**< messages carrying groundingMetadata */
    uint32_t search_queries;       /**< webSearchQueries reported by the server */
    uint32_t tool_calls;           /**< toolCall messages (none expected: search is server side) */
} app_gemini_live_stats_t;
void app_gemini_live_get_stats(app_gemini_live_handle_t handle, app_gemini_live_stats_t *out);

/** Grounding of the current answer, accumulated since the last
 * app_gemini_live_clear_sources(). Both getters copy unique entries separated
 * by '\n' into out (whole entries only, NUL-terminated) and return how many
 * were copied. Any task.
 * - sources: web domains of groundingChunks (lowercase, "www." stripped);
 * - queries: webSearchQueries, i.e. the Google Search Suggestions (UTF-8). */
size_t app_gemini_live_get_sources(app_gemini_live_handle_t handle, char *out, size_t cap);
size_t app_gemini_live_get_queries(app_gemini_live_handle_t handle, char *out, size_t cap);

/** Forget sources and queries (call at the start of each user turn). Any task. */
void app_gemini_live_clear_sources(app_gemini_live_handle_t handle);

/** Model currently used for setup (valid until the next connect). */
const char *app_gemini_live_active_model(app_gemini_live_handle_t handle);

/** Disconnect if needed and free everything; clears *handle. */
esp_err_t app_gemini_live_destroy(app_gemini_live_handle_t *handle);

#ifdef __cplusplus
}
#endif
