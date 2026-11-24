# app_gemini_live

Native Gemini Live client over a secure WebSocket (no bridge). Uses
`espressif/esp_websocket_client` with the ESP-IDF CA bundle (server certificate
always verified), `espressif/cjson` (PSRAM allocator) and Mbed TLS base64.

## Protocol

- Endpoint: `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=…`
- On connect sends `setup`: `models/<model>`, `generationConfig.responseModalities=["AUDIO"]`,
  `speechConfig` (voice, `languageCode`), `systemInstruction`, input/output
  transcription, automatic activity detection, `contextWindowCompression.slidingWindow`
  and `sessionResumption` (with the latest handle on reconnects). With
  `config.google_search`, also `tools: [{"googleSearch":{}}]` (native Grounding with
  Google Search, executed by Google; no `toolResponse` needed). No `thinkingConfig`
  is sent (`gemini-3.8-live` rejects it; 3.1 keeps its default `minimal`).
- Uplink: `realtimeInput.audio` (`audio/pcm;rate=16000`, base64) per 40 ms frame;
  `audioStreamEnd` when the mic pauses.
- Downlink: reassembles fragmented frames (text or binary) up to 256 KiB, then
  decodes `serverContent` (audio 24 kHz, transcripts, `interrupted`,
  `turnComplete` + `interactionStatus`), `sessionResumptionUpdate`, `goAway` and `error`.
- Non-audio parts (`text`, `thought`, `executableCode`, `codeExecutionResult`) and
  unexpected `toolCall`s are never played; they raise `APP_GL_EVT_MODEL_ACTIVITY`
  (the turn is in progress) and are logged by kind only (DEBUG).
- `groundingMetadata` (private `gl_grounding.c`): unique `webSearchQueries` and source
  domains (`groundingChunks[].web.domain`/`title`) accumulated per answer in PSRAM, read with
  `app_gemini_live_get_queries()` / `_get_sources()`, reset with `_clear_sources()`;
  raises `APP_GL_EVT_GROUNDING`. `searchEntryPoint.renderedContent` cannot be drawn on
  the device (see `docs/GOOGLE_SEARCH.md`).
- `turnComplete` with `interactionStatus: IN_PROGRESS` (spoken filler while the model keeps
  working) is reported with `event.in_progress = true`.
- `usageMetadata`: one INFO line per finished turn (`turn usage: prompt response tool thoughts total`).
- Resilience: automatic reconnect (2 s) including after a clean server close;
  after 2 failed setups the fallback model is used; after 5 a `FAILED` event asks
  the owner to disconnect.

## Contract

- `create` copies configuration and allocates RX (256 KiB) and PCM (192 KiB)
  buffers in PSRAM plus a 6 KiB TX buffer.
- `connect(key)` and `disconnect()` from one application task (the session).
  The key is copied only into the client URI; the local URI buffer is zeroized
  immediately. Transport debug logs are set to WARN so the URI is never printed.
- `send_audio()` / `send_audio_stream_end()` from any task; they share a mutex
  with `disconnect()`. The WebSocket task never takes that mutex, so stop/destroy
  cannot deadlock.
- `on_event` runs on the WebSocket task (core 0, prio 10). It may block briefly
  (playback backpressure) but must never call `disconnect()` or `destroy()`.
- Each `connect()` starts a fresh conversation (no resumption handle carried over).
- `app_gemini_live_swap_model()` (session task, while disconnected) makes the other model the
  starting model for later connections until reboot; used by the session's silent-model watchdog.
- `app_gemini_live_get_stats()` exposes lock-free counters (frames sent/failed, messages,
  audio chunks, input transcriptions, non-audio parts, grounding messages, search queries,
  tool calls) for diagnostics.
- Grounding getters may be called from any task: a dedicated mutex guards short copies
  only and never nests with the send lock. Tests on the PC: `tools/host_tests/run.ps1`.
