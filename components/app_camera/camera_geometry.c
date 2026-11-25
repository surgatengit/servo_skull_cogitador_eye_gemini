// SPDX-License-Identifier: Apache-2.0

#include "camera_geometry.h"

#include <stddef.h>

static uint32_t ceil_div(uint32_t a, uint32_t b)
{
    return (a + b - 1U) / b;
}

bool camera_geometry_crop(uint32_t frame_w, uint32_t frame_h, uint32_t out_w, uint32_t out_h,
                          bool swap_axes, camera_crop_t *crop)
{
    if (!crop || out_w == 0 || out_h == 0) {
        return false;
    }
    /* Target size in sensor orientation: a 90/270 rotation swaps the axes. */
    const uint32_t want_w = swap_axes ? out_h : out_w;
    const uint32_t want_h = swap_axes ? out_w : out_h;
    if (frame_w < want_w || frame_h < want_h) {
        return false;
    }
    /* The PPA scales in steps of 1/CAMERA_SCALE_STEPS: pick the smallest step k
     * (widest field of view) whose crop fits the frame and scales exactly to want. */
    for (uint32_t k = 1; k <= CAMERA_SCALE_STEPS; ++k) {
        uint32_t block_w = ceil_div(want_w * CAMERA_SCALE_STEPS, k);
        uint32_t block_h = ceil_div(want_h * CAMERA_SCALE_STEPS, k);
        block_w += block_w & 1U; /* YUV422 pairs: keep sizes and offsets even */
        block_h += block_h & 1U;
        if (block_w > frame_w || block_h > frame_h) {
            continue;
        }
        /* The PPA rejects a scaled block larger than the output picture, and a
         * smaller one would leave a black edge: require an exact fit. */
        if (block_w * k / CAMERA_SCALE_STEPS != want_w || block_h * k / CAMERA_SCALE_STEPS != want_h) {
            continue;
        }
        crop->block_w = block_w;
        crop->block_h = block_h;
        crop->offset_x = ((frame_w - block_w) / 2U) & ~1U;
        crop->offset_y = ((frame_h - block_h) / 2U) & ~1U;
        crop->scale_steps = k;
        return true;
    }
    return false;
}
