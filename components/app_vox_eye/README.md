# app_vox_eye

Mechanical eye of VOX M-01: a full-screen GSP scene (`ui/main.json`) with an
animated iris group, pupil, eyelids, status glow ring and a transparent
full-screen tap target. The same controller runs on the device and on the Host
simulator (`host/`). It depends only on the public ESP-GSP API and the bindings
generated from `ui/main.json`; it never imports BSP, networking or `main`.

## Behavior

- 30 fps GSP timer (`EYE_TICK_MS` = 33 ms) on the render task.
- Gaze: random targets inside the socket (range and hold time per state),
  critically damped spring, microsaccades every ~300 ms.
- Blinks every 2.5–7 s (~200 ms, 20 % chance of a double blink) and a blink
  acknowledging each tap.
- State colors (0xRRGGBB): idle `00E5FF`, listening `FF1A1A`, processing
  `FFB000`, speaking `20FF60`, error `B040FF`. The glow ring pulses while
  listening/processing, follows the output level while speaking and flashes on error.
- Pupil diameter per state (50..130 px); while speaking it adds up to 40 px
  from the `level` callback.

## Contract

```c
app_vox_eye_handle_t eye;
const app_vox_eye_config_t cfg = { .on_tap = tapped, .level = level, .seed = esp_random() | 1 };
ESP_ERROR_CHECK(app_vox_eye_create(ui, &cfg, &eye) == ESP_GSP_OK ? ESP_OK : ESP_FAIL);
app_vox_eye_set_state(eye, APP_VOX_EYE_LISTENING);   /* any task */
app_vox_eye_set_status(eye, "SIN WI-FI");            /* any task; "" hides */
```

- Create once from the startup task; the controller claims the UI's single
  application event callback and one timer. `ui` must outlive it.
- `on_tap` runs on the GSP framework task and `level` on the render task: both
  must return quickly and never block.
- `app_vox_eye_set_state()` writes one aligned 32-bit word read by the render
  task (single writer: the session task). Only GSP callbacks touch scene state.
- `app_vox_eye_set_status()` uses a queued GSP setter. Text must use the
  scene charset: uppercase Spanish letters, digits and ` -·.:/`.
- Errors: create returns `ESP_GSP_ERR_INVALID_ARG`, `ESP_GSP_ERR_NO_MEM` or a GSP
  setter/registration error; setter failures during animation are kept in the
  snapshot's `last_error`. Destroy requires quiesced dispatch.

## Simulator

`python mosaico.py project sim --project projects/servocraneo_gemini_direct --interactive`
runs the same controller; the Host adapter cycles the five states every 3 s and
a tap toggles idle/listening. Networking and audio are not simulated.
