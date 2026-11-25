// SPDX-License-Identifier: Apache-2.0
/* PC tests for the camera crop/scale arithmetic (components/app_camera/
 * camera_geometry.c). Built and run by tools/host_tests/run.ps1. */

#include <stdio.h>

#include "camera_geometry.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

/* Mirrors the PPA driver's output size computation (esp_driver_ppa ppa_srm.c). */
static void scaled(const camera_crop_t *c, bool swap, uint32_t *w, uint32_t *h)
{
    const uint32_t sw = c->block_w * c->scale_steps / CAMERA_SCALE_STEPS;
    const uint32_t sh = c->block_h * c->scale_steps / CAMERA_SCALE_STEPS;
    *w = swap ? sh : sw;
    *h = swap ? sw : sh;
}

static void check_case(uint32_t fw, uint32_t fh, uint32_t ow, uint32_t oh, bool swap)
{
    camera_crop_t c;
    CHECK(camera_geometry_crop(fw, fh, ow, oh, swap, &c));
    uint32_t w = 0;
    uint32_t h = 0;
    scaled(&c, swap, &w, &h);
    CHECK(w == ow && h == oh);                        /* exact fit: PPA accepts it, no edge */
    CHECK(c.block_w <= fw && c.block_h <= fh);
    CHECK(c.offset_x + c.block_w <= fw && c.offset_y + c.block_h <= fh);
    CHECK((c.block_w | c.block_h | c.offset_x | c.offset_y) % 2 == 0);
    CHECK(c.scale_steps >= 1 && c.scale_steps <= CAMERA_SCALE_STEPS);
    printf("  %ux%u -> %ux%u%s: crop %ux%u at (%u,%u), scale %u/16\n", (unsigned)fw, (unsigned)fh,
           (unsigned)ow, (unsigned)oh, swap ? " (rot 90/270)" : "", (unsigned)c.block_w,
           (unsigned)c.block_h, (unsigned)c.offset_x, (unsigned)c.offset_y, (unsigned)c.scale_steps);
}

int main(void)
{
    /* SC101IOT default 1280x720 (owner's V1.4 module), portrait 240x320 after 270 degrees. */
    check_case(1280, 720, 240, 320, true);
    camera_crop_t c;
    CHECK(camera_geometry_crop(1280, 720, 240, 320, true, &c) && c.block_w == 854 &&
          c.block_h == 640 && c.scale_steps == 6);
    /* OV3640 default 1024x768 (V1.2 module): full frame at 5/16. */
    check_case(1024, 768, 240, 320, true);
    CHECK(camera_geometry_crop(1024, 768, 240, 320, true, &c) && c.block_w == 1024 &&
          c.block_h == 768 && c.scale_steps == 5);
    /* Other orientations and sizes. */
    check_case(1280, 720, 320, 240, false);
    check_case(1280, 720, 320, 176, false);
    check_case(640, 480, 320, 240, false);
    check_case(320, 240, 320, 240, false);       /* scale 16/16 */
    /* Frame smaller than the output: refused. */
    CHECK(!camera_geometry_crop(200, 200, 240, 320, true, &c));
    CHECK(!camera_geometry_crop(1280, 720, 0, 320, true, &c));
    CHECK(!camera_geometry_crop(1280, 720, 240, 320, true, NULL));
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all camera geometry host tests passed\n");
    return 0;
}
