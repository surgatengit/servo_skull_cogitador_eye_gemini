// SPDX-License-Identifier: Apache-2.0

#include "app_vox_eye.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bundle_gsp.h"
#include "secret_text.h"

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
/* Dice panel (ui/main.json "dice"; keep in sync). */
#define DIE_SIZE               76
#define DIE_GAP                16
#define DICE_ROLL_TICKS        24       /* ~0.8 s of tumbling numbers */
#define DICE_SHOW_TICKS        180      /* ~6 s with the result */
#define DIE_COLOR_NORMAL       0x1E2A33
#define DIE_COLOR_MAX          0xB37800 /* gold: critical on a d20 */
#define DIE_COLOR_ONE          0xA0102A /* red: fumble on a d20 */
/* Dice request handoff: release/acquire on the device; the PC preview (MSVC)
 * runs GSP and the demo on one thread. */
#if defined(__GNUC__)
#define SEQ_LOAD(p)            __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define SEQ_BUMP(p)            ((void)__atomic_add_fetch((p), 1U, __ATOMIC_RELEASE))
#else
#define SEQ_LOAD(p)            (*(volatile uint32_t *)(p))
#define SEQ_BUMP(p)            ((void)(++*(volatile uint32_t *)(p)))
#endif
/* Secret card text box: 332 px wide at 20 px, 11 lines of 24 px. */
#define SECRET_COLUMNS         27
#define SECRET_LINES           11
#define SECRET_CAPACITY        480

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
    /* Dice: the producer fills dice_req_* and then bumps dice_req_seq (release);
     * the render task copies them when it sees a new sequence (acquire). */
    uint16_t dice_req_values[APP_VOX_EYE_MAX_DICE];
    uint32_t dice_req_count;
    uint32_t dice_req_sides;
    uint32_t dice_req_seq;
    uint32_t dice_seen_seq;         /* render task only from here on */
    uint16_t dice_values[APP_VOX_EYE_MAX_DICE];
    uint32_t dice_count;
    uint32_t dice_sides;
    uint32_t dice_tick;
    bool dice_active;
    volatile int32_t secret_open;   /* set by any task, cleared on the render task */
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

static void set_die_visible(struct app_vox_eye *eye, unsigned i, bool visible)
{
    switch (i) {
    case 0: remember(eye, gsp_vox_die0_set_visible(eye->ui, visible)); break;
    case 1: remember(eye, gsp_vox_die1_set_visible(eye->ui, visible)); break;
    case 2: remember(eye, gsp_vox_die2_set_visible(eye->ui, visible)); break;
    default: remember(eye, gsp_vox_die3_set_visible(eye->ui, visible)); break;
    }
}

static void set_die_x(struct app_vox_eye *eye, unsigned i, int32_t x)
{
    switch (i) {
    case 0: remember(eye, gsp_vox_die0_slot_set_x(eye->ui, x)); break;
    case 1: remember(eye, gsp_vox_die1_slot_set_x(eye->ui, x)); break;
    case 2: remember(eye, gsp_vox_die2_slot_set_x(eye->ui, x)); break;
    default: remember(eye, gsp_vox_die3_slot_set_x(eye->ui, x)); break;
    }
}

static void set_die_color(struct app_vox_eye *eye, unsigned i, uint32_t rgb)
{
    switch (i) {
    case 0: remember(eye, gsp_vox_die0_face_set_color_rgb888(eye->ui, rgb)); break;
    case 1: remember(eye, gsp_vox_die1_face_set_color_rgb888(eye->ui, rgb)); break;
    case 2: remember(eye, gsp_vox_die2_face_set_color_rgb888(eye->ui, rgb)); break;
    default: remember(eye, gsp_vox_die3_face_set_color_rgb888(eye->ui, rgb)); break;
    }
}

static void set_die_value(struct app_vox_eye *eye, unsigned i, unsigned value)
{
    char text[8];
    snprintf(text, sizeof(text), "%u", value);
    switch (i) {
    case 0: remember(eye, (esp_gsp_err_t)gsp_vox_die0_value_set_text(eye->ui, text)); break;
    case 1: remember(eye, (esp_gsp_err_t)gsp_vox_die1_value_set_text(eye->ui, text)); break;
    case 2: remember(eye, (esp_gsp_err_t)gsp_vox_die2_value_set_text(eye->ui, text)); break;
    default: remember(eye, (esp_gsp_err_t)gsp_vox_die3_value_set_text(eye->ui, text)); break;
    }
}

static void hide_dice(struct app_vox_eye *eye)
{
    eye->dice_active = false;
    remember(eye, gsp_vox_dice_set_visible(eye->ui, false));
}

/* Render task: start a new roll, tumble, settle on the results, hide. */
static void step_dice(struct app_vox_eye *eye)
{
    const uint32_t seq = SEQ_LOAD(&eye->dice_req_seq);
    if (seq != eye->dice_seen_seq) {
        eye->dice_seen_seq = seq;
        eye->dice_count = eye->dice_req_count;
        eye->dice_sides = eye->dice_req_sides;
        memcpy(eye->dice_values, eye->dice_req_values, sizeof(eye->dice_values));
        char title[24];
        snprintf(title, sizeof(title), "TIRADA %uD%u", (unsigned)eye->dice_count,
                 (unsigned)eye->dice_sides);
        remember(eye, (esp_gsp_err_t)gsp_vox_dice_title_set_text(eye->ui, title));
        remember(eye, (esp_gsp_err_t)gsp_vox_dice_total_set_text(eye->ui, " "));
        const int32_t width = (int32_t)eye->dice_count * DIE_SIZE +
                              ((int32_t)eye->dice_count - 1) * DIE_GAP;
        const int32_t x0 = 240 - width / 2;
        for (unsigned i = 0; i < APP_VOX_EYE_MAX_DICE; ++i) {
            const bool used = i < eye->dice_count;
            if (used) {
                set_die_x(eye, i, x0 + (int32_t)i * (DIE_SIZE + DIE_GAP));
                set_die_color(eye, i, DIE_COLOR_NORMAL);
            }
            set_die_visible(eye, i, used);
        }
        eye->dice_tick = 0;
        eye->dice_active = true;
        remember(eye, gsp_vox_dice_set_visible(eye->ui, true));
    }
    if (!eye->dice_active) {
        return;
    }
    eye->dice_tick++;
    if (eye->dice_tick < DICE_ROLL_TICKS) {
        if ((eye->dice_tick & 1U) == 0U) {
            for (unsigned i = 0; i < eye->dice_count; ++i) {
                set_die_value(eye, i, rng_range(eye, 1, eye->dice_sides));
            }
        }
    } else if (eye->dice_tick == DICE_ROLL_TICKS) {
        unsigned total = 0;
        bool any_max = false;
        bool any_one = false;
        for (unsigned i = 0; i < eye->dice_count; ++i) {
            const unsigned v = eye->dice_values[i];
            const bool max = v == eye->dice_sides;
            const bool one = v == 1U;
            total += v;
            any_max = any_max || max;
            any_one = any_one || one;
            set_die_value(eye, i, v);
            set_die_color(eye, i, max ? DIE_COLOR_MAX : (one ? DIE_COLOR_ONE : DIE_COLOR_NORMAL));
        }
        const char *mark = "";
        if (eye->dice_sides == 20U && any_max) {
            mark = eye->dice_count > 1 ? " · CRÍTICO" : "CRÍTICO";
        } else if (eye->dice_sides == 20U && any_one) {
            mark = eye->dice_count > 1 ? " · PIFIA" : "PIFIA";
        }
        char text[40];
        if (eye->dice_count > 1) {
            snprintf(text, sizeof(text), "TOTAL %u%s", total, mark);
        } else {
            snprintf(text, sizeof(text), "%s", mark[0] ? mark : " ");
        }
        remember(eye, (esp_gsp_err_t)gsp_vox_dice_total_set_text(eye->ui, text));
    } else if (eye->dice_tick > DICE_ROLL_TICKS + DICE_SHOW_TICKS) {
        hide_dice(eye);
    }
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
    step_dice(eye);
    if (eye->gallery_open || eye->dice_active || eye->secret_open) {
        return; /* covered by an overlay: no eye repaints, less DMA traffic */
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

/* Overlays and bezel buttons (1.6.0). Returns true when the event was consumed. */
static bool rol_event(struct app_vox_eye *eye, const esp_gsp_event_t *event)
{
    if (gsp_vox_event_is_dice_close(event)) {
        if (eye->dice_tick >= DICE_ROLL_TICKS) {
            hide_dice(eye); /* keep it while tumbling */
        }
        return true;
    }
    if (gsp_vox_event_is_secret_close(event)) {
        remember(eye, gsp_vox_secret_set_visible(eye->ui, false));
        eye->secret_open = 0;
        return true;
    }
    app_vox_eye_action_t action;
    if (gsp_vox_event_is_voice_next(event)) {
        action = APP_VOX_EYE_ACTION_VOICE_NEXT;
    } else if (gsp_vox_event_is_role_roll(event)) {
        action = APP_VOX_EYE_ACTION_ROLE_ROLL;
    } else {
        return false;
    }
    if (!eye->gallery_open && eye->config.on_action) {
        eye->config.on_action(action, eye->config.action_ctx);
    }
    return true;
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    struct app_vox_eye *eye = ctx;
    if (gallery_event(eye, event) || rol_event(eye, event)) {
        return;
    }
    if (!gsp_vox_event_is_eye_tap(event) || eye->gallery_open || eye->dice_active ||
            eye->secret_open) {
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

esp_gsp_err_t app_vox_eye_set_voice_initial(app_vox_eye_handle_t handle, char initial)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    if (initial >= 'a' && initial <= 'z') {
        initial = (char)(initial - 32);
    }
    const char text[2] = { (initial >= 'A' && initial <= 'Z') ? initial : 'V', '\0' };
    return (esp_gsp_err_t)gsp_vox_voice_badge_text_set_text(handle->ui, text);
}

esp_gsp_err_t app_vox_eye_set_role_mode(app_vox_eye_handle_t handle, bool enabled)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return (esp_gsp_err_t)gsp_vox_role_badge_set_visible(handle->ui, enabled);
}

esp_gsp_err_t app_vox_eye_show_dice(app_vox_eye_handle_t handle, const uint16_t *values,
                                    unsigned count, unsigned sides)
{
    if (!handle || !values || count == 0 || count > APP_VOX_EYE_MAX_DICE || sides < 2 ||
            sides > 100) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    memcpy(handle->dice_req_values, values, count * sizeof(values[0]));
    handle->dice_req_count = count;
    handle->dice_req_sides = sides;
    SEQ_BUMP(&handle->dice_req_seq);
    return ESP_GSP_OK;
}

esp_gsp_err_t app_vox_eye_show_secret(app_vox_eye_handle_t handle, const char *player,
                                      const char *text)
{
    if (!handle || !text) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    char clean[SECRET_CAPACITY];
    char wrapped[SECRET_CAPACITY];
    char name[40];
    char title[64];
    secret_text_sanitize(player, name, sizeof(name), true);
    snprintf(title, sizeof(title), "SOLO PARA %s", name[0] ? name : "TI");
    secret_text_sanitize(text, clean, sizeof(clean), false);
    secret_text_wrap(clean, SECRET_COLUMNS, SECRET_LINES, wrapped, sizeof(wrapped));
    esp_gsp_err_t err = (esp_gsp_err_t)gsp_vox_secret_title_set_text(handle->ui, title);
    const esp_gsp_err_t body = (esp_gsp_err_t)gsp_vox_secret_text_set_text(
        handle->ui, wrapped[0] ? wrapped : " ");
    handle->secret_open = 1;
    const esp_gsp_err_t vis = (esp_gsp_err_t)gsp_vox_secret_set_visible(handle->ui, true);
    return err != ESP_GSP_OK ? err : (body != ESP_GSP_OK ? body : vis);
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
