# app_vox_eye

Mechanical eye of Cogitador M-01: a full-screen GSP scene (`ui/main.json`) with an
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

## RoleMaster controls (1.6.0)

- Voice badge (top right, mirrors the photo counter): shows the voice's initial
  (`app_vox_eye_set_voice_initial`). A tap reports `APP_VOX_EYE_ACTION_VOICE_NEXT` through
  `on_action`.
- Role badge (right bezel, hidden): shown in role mode (`app_vox_eye_set_role_mode`). Its label
  is the prepared roll, «2D20», «D%» or «6D10» (`app_vox_eye_set_role_badge`). A tap reports
  `APP_VOX_EYE_ACTION_ROLE_ROLL`.
- Dice panel (`app_vox_eye_show_roll`):
  - up to 10 dice of 64 px: one centred row up to 5, two rows from 6 (the top row takes the odd
    die). The `dieN_slot` containers have a ranged x and y (`_set_position`);
  - the caller supplies the face texts, a mark per die (gold, red or none), the title and the
    result line (`vox_rol_view()` in `app_vox_session` builds them for the three roll modes);
  - the render task tumbles random numbers 1..`tumble` for ~0.8 s, then shows the faces, the
    marks and the result;
  - it hides after ~6 s, or on a tap once the dice have settled;
  - the caller hands the roll over through a sequence counter (release/acquire on the device,
    plain volatile in the single-threaded PC preview).
- Secret card (`app_vox_eye_show_secret`):
  - full screen, «SOLO PARA <NOMBRE>»;
  - the text is mapped to the card charset and word-wrapped by `secret_text.c` (27 columns,
    11 lines; host-tested);
  - it stays until it is tapped.
- The eye animation pauses under the dice panel and the secret card, as it does under the gallery.
- In role mode (device only, input interceptor), a press held ~1.5 s on the eye, outside the badges
  and overlays, reports `APP_VOX_EYE_ACTION_CLOSE_HOLD`, and its release is swallowed.
- PC preview: the voice badge cycles C/A/K/S and shows a secret on the fourth voice. The dice
  badge cycles a 2d20 critical, a 1d20 fumble, a percentile roll, a 6d10 pool and a 10d10 pool
  (the preview builds `app_vox_session/vox_rol.c` for this).

## Photo counter and gallery (1.5.0)

- **Counter:** `photo_badge` / `photo_count` on the bezel at about 10:30 (60,60 56×56), above the
  full-screen `tap` button. It is hidden while the count is 0.
  - `app_vox_eye_set_photo_count()` updates it.
  - A tap on it raises `APP_VOX_GALLERY_OPEN` through `config.on_gallery`.
- **Gallery:** the `gallery` container covers the screen and starts hidden.
  - Contents: the `gallery_photo` image (264×352, bound resource with a 240×320 placeholder that fixes
    the pixel format), the X (`gallery_close`), the arrows (`gallery_prev` / `gallery_next`) and the
    position (`gallery_index`, "n / m").
  - A full-screen `gallery_bg` button swallows other touches (`APP_VOX_GALLERY_TOUCH`), so no tap
    reaches the eye's `tap` while the gallery is shown.
  - `app_vox_eye_show_photo()` copies the JPEG (GSP decodes it), sets the position and shows the
    gallery. `app_vox_eye_hide_gallery()` hides it.
  - The eye animation pauses while the gallery is shown: no repaints and no DMA traffic underneath.
- **Delete all:** holding the X for 1.5 s raises `APP_VOX_GALLERY_DELETE_ALL`.
  - It is detected through the GSP input interceptor (`esp_gsp_set_input_interceptor`): a press that
    starts on the X is timed with the animation timer, which keeps ticking while the eye is paused.
    The action fires while the X is still held, and the release is swallowed so it does not also
    close the gallery as a normal tap.
  - Device only (`ESP_PLATFORM`): the PC sim_bridge has no interceptor, so there the X only closes.
- **PC preview:** `host/platform_pc.c` sets 3 demo photos and runs the gallery flow with the
  placeholder picture.
  - Scripted simulator runs (2026-11-25) checked: open (3/3), ‹ (2/3), › with wrap (1/3), X, and a
    tap on the photo that does not start a conversation.
