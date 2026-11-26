# app_vox_session

Conversation orchestrator for VOX M-01. Owns the session task (core 0, prio 8),
the Gemini Live client and the audio pipeline, and publishes views to a UI sink
supplied by `main`. Depends on `app_audio`, `app_gemini_live`, `app_secrets` and
`app_wifi`; it does not depend on GSP.

## States

| State | View (eye) | Mic | Entered by |
|---|---|---|---|
| CLOSED | idle (cyan) | local only: "Hola ESP" (`app_wake`) | boot, tap to close, 15 s idle |
| CONNECTING | processing (amber) | off (pre-roll after wake word) | tap or "Hola ESP" to open, link lost (auto-resume) |
| LISTENING | listening (red) | on | `setupComplete`, end of VOX speech, tap during speech |
| PROCESSING | processing (amber) | on | first input transcription, non-audio model output (search, thinking), speech drained with the turn still open |
| SPEAKING | speaking (green) | off | first model audio of a turn |
| ERROR | error (violet) + status | off | no Wi-Fi, no key, repeated link failures |

Taps: CLOSED → open; SPEAKING → interrupt (flush playback, drop the rest of
the turn, listen again); any other state → close. Leaving SPEAKING needs
`turnComplete` plus the playback ring drained (300 ms tail); if the ring drains for 2 s
while the turn is still open (e.g. a filler, then a Google search) the eye goes amber
(PROCESSING) instead. Taps are logged.

Pending reply (1.2.0): from the user's turn (first transcription) or any model output
until `turnComplete` (`interactionStatus` not `IN_PROGRESS`), `interrupted`, an interrupting
tap, a reconnect or close. While pending, the idle return does not run. If no model
output arrives for `VOX_RESPONSE_TIMEOUT_S` (30 s), the reply is abandoned and the session
listens again, after which the normal idle countdown applies.

Grounding display (Kconfig `VOX_SHOW_SEARCH_SOURCES`, off by default: quiet search): the status line shows the Search Suggestions
("GOOGLE: ...") then the sources ("FUENTE 1/3: RAE.ES"), formatted by the private
`vox_status_text.c` for the eye's charset (24 characters), rotating every 3 s. They are cleared
at the next question or on close and never stored. Errors take precedence on the line.

Wake word (Kconfig `VOX_WAKE_WORD`): while CLOSED, mic frames go only to `app_wake`;
no WebSocket exists and nothing leaves the device. A detection blinks the eye,
starts the pre-roll (`VOX_PREROLL_MS`, 2 s, PSRAM) and opens the conversation; the
pre-roll frames are sent right after `setupComplete`, before live audio. LISTENING
with no local voice, transcription, model audio, turn or tap for `VOX_IDLE_TIMEOUT_S` (15 s)
closes the conversation and re-arms local listening.

## Contract

- `app_vox_session_start(&cfg)`: once, after NVS, ESP-Iris, `app_secrets_init()`
  and `app_wifi_start()`. Starts audio and creates the Gemini client.
- `app_vox_session_tap()`, `_wifi_changed()`, `_secrets_changed()` only enqueue
  events (non-blocking, any task including GSP callbacks).
- UI sink callbacks run on the session task.
- Silent-model watchdog: after `VOX_SILENT_MODEL_VOICED_FRAMES` voiced uplink frames on a
  connection with no transcription or audio back, reconnect once per boot with the other
  model (`app_gemini_live_swap_model`). Defaults: primary `gemini-3.1-flash-live-preview`,
  fallback `gemini-3.8-live`.
- Persona, voice (`Charon`), language (`es-ES`) and models come from this
  component (Kconfig `VOX_*`); a provisioned model override takes precedence.
- Gallery (1.5.0):
  - Stills are saved with `app_photos` after each `capturar_imagen`, and the photo count is pushed to
    the UI sink (`set_photo_count`).
  - `app_vox_session_gallery(action)` (GSP task) only enqueues. The session reads the JPEG from flash
    on its own task and calls `show_photo` (GSP copies it) or `hide_gallery`.
  - Opening is refused while CONNECTING, PROCESSING or SPEAKING.
  - The gallery opens on the newest photo; ‹ and › wrap around.
  - It closes after 30 s without touches, or when «Hola ESP» is heard.
- Transcripts, search queries and source domains are logged only at DEBUG level.
- Camera tool (Kconfig `VOX_CAMERA`, 1.4.0): the session declares `capturar_imagen` and handles
  each call on its own task.
  - The WebSocket task copies the call into one of 2 slots and posts `EV_TOOL`.
  - During the call: blink, «OBSERVANDO» on the status line, uplink paused.
  - It runs `app_camera_capture_jpeg()`, sends the still with `app_gemini_live_send_image()`, then
    answers with `toolResponse`: a result, or an error such as «no camera» or «capture failed».
  - A cancelled call gets no response.
  - The `diag` line reports `tools{calls=… stills=…}`.
  - The session task stack is 8 KiB.
- RoleMaster (1.6.0). Function declarations are always sent; `capturar_imagen` only with the camera.
  - Roll modes (`vox_rol.c`, host-tested), argument `modo`:
    - `suma`: the dice are added; `critico`/`pifia` on a d20;
    - `porcentaje`: always 2d10 read as tens and units (0 and 0 = 100);
    - `reserva`: count the dice that reach `exito_con` (default `sides - sides/5`: 8 on a d10,
      5 on a d6); the ones and maximums are reported too, so the model applies special rules.
    - 1–10 dice of 2–100 sides; each die comes from `esp_random()` with rejection sampling.
  - `tirar_dados(modo, cantidad, caras, exito_con)`: the master's roll. It is shown with
    `show_roll`, and the response is JSON for the mode (results, total, percentage or successes).
  - `preparar_tirada(modo, cantidad, caras, exito_con)` (role mode only): what the dice badge
    rolls from now on, e.g. one d10 per skill point. The badge label follows («2D20», «D%»,
    «6D10»). Every game starts with 2d20 `suma`.
  - `cambiar_voz(voz)`: one of Charon, Algenib, Kore or Sulafat (genders from Google Cloud
    Chirp 3 HD).
    - The voice is stored in NVS.
    - When the reply has been spoken, the session reconnects with the new voice. It is a fresh
      session, so the context is lost.
  - Voice badge (`app_vox_session_action(VOICE_NEXT)`): cycles the four voices.
    - With the conversation closed, the voice applies at once and the status shows «KORE · CHICA
      FIRME».
    - During a conversation it applies when the conversation closes («PRÓXIMA VOZ: KORE»).
  - `modo_rol(activar)`:
    - role badge on;
    - idle timeout `VOX_ROLE_IDLE_TIMEOUT_S` (180 s);
    - phone page started (`app_rol_web`), with its address on the status line;
    - closing the conversation ends role mode;
    - a tap only interrupts the master: while listening it shows «MANTÉN PARA SALIR». Holding the
      eye ~1.5 s (`APP_VOX_SESSION_ACTION_CLOSE_HOLD`) or asking the master to end the game leaves
      it.
    - the response carries the phone address, lobby instructions and, when a game is saved, its
      title so the master can offer to continue it.
  - `mostrar_secreto(jugador, texto, en_pantalla)`: to that player's phone; if the name has not
    joined yet it waits there for them (`app_rol_web` reserves the name). `en_pantalla=true`, or
    the page being off, shows the screen card (`show_secret`). The response tells the model which.
  - `estado_jugadores()`: phone address, players (`movil`, `secreto_sin_leer`, `ficha`) and the
    saved game's title.
  - `actualizar_ficha(jugador, ficha)`: the whole character sheet shown on the phone (also kept
    for a name that has not joined yet).
  - `guardar_partida(titulo, juego, resumen)`: the master's summary, the sheets, the secrets and
    the prepared roll, kept in NVS (`app_rol_web_store_game`, one game) and downloadable at
    `/partida`.
  - `cargar_partida(modo)`: turns role mode on, restores the players on the page (sheets; secrets
    too with `continuar`, not with `mismos_personajes`) and the prepared roll, and returns the
    whole document to the model (without secrets for `mismos_personajes`).
  - Dice badge (role mode only): the player rolls the prepared roll on the device. The roll is
    shown and sent to Gemini as a `clientContent` text turn (`app_gemini_live_send_text`), e.g.
    «Tirada de reserva del jugador…: 6d10, éxito con 8 o más; resultados …; 3 éxitos, 1 uno y 1
    máximo (10).»
  - Notes for the master: player rolls and phone notices («Aviso de VOX (no lo dice un jugador):
    Ana se ha conectado al móvil.», taken from `app_rol_web_take_notices` every tick) share one
    queue of 1 KiB. While the model streams a reply the WebSocket client is busy and a send fails,
    so notes are sent once `reply_pending` clears, several in one message, retrying every 0.5 s.
    They are dropped after 90 s, on close, or on a voice reconnect (`notes delivered` /
    `notes not delivered (…)`).
  - Up to 8 function calls can be pending at once (slots in PSRAM): the model often sends a secret
    for every player plus a roll in one message. Arguments are copied into the slot (6 KiB each,
    room for the saved-game summary) by the WebSocket task and parsed with cJSON on the session
    task.
- Quiet search (owner's decision): the persona tells Gemini to answer immediately and search only
  when asked or for recent facts it does not know, never mentioning sources or that it searched.
