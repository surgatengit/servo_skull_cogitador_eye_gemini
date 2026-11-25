// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One JPEG still for Gemini Live; contracts: ../README.md. */
typedef struct {
    const uint8_t *data;   /**< JPEG bytes, owned by app_camera (valid until the next capture). */
    size_t size;
    uint32_t width;        /**< Pixels after crop, scale and rotation. */
    uint32_t height;
    uint8_t quality;       /**< JPEG quality used (stepped down to fit the byte budget). */
    uint32_t elapsed_ms;   /**< Open → JPEG ready. */
} app_camera_still_t;

/**
 * Take one picture: open the left-slot camera, grab a frame after the exposure
 * settles, crop/scale/rotate it with the PPA into RGB565, encode it with the
 * hardware JPEG engine within max_bytes, then close and delete the camera so no
 * DMA buffers, lease or USB Serial/JTAG claim stay held. Blocking (about 1-2 s);
 * call from one application task only. Buffers live in PSRAM and are reused.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND (no camera module), ESP_ERR_INVALID_SIZE (JPEG
 *         does not fit even at the lowest quality), or the driver error.
 */
esp_err_t app_camera_capture_jpeg(size_t max_bytes, app_camera_still_t *out);

/** Diagnostic (test 12): request 1280x720 UYVY explicitly (false, default) or
 * use the sensor's Kconfig default format (true). Same task as the capture. */
void app_camera_use_sensor_default_format(bool use_default);

#ifdef __cplusplus
}
#endif
