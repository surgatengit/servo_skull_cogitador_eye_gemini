# app_audio

ES8311 audio for VOX M-01 on the BSP's shared 48 kHz / 16-bit / stereo I2S bus.
Depends on `esp-mosaico-bsp` (codec devices), FreeRTOS and esp_timer.

```
MIC  48k stereo → louder slot → 47-tap FIR (7 kHz) ÷3 → 16 kHz mono, 640-sample frames → on_mic()
SPK  ring 24 kHz (8 s, PSRAM) → 47-tap polyphase FIR ×2 (11 kHz) → 48 kHz → stereo → I2S TX
```

## Contract

- `app_audio_start(&cfg)`: singleton, once. Opens both codec devices and starts
  `vox_mic` (prio 18) and `vox_play` (prio 19) on core 1.
- `on_mic(pcm, 640, ctx)` runs on the capture task every 40 ms while enabled
  (`app_audio_set_mic_enabled`). Keep it short; `pcm` is valid only during the call.
- `app_audio_play(pcm24k, n, timeout_ms)`: single producer. Blocks while the ring
  is full; returns `ESP_ERR_TIMEOUT` (remaining samples dropped) after the timeout.
- `app_audio_flush_playback()`, `app_audio_is_playing()` (queued audio or within
  `playback_tail_ms`, default 300 ms) and `app_audio_output_level()` (0..100) are
  lock-free and callable from any task.
- Cross-task state uses GCC `__atomic` builtins; the ring is single-producer /
  single-consumer. Buffers are instance-owned; no large stack arrays.
- Defaults: mic gain 30 dB, volume 70 (Kconfig `VOX_MIC_GAIN_DB`, `VOX_SPEAKER_VOLUME`).

## Limits

No acoustic echo cancellation: the BSP configures the ES8311 without a DAC
reference. The session therefore mutes the uplink while playing (phase 1).
