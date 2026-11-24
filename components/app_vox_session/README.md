# app_vox_session

Conversation orchestrator for VOX M-01. Owns the session task (core 0, prio 8),
the Gemini Live client and the audio pipeline, and publishes views to a UI sink
supplied by `main`. Depends on `app_audio`, `app_gemini_live`, `app_secrets` and
`app_wifi`; it does not depend on GSP.

## States

| State | View (eye) | Mic | Entered by |
|---|---|---|---|
| CLOSED | idle (cyan) | off | boot, tap to close |
| CONNECTING | processing (amber) | off | tap to open, link lost (auto-resume) |
| LISTENING | listening (red) | on | `setupComplete`, end of VOX speech, tap during speech |
| PROCESSING | processing (amber) | on | first input transcription |
| SPEAKING | speaking (green) | off (+`audioStreamEnd`) | first model audio of a turn |
| ERROR | error (violet) + status | off | no Wi-Fi, no key, repeated link failures |

Taps: CLOSED → open; SPEAKING → interrupt (flush playback, drop the rest of
the turn, listen again); any other state → close. Leaving SPEAKING needs
`turnComplete` plus the playback ring drained (300 ms tail), or 8 s without audio.
PROCESSING falls back to LISTENING after 15 s without a reply.

## Contract

- `app_vox_session_start(&cfg)`: once, after NVS, ESP-Iris, `app_secrets_init()`
  and `app_wifi_start()`. Starts audio and creates the Gemini client.
- `app_vox_session_tap()`, `_wifi_changed()`, `_secrets_changed()` only enqueue
  events (non-blocking, any task including GSP callbacks).
- UI sink callbacks run on the session task.
- Persona, voice (`Charon`), language (`es-ES`) and models come from this
  component (Kconfig `VOX_*`); a provisioned model override takes precedence.
- Transcripts are logged only at DEBUG level.
