// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Private to app_camera: pure crop/scale arithmetic, also built on the PC
 * (tools/host_tests/test_camera_geometry.c). */

#include <stdbool.h>
#include <stdint.h>

/* The ESP32-S31 PPA scales by an integer plus a 4-bit fraction: 1/16 steps. */
#define CAMERA_SCALE_STEPS 16U

typedef struct {
    uint32_t offset_x;    /* even, in sensor pixels */
    uint32_t offset_y;    /* even */
    uint32_t block_w;     /* crop in sensor orientation, even */
    uint32_t block_h;
    uint32_t scale_steps; /* scale = scale_steps / CAMERA_SCALE_STEPS, <= 1 */
} camera_crop_t;

/**
 * Widest centred crop of a frame_w x frame_h sensor image that a PPA scale of
 * k/16 turns into exactly out_w x out_h (after an optional 90/270 degree
 * rotation, swap_axes): floor(block * k / 16) equals the target on both axes.
 * Returns false if no such crop fits the frame.
 */
bool camera_geometry_crop(uint32_t frame_w, uint32_t frame_h, uint32_t out_w, uint32_t out_h,
                          bool swap_axes, camera_crop_t *crop);
