# app_wake

Local "Hola ESP" wake word for Cogitador M-01 with Espressif ESP-SR WakeNet10
(`wn10_es_holaesp`, `espressif/esp-sr` 2.6.2, `lib/esp32s31`). Audio is processed
only in RAM on the device; nothing is sent anywhere.

## Model storage

`CONFIG_SR_WN_WN10_ES_HOLAESP=y` makes esp-sr pack `srmodels.bin` (~418 KB) for
the application-owned `model` partition (data/spiffs, 1 MiB at `0xE00000`). The
project `CMakeLists.txt` registers that image as a System Update data component,
because esp-sr itself only flashes it through `idf.py flash`.

## Contract

- `app_wake_init(cb, ctx)`: singleton, once. Loads the model list with
  `esp_srmodel_init("model")`; returns `ESP_ERR_NOT_FOUND` when the partition or
  model is missing (Cogitador then works with touch only). Starts `vox_wake`
  (core 1, prio 5, below audio capture/playback).
- `app_wake_set_active(bool)`: any task. The wake task creates the WakeNet
  detector (≈16 KB internal RAM, ~0.5 MB PSRAM) only while active and destroys it
  when inactive, so it never competes with a Gemini TLS session for internal RAM.
- `app_wake_feed(pcm, n)`: capture task only (single writer), mono 16 kHz.
  Non-blocking; copies into a 1 s PSRAM stream buffer and drops when full.
- `cb(ctx)` runs on the wake task after a detection and must not block. Listening
  stops immediately on detection; the session re-arms it when idle again.
- Detection mode `DET_MODE_90` (normal). Chunk size and sample rate come from the
  model (`get_samp_chunksize`, `get_samp_rate`) and are logged when listening starts.
