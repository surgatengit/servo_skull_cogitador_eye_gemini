// SPDX-License-Identifier: Apache-2.0

#include "app_vox_eye.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bundle_gsp.h"

/* Scene geometry (authored in ui/main.json; keep in sync). */
#define EYE_TICK_MS            33
#define EYE_DT                 (EYE_TICK_MS / 1000.0f)
#define IRIS_HOME              130      /* iris top-left when centred */
#define IRIS_MAX_OFFSET        58.0f    /* radial gaze limit inside the socket */
#define IRIS_HALF              110      /* iris radius; pupil centre in iris space */
#define PUPIL_MIN              50
#define PUPIL_MAX              130
#define LID_TOP_OPEN           (-250)
#define LID_BOTTOM_OPEN        480
#define LID_TRAVEL             250
#define GLOW_LEVELS            8.0f     /* quantized glow brightness steps */
#define GLOW_MIN_TICKS         3        /* >= ~100 ms between glow repaints */
/* Gallery X (gallery_close button hit area in ui/main.json; keep in sync). */
#define CLOSE_X0               206
#define CLOSE_X1               274
#define CLOSE_Y0               0
#define CLOSE_Y1               64
#define DELETE_HOLD_TICKS      45       /* ~1.5 s holding the X deletes every photo */

/* Blink phases in ticks (~66 ms close, ~33 ms hold, ~100 ms open). */
#define BLINK_CLOSE_TICKS      2
#define BLINK_HOLD_TICKS       1
#define BLINK_OPEN_TICKS       3
#define BLINK_TOTAL_TICKS      (BLINK_CLOSE_TICKS + BLINK_HOLD_TICKS + BLINK_OPEN_TICKS)

typedef struct {
    uint32_t color;         /* 0xRRGGBB */
    float gaze_range;       /* fraction of IRIS_MAX_OFFSET */
    uint16_t hold_min_ms;   /* gaze hold range */
    uint16_t hold_max_ms;
    float spring_hz;        /* gaze responsiveness */
    float pupil;            /* base pupil diameter, px */
} eye_style_t;

static const eye_style_t STYLES[APP_VOX_EYE_STATE_COUNT] = {
    [APP_VOX_EYE_IDLE]       = { 0x00E5FF, 1.00f, 700, 2800, 2.6f, 84.0f },
    [APP_VOX_EYE_LISTENING]  = { 0xFF1A1A, 0.45f, 1200, 3200, 2.2f, 104.0f },
    [APP_VOX_EYE_PROCESSING] = { 0xFFB000, 0.70f, 250, 700, 3.4f, 70.0f },
    [APP_VOX_EYE_SPEAKING]   = { 0x20FF60, 0.30f, 900, 2400, 2.0f, 82.0f },
    [APP_VOX_EYE_ERROR]      = { 0xB040FF, 0.85f, 1500, 4000, 1.2f, 60.0f },
};

/* Render-task state is touched only by the GSP timer/event callbacks.
 * requested_state is the single cross-task word: an aligned 32-bit store from
 * the application task and a load on the render task (no read-modify-write). */
struct app_vox_eye {
    esp_gsp_handle_t ui;
    app_vox_eye_config_t config;
    void *timer;
    volatile int32_t requested_state;
    volatile int32_t blink_request; /* set by any task, consumed by the timer */
    volatile int32_t gallery_open;  /* set by any task: pause the eye under the gallery */
    /* Long press on the gallery X (render task only: interceptor + timer). */
    uint32_t ticks;                 /* animation timer ticks, also while paused */
    bool x_held;
    bool x_fired;
    uint32_t x_since;
    app_vox_eye_state_t drawn_state;
    uint32_t rng;
    /* Gaze spring (offsets from centre, px). */
    float gx, gy, vx, vy, tx, ty;
    uint32_t hold_ticks;
    uint32_t micro_ticks;
    /* Pupil. */
    float pupil;
    /* Blink. */
    uint32_t blink_wait_ticks;
    uint32_t blink_tick;
    bool blinking;
    bool double_blink;
    /* Last values pushed to the scene, to avoid redundant setters. */
    int32_t last_iris_x, last_iris_y, last_pupil, last_lid;
    uint32_t last_glow, last_ring, last_core;
    uint32_t glow_ticks;   /* ticks since the last glow repaint */
    float phase;
    app_vox_eye_snapshot_t stats;
};

/* Record a setter error; returns true on success so callers cache only values
 * the scene accepted (a rejected setter is retried on the next tick). */
static bool remember(struct app_vox_eye *eye, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        eye->stats.last_error = err;
        return false;
    }
    return true;
}

static uint32_t rng_next(struct app_vox_eye *eye)
{
    uint32_t x = eye->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    eye->rng = x;
    return x;
}

static float rng_unit(struct app_vox_eye *eye)
{
    return (float)(rng_next(eye) & 0xFFFFFF) / (float)0x1000000;
}

static uint32_t rng_range(struct app_vox_eye *eye, uint32_t lo, uint32_t hi)
{
    return lo + (hi > lo ? rng_next(eye) % (hi - lo + 1U) : 0U);
}

static uint32_t ms_to_ticks(uint32_t ms)
{
    return (ms + EYE_TICK_MS - 1U) / EYE_TICK_MS;
}

static uint32_t scale_rgb(uint32_t rgb, float k)
{
    if (k < 0.0f) {
        k = 0.0f;
    }
    if (k > 1.0f) {
        k = 1.0f;
    }
    const uint32_t r = (uint32_t)(((rgb >> 16) & 0xFF) * k);
    const uint32_t g = (uint32_t)(((rgb >> 8) & 0xFF) * k);
    const uint32_t b = (uint32_t)((rgb & 0xFF) * k);
    return (r << 16) | (g << 8) | b;
}

static uint32_t lighten_rgb(uint32_t rgb, float k)
{
    const uint32_t r = (rgb >> 16) & 0xFF;
    const uint32_t g = (rgb >> 8) & 0xFF;
    const uint32_t b = rgb & 0xFF;
    return ((r + (uint32_t)((255U - r) * k)) << 16) |
           ((g + (uint32_t)((255U - g) * k)) << 8) |
           (b + (uint32_t)((255U - b) * k));
}

static void pick_gaze_target(struct app_vox_eye *eye)
{
    const eye_style_t *style = &STYLES[eye->drawn_state];
    const float angle = rng_unit(eye) * 6.2831853f;
    /* sqrt gives a uniform distribution over the disc; bias slightly to centre. */
    float radius = sqrtf(rng_unit(eye)) * style->gaze_range * IRIS_MAX_OFFSET;
    if ((rng_next(eye) & 3U) == 0U) {
        radius *= 0.2f; /* occasional glance back to centre */
    }
    eye->tx = cosf(angle) * radius;
    eye->ty = sinf(angle) * radius * 0.8f; /* eyes travel less vertically */
    if (eye->drawn_state == APP_VOX_EYE_PROCESSING) {
        eye->ty -= 18.0f; /* "thinking": glance upwards */
    }
    eye->hold_ticks = ms_to_ticks(rng_range(eye, style->hold_min_ms, style->hold_max_ms));
}

static void schedule_blink(struct app_vox_eye *eye)
{
    eye->blink_wait_ticks = ms_to_ticks(rng_range(eye, 2500, 7000));
}

static void apply_colors(struct app_vox_eye *eye, float glow_gain)
{
    const uint32_t base = STYLES[eye->drawn_state].color;
    /* The glow ring is a 400x400 px repaint. Quantize the pulse to 8 levels and at
     * most GLOW_MIN_TICKS apart: every panel transfer needs internal DMA bounce
     * buffers (the BSP disables PSRAM-direct DMA), so traffic must stay low. */
    const float quantized = (float)lroundf(glow_gain * GLOW_LEVELS) / GLOW_LEVELS;
    const uint32_t glow = scale_rgb(base, quantized);
    const bool state_changed = base != eye->last_ring;
    if (++eye->glow_ticks < GLOW_MIN_TICKS && !state_changed) {
        glow_gain = -1.0f; /* skip the glow this tick */
    }
    if (glow_gain >= 0.0f && glow != eye->last_glow &&
            remember(eye, gsp_vox_glow_set_color_rgb888(eye->ui, glow))) {
        eye->last_glow = glow;
        eye->glow_ticks = 0;
    }
    const uint32_t ring = base;
    const uint32_t core = lighten_rgb(base, 0.35f);
    if (ring != eye->last_ring && remember(eye, gsp_vox_iris_ring_set_color_rgb888(eye->ui, ring))) {
        eye->last_ring = ring;
    }
    if (core != eye->last_core && remember(eye, gsp_vox_iris_core_set_color_rgb888(eye->ui, core))) {
        eye->last_core = core;
    }
}

static float blink_closure(struct app_vox_eye *eye)
{
    if (!eye->blinking) {
        return 0.0f;
    }
    const uint32_t t = eye->blink_tick;
    if (t < BLINK_CLOSE_TICKS) {
        return (float)(t + 1U) / BLINK_CLOSE_TICKS;
    }
    if (t < BLINK_CLOSE_TICKS + BLINK_HOLD_TICKS) {
        return 1.0f;
    }
    const uint32_t open = t - BLINK_CLOSE_TICKS - BLINK_HOLD_TICKS;
    return 1.0f - (float)(open + 1U) / BLINK_OPEN_TICKS;
}

static void step_blink(struct app_vox_eye *eye)
{
    if (eye->blinking) {
        if (++eye->blink_tick >= BLINK_TOTAL_TICKS) {
            eye->blinking = false;
            eye->stats.blinks++;
            if (eye->double_blink) {
                eye->double_blink = false;
                eye->blink_wait_ticks = 3;
            } else {
                schedule_blink(eye);
            }
        }
        return;
    }
    if (eye->blink_wait_ticks > 0U) {
        eye->blink_wait_ticks--;
        return;
    }
    eye->blinking = true;
    eye->blink_tick = 0;
    eye->double_blink = eye->double_blink || (rng_next(eye) % 5U) == 0U;
}

static void animate(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    struct app_vox_eye *eye = ctx;
    eye->ticks++;
    if (eye->x_held && !eye->x_fired && eye->ticks - eye->x_since >= DELETE_HOLD_TICKS) {
        eye->x_fired = true; /* fire while still held; the release is swallowed */
        if (eye->config.on_gallery) {
            eye->config.on_gallery(APP_VOX_GALLERY_DELETE_ALL, eye->config.gallery_ctx);
        }
    }
    if (eye->gallery_open) {
        return; /* hidden under the gallery: no repaints, no DMA traffic */
    }
    eye->stats.frames++;

    int32_t requested = eye->requested_state;
    if (requested < 0 || requested >= APP_VOX_EYE_STATE_COUNT) {
        requested = APP_VOX_EYE_ERROR;
    }
    if ((app_vox_eye_state_t)requested != eye->drawn_state) {
        eye->drawn_state = (app_vox_eye_state_t)requested;
        eye->stats.state = eye->drawn_state;
        pick_gaze_target(eye);
    }
    const eye_style_t *style = &STYLES[eye->drawn_state];

    /* Gaze: critically damped spring towards the current target, plus microsaccades. */
    if (eye->hold_ticks > 0U) {
        eye->hold_ticks--;
    } else {
        pick_gaze_target(eye);
    }
    if (++eye->micro_ticks >= 9U) {
        eye->micro_ticks = 0;
        eye->tx += ((float)rng_range(eye, 0, 6) - 3.0f);
        eye->ty += ((float)rng_range(eye, 0, 4) - 2.0f);
    }
    const float w = 6.2831853f * style->spring_hz;
    eye->vx += (w * w * (eye->tx - eye->gx) - 2.0f * w * eye->vx) * EYE_DT;
    eye->vy += (w * w * (eye->ty - eye->gy) - 2.0f * w * eye->vy) * EYE_DT;
    eye->gx += eye->vx * EYE_DT;
    eye->gy += eye->vy * EYE_DT;
    const float r = sqrtf(eye->gx * eye->gx + eye->gy * eye->gy);
    if (r > IRIS_MAX_OFFSET) {
        eye->gx *= IRIS_MAX_OFFSET / r;
        eye->gy *= IRIS_MAX_OFFSET / r;
    }
    const int32_t ix = IRIS_HOME + (int32_t)lroundf(eye->gx);
    const int32_t iy = IRIS_HOME + (int32_t)lroundf(eye->gy);
    if ((ix != eye->last_iris_x || iy != eye->last_iris_y) &&
            remember(eye, gsp_vox_iris_set_position(eye->ui, ix, iy))) {
        eye->last_iris_x = ix;
        eye->last_iris_y = iy;
    }

    /* Pupil: per-state size, speaking pulses with the output level. */
    eye->phase += EYE_DT;
    float target = style->pupil;
    float glow_gain = 1.0f;
    if (eye->drawn_state == APP_VOX_EYE_SPEAKING && eye->config.level) {
        const float level = (float)eye->config.level(eye->config.level_ctx) / 100.0f;
        target += level * 40.0f;
        glow_gain = 0.55f + 0.45f * level;
    } else if (eye->drawn_state == APP_VOX_EYE_PROCESSING) {
        glow_gain = 0.6f + 0.4f * (0.5f + 0.5f * sinf(eye->phase * 9.0f));
    } else if (eye->drawn_state == APP_VOX_EYE_LISTENING) {
        glow_gain = 0.8f + 0.2f * (0.5f + 0.5f * sinf(eye->phase * 3.0f));
    } else if (eye->drawn_state == APP_VOX_EYE_ERROR) {
        glow_gain = (fmodf(eye->phase, 1.2f) < 0.6f) ? 1.0f : 0.35f;
    }
    eye->pupil += (target - eye->pupil) * 0.25f;
    int32_t pupil = (int32_t)lroundf(eye->pupil);
    pupil = pupil < PUPIL_MIN ? PUPIL_MIN : (pupil > PUPIL_MAX ? PUPIL_MAX : pupil);
    pupil &= ~1; /* even diameter keeps the pupil centred */
    if (pupil != eye->last_pupil) {
        bool ok = remember(eye, gsp_vox_pupil_set_w(eye->ui, pupil));
        ok = remember(eye, gsp_vox_pupil_set_h(eye->ui, pupil)) && ok;
        ok = remember(eye, gsp_vox_pupil_set_radius(eye->ui, pupil / 2)) && ok;
        ok = remember(eye, gsp_vox_pupil_set_position(eye->ui, IRIS_HALF - pupil / 2,
                                                      IRIS_HALF - pupil / 2)) && ok;
        if (ok) {
            eye->last_pupil = pupil;
        }
    }

    apply_colors(eye, glow_gain);

    /* Eyelids. */
    if (eye->blink_request) {
        eye->blink_request = 0;
        if (!eye->blinking) {
            eye->blink_wait_ticks = 0;
        }
    }
    step_blink(eye);
    const int32_t lid = (int32_t)lroundf(blink_closure(eye) * LID_TRAVEL);
    if (lid != eye->last_lid) {
        bool ok = remember(eye, gsp_vox_lid_top_set_y(eye->ui, LID_TOP_OPEN + lid));
        ok = remember(eye, gsp_vox_lid_bottom_set_y(eye->ui, LID_BOTTOM_OPEN - lid)) && ok;
        if (ok) {
            eye->last_lid = lid;
        }
    }
}

static bool gallery_event(struct app_vox_eye *eye, const esp_gsp_event_t *event)
{
    app_vox_gallery_action_t action;
    if (gsp_vox_event_is_photos_open(event)) {
        action = APP_VOX_GALLERY_OPEN;
    } else if (gsp_vox_event_is_gallery_close(event)) {
        action = APP_VOX_GALLERY_CLOSE;
    } else if (gsp_vox_event_is_gallery_prev(event)) {
        action = APP_VOX_GALLERY_PREV;
    } else if (gsp_vox_event_is_gallery_next(event)) {
        action = APP_VOX_GALLERY_NEXT;
    } else if (gsp_vox_event_is_gallery_touch(event)) {
        action = APP_VOX_GALLERY_TOUCH;
    } else {
        return false;
    }
    if (eye->config.on_gallery) {
        eye->config.on_gallery(action, eye->config.gallery_ctx);
    }
    return true;
}

#if defined(ESP_PLATFORM) /* the PC sim_bridge has no input interceptor */
/* Raw touch samples (render task): track a press that starts on the gallery X.
 * GSP has no long-press event; a held X deletes all photos and its release
 * must not also close the gallery as a normal tap. */
static bool on_input(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    struct app_vox_eye *eye = ctx;
    const bool on_x = x >= CLOSE_X0 && x < CLOSE_X1 && y >= CLOSE_Y0 && y < CLOSE_Y1;
    if (pressed) {
        if (!eye->x_held && eye->gallery_open && on_x) {
            eye->x_held = true;
            eye->x_fired = false;
            eye->x_since = eye->ticks;
        } else if (eye->x_held && !on_x && !eye->x_fired) {
            eye->x_held = false; /* slid off the X: neither delete nor block */
        }
        return eye->x_fired;
    }
    const bool swallow = eye->x_held && eye->x_fired;
    eye->x_held = false;
    eye->x_fired = false;
    return swallow;
}
#endif

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    struct app_vox_eye *eye = ctx;
    if (gallery_event(eye, event)) {
        return;
    }
    if (!gsp_vox_event_is_eye_tap(event) || eye->gallery_open) {
        return;
    }
    eye->stats.taps++;
    /* Acknowledge the touch with a quick blink. */
    if (!eye->blinking) {
        eye->blink_wait_ticks = 0;
    }
    if (eye->config.on_tap) {
        eye->config.on_tap(eye->config.tap_ctx);
    }
}

esp_gsp_err_t app_vox_eye_create(esp_gsp_handle_t ui, const app_vox_eye_config_t *config,
                                 app_vox_eye_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui || !config) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_vox_eye *eye = calloc(1, sizeof(*eye));
    if (!eye) {
        return ESP_GSP_ERR_NO_MEM;
    }
    eye->ui = ui;
    eye->config = *config;
    eye->rng = config->seed ? config->seed : 0x5EED1234u;
    eye->requested_state = APP_VOX_EYE_IDLE;
    eye->drawn_state = APP_VOX_EYE_IDLE;
    eye->pupil = STYLES[APP_VOX_EYE_IDLE].pupil;
    eye->last_iris_x = eye->last_iris_y = eye->last_pupil = eye->last_lid = -1000;
    eye->last_glow = eye->last_ring = eye->last_core = 0xFFFFFFFFu;
    pick_gaze_target(eye);
    schedule_blink(eye);

    remember(eye, esp_gsp_set_press_feedback_enabled(ui, false));
    remember(eye, (esp_gsp_err_t)gsp_vox_status_set_text(ui, " "));
    esp_gsp_err_t err = eye->stats.last_error;
    if (err == ESP_GSP_OK) {
        eye->timer = esp_gsp_timer_create(ui, EYE_TICK_MS, animate, eye);
        if (!eye->timer) {
            err = ESP_GSP_ERR_NO_MEM;
        }
    }
    if (err == ESP_GSP_OK) {
        /* Publish only a fully initialized context; dispatch may start now. */
        err = esp_gsp_on_event(ui, on_event, eye);
        if (err != ESP_GSP_OK) {
            (void)esp_gsp_timer_delete(ui, eye->timer);
        }
    }
    if (err == ESP_GSP_OK) {
#if defined(ESP_PLATFORM)
        /* Optional: without it the X only closes (no delete-all gesture). */
        (void)esp_gsp_set_input_interceptor(ui, on_input, eye);
#endif
    }
    if (err != ESP_GSP_OK) {
        free(eye);
        return err;
    }
    *out_handle = eye;
    return ESP_GSP_OK;
}

void app_vox_eye_set_state(app_vox_eye_handle_t handle, app_vox_eye_state_t state)
{
    if (handle && state >= APP_VOX_EYE_IDLE && state < APP_VOX_EYE_STATE_COUNT) {
        handle->requested_state = (int32_t)state;
    }
}

void app_vox_eye_blink(app_vox_eye_handle_t handle)
{
    if (handle) {
        handle->blink_request = 1;
    }
}

esp_gsp_err_t app_vox_eye_set_status(app_vox_eye_handle_t handle, const char *utf8)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return (esp_gsp_err_t)gsp_vox_status_set_text(handle->ui,
                                                  (utf8 && utf8[0]) ? utf8 : " ");
}

esp_gsp_err_t app_vox_eye_set_photo_count(app_vox_eye_handle_t handle, unsigned count)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    char text[8];
    snprintf(text, sizeof(text), "%u", count > 99U ? 99U : count);
    esp_gsp_err_t err = (esp_gsp_err_t)gsp_vox_photo_count_set_text(handle->ui, text);
    const esp_gsp_err_t vis = (esp_gsp_err_t)gsp_vox_photo_badge_set_visible(handle->ui, count > 0U);
    return err != ESP_GSP_OK ? err : vis;
}

esp_gsp_err_t app_vox_eye_show_photo(app_vox_eye_handle_t handle, const uint8_t *jpeg, size_t size,
                                     unsigned position, unsigned count)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    esp_gsp_err_t err = ESP_GSP_OK;
    if (jpeg && size > 0) {
        /* The previous picture stays visible while this one decodes. */
        err = (esp_gsp_err_t)gsp_vox_gallery_photo_set_image(handle->ui, jpeg, size);
    }
    char text[16];
    snprintf(text, sizeof(text), "%u / %u", position, count);
    const esp_gsp_err_t idx = (esp_gsp_err_t)gsp_vox_gallery_index_set_text(handle->ui, text);
    handle->gallery_open = 1;
    const esp_gsp_err_t vis = (esp_gsp_err_t)gsp_vox_gallery_set_visible(handle->ui, true);
    return err != ESP_GSP_OK ? err : (idx != ESP_GSP_OK ? idx : vis);
}

esp_gsp_err_t app_vox_eye_hide_gallery(app_vox_eye_handle_t handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    const esp_gsp_err_t err = (esp_gsp_err_t)gsp_vox_gallery_set_visible(handle->ui, false);
    handle->gallery_open = 0;
    return err;
}

esp_gsp_err_t app_vox_eye_read(app_vox_eye_handle_t handle, app_vox_eye_snapshot_t *out_snapshot)
{
    if (!handle || !out_snapshot) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = handle->stats;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_vox_eye_destroy(app_vox_eye_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_vox_eye *eye = *handle;
    if (!eye) {
        return ESP_GSP_OK;
    }
#if defined(ESP_PLATFORM)
    (void)esp_gsp_set_input_interceptor(eye->ui, NULL, NULL);
#endif
    esp_gsp_err_t err = esp_gsp_on_event(eye->ui, NULL, NULL);
    if (err != ESP_GSP_OK) {
        return err;
    }
    if (eye->timer) {
        err = esp_gsp_timer_delete(eye->ui, eye->timer);
        if (err != ESP_GSP_OK) {
            return err;
        }
        eye->timer = NULL;
    }
    free(eye);
    *handle = NULL;
    return ESP_GSP_OK;
}
