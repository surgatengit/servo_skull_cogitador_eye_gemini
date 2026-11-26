# app_camera

On-demand JPEG stills from the ESP-Mosaico camera module (left slot) for Gemini Live.
Used by `app_vox_session` when Gemini calls the `capturar_imagen` function.
Kconfig: `VOX_CAMERA*`.

## Pipeline (`app_camera_capture_jpeg`)

1. `mosaico_camera_new/open/start_stream`. This uses the BSP `mosaico_module_camera`, vendored
   verbatim in `third_party/`. The capture is requested as 1280×720 UYVY with 2 buffers, which both
   module versions support.
   - Between `new` and `open`, with `VOX_CAMERA_SENSOR_RESET` (default y), the sensor gets a
     hardware reset (10 ms low on `CAM_RESET`, GPIO53, per the official V1.4 schematic; the driver
     ignores this pin), then a soft reset (`ESP_CAM_SENSOR_IOC_SW_RESET`), and its format is
     written again. A software restart does
     not power the camera board off and `esp_video` never resets the sensor (the legacy
     `esp32-camera` driver always did); without it the SC101IOT gave no frames after `iris restart`.
2. Discard `VOX_CAMERA_SETTLE_FRAMES` frames (8 by default) so auto exposure settles, then grab one
   frame.
3. **PPA** (hardware) crop, scale and rotate: UYVY to RGB565 `VOX_CAMERA_WIDTH`×`HEIGHT` (240×320
   portrait), with 270° rotation and a vertical mirror by default, like the BSP `camera_photo_app`
   preview.
   - The PPA scales in 1/16 steps. `camera_geometry.c` picks the widest centred crop that scales
     *exactly* to the output: 854×640 at 6/16 for 1280×720.
   - The pure function is tested on the PC (`tools/host_tests/run.ps1`).
4. Return the frame and **delete the camera**. This releases the DMA buffers, the module lease and
   the USB Serial/JTAG pads (camera D2 = GPIO33). Cogitador's `iris` link uses USB-OTG and is not affected.
5. **Hardware JPEG** (`esp_driver_jpeg`), RGB565 in and YUV420 out. Quality steps 80, 65, 50, 35, 20
   until the still fits `max_bytes` (`VOX_CAMERA_JPEG_MAX_KB`, 24 KiB by default).

## Contract

- Blocking, about 1–2 s. Call from one task only (the Cogitador session task).
- The returned `data` stays valid until the next capture.
- Buffers are allocated on the first call and kept, in PSRAM with JPEG/DMA2D alignment: an RGB565
  frame of 150 KiB and a 64 KiB JPEG output.
- The module manager (`mosaico_module_mgr`) starts on the first capture and keeps two 4 KiB tasks.
- Errors: `ESP_ERR_NOT_FOUND` when no camera module is detected (1.5 s discovery);
  `ESP_ERR_INVALID_SIZE` when the JPEG does not fit even at quality 20.
- Nothing is stored and there is no video stream. The module's red LED lights while the camera is
  open.

## Requirements (test 12)

- **USB Serial/JTAG must be disabled** (`# CONFIG_USJ_ENABLE_USB_SERIAL_JTAG is not set`). Its pads
  are camera D2 and flash (GPIO33/34). With USJ enabled the sensor streams but no frame completes.
- **`mosaico_module_camera` must include the official CAM_PWDN fix `b60f5d8`** (test 13).
  Without it, the sensor answers over SCCB but no frame completes.
- **SC101IOT format: 1280×720 at 25 fps only** (test 13). With the 15 fps format (the sensor
  driver's Kconfig default) no frame completes either. `sdkconfig.defaults` makes 25 fps the default
  and leaves 15 fps out of the build.
- Validated with `esp_video` 2.5.0, the version pinned in `main/idf_component.yml`, on the V1.4 module
  (SC101IOT): 13–14 KB JPEG at quality 80 in about 1.4 s. Orientation and colours were confirmed by the
  owner.

## Build note

At CMake configure time, `mosaico_module_camera` patches ESP-IDF's DVP driver (`esp_driver_cam`)
in `$IDF_PATH`, and the SC101IOT sensor in `managed_components`. That was the owner's decision on
2026-11-25: see `docs/HANDOFF.md` §5.
