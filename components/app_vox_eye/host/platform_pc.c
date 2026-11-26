// SPDX-License-Identifier: Apache-2.0
/* Host preview adapter: runs the real eye controller with a scripted demo of
 * the session states. No networking or audio exists on the PC backend. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "app_vox_eye.h"
#include "gsp_sim_bridge.h"

#define DEMO_STEP_MS 3000

/* The bridge ABI has no caller context; keep the owned handles here. */
static struct {
    app_vox_eye_handle_t eye;
    void *demo_timer;
    app_vox_eye_state_t state;
    unsigned level_tick;
    unsigned transitions;
    unsigned photo;          /* gallery demo position, 1..DEMO_PHOTOS */
    bool gallery;
    unsigned gallery_events;
    unsigned voice;          /* RoleMaster demo: voice badge position */
    unsigned rolls;
} backend;

static const char VOICE_INITIALS[] = "CAKS"; /* Charon, Algenib, Kore, Sulafat */

#define DEMO_PHOTOS 3        /* the PC has no photo store: placeholder picture */

static const char *const STATUS[APP_VOX_EYE_STATE_COUNT] = {
    [APP_VOX_EYE_IDLE] = "",
    [APP_VOX_EYE_LISTENING] = "",
    [APP_VOX_EYE_PROCESSING] = "",
    [APP_VOX_EYE_SPEAKING] = "",
    [APP_VOX_EYE_ERROR] = "SIN WI-FI",
};

static void show(app_vox_eye_state_t state)
{
    backend.state = state;
    backend.transitions++;
    app_vox_eye_set_state(backend.eye, state);
    (void)app_vox_eye_set_status(backend.eye, STATUS[state]);
    fprintf(stderr, "vox_eye_backend: state=%d\n", (int)state);
}

static void advance(void)
{
    show((app_vox_eye_state_t)((backend.state + 1) % APP_VOX_EYE_STATE_COUNT));
}

static void demo_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    (void)ctx;
    if (!backend.gallery) {
        advance();
    }
}

/* Gallery flow with the scene's placeholder picture (no JPEG on the PC). */
static void on_gallery(app_vox_gallery_action_t action, void *ctx)
{
    (void)ctx;
    backend.gallery_events++;
    switch (action) {
    case APP_VOX_GALLERY_OPEN:
        backend.photo = DEMO_PHOTOS;
        backend.gallery = true;
        break;
    case APP_VOX_GALLERY_PREV:
        backend.photo = backend.photo <= 1 ? DEMO_PHOTOS : backend.photo - 1;
        break;
    case APP_VOX_GALLERY_NEXT:
        backend.photo = backend.photo >= DEMO_PHOTOS ? 1 : backend.photo + 1;
        break;
    case APP_VOX_GALLERY_CLOSE:
        backend.gallery = false;
        (void)app_vox_eye_hide_gallery(backend.eye);
        fprintf(stderr, "vox_eye_backend: gallery closed (events=%u)\n", backend.gallery_events);
        return;
    case APP_VOX_GALLERY_TOUCH:
    default:
        return;
    }
    (void)app_vox_eye_show_photo(backend.eye, NULL, 0, backend.photo, DEMO_PHOTOS);
    fprintf(stderr, "vox_eye_backend: gallery %u / %u\n", backend.photo, DEMO_PHOTOS);
}

/* RoleMaster demo (1.6.0): the voice badge cycles C/A/K/S and shows a secret
 * card on the last voice; the D20 badge alternates a 2d20 roll with a critical
 * and a 1d20 fumble. */
static void on_action(app_vox_eye_action_t action, void *ctx)
{
    (void)ctx;
    if (action == APP_VOX_EYE_ACTION_VOICE_NEXT) {
        backend.voice = (backend.voice + 1) % 4;
        (void)app_vox_eye_set_voice_initial(backend.eye, VOICE_INITIALS[backend.voice]);
        fprintf(stderr, "vox_eye_backend: voice %c\n", VOICE_INITIALS[backend.voice]);
        if (backend.voice == 3) {
            (void)app_vox_eye_show_secret(backend.eye, "Íñigo",
                                          "Eres el hijo perdido del gobernador. Si Ana descubre "
                                          "tu sello, intentará venderte al Inquisidor… "
                                          "¿confiarás en ella?");
        }
        return;
    }
    backend.rolls++;
    if (backend.rolls % 2) {
        const uint16_t two[] = { 20, 7 };
        (void)app_vox_eye_show_dice(backend.eye, two, 2, 20);
    } else {
        const uint16_t one[] = { 1 };
        (void)app_vox_eye_show_dice(backend.eye, one, 1, 20);
    }
    fprintf(stderr, "vox_eye_backend: roll %u\n", backend.rolls);
}

/* Tap opens/closes the demo conversation like the device session does. */
static void on_tap(void *ctx)
{
    (void)ctx;
    show(backend.state == APP_VOX_EYE_IDLE ? APP_VOX_EYE_LISTENING : APP_VOX_EYE_IDLE);
}

/* Synthetic speech envelope for the speaking pupil pulse. */
static uint8_t level(void *ctx)
{
    (void)ctx;
    backend.level_tick++;
    const float t = (float)backend.level_tick * 0.033f;
    const float env = 0.5f + 0.5f * sinf(t * 7.0f) * sinf(t * 2.3f);
    return (uint8_t)(env * 100.0f);
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    const app_vox_eye_config_t config = {
        .on_tap = on_tap,
        .on_gallery = on_gallery,
        .on_action = on_action,
        .level = level,
        .seed = 0xC0617A70u,
    };
    esp_gsp_err_t err = app_vox_eye_create(ui, &config, &backend.eye);
    if (err != ESP_GSP_OK) {
        return err;
    }
    (void)app_vox_eye_set_photo_count(backend.eye, DEMO_PHOTOS);
    (void)app_vox_eye_set_voice_initial(backend.eye, 'C');
    (void)app_vox_eye_set_role_mode(backend.eye, true);
    backend.state = APP_VOX_EYE_IDLE;
    backend.demo_timer = esp_gsp_timer_create(ui, DEMO_STEP_MS, demo_tick, NULL);
    return backend.demo_timer ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    if (backend.demo_timer) {
        (void)esp_gsp_timer_delete(ui, backend.demo_timer);
        backend.demo_timer = NULL;
    }
    app_vox_eye_snapshot_t snapshot;
    if (app_vox_eye_read(backend.eye, &snapshot) == ESP_GSP_OK) {
        fprintf(stderr,
                "vox_eye_backend: frames=%u blinks=%u taps=%u state=%d transitions=%u last_error=%d\n",
                (unsigned)snapshot.frames, (unsigned)snapshot.blinks, (unsigned)snapshot.taps,
                (int)snapshot.state, backend.transitions, (int)snapshot.last_error);
    }
    /* The bridge calls deinit after poll exits; no callbacks can use the state. */
    const esp_gsp_err_t err = app_vox_eye_destroy(&backend.eye);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "vox_eye_backend: cleanup failed (%d)\n", err);
    }
}
