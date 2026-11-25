// SPDX-License-Identifier: Apache-2.0

#include "app_camera.h"
#include "camera_geometry.h"

#include <string.h>

#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"
#include "sdkconfig.h"

#if CONFIG_VOX_CAMERA

static const char *TAG = "app_camera";

#define OUT_W               CONFIG_VOX_CAMERA_WIDTH
#define OUT_H               CONFIG_VOX_CAMERA_HEIGHT
#define SETTLE_FRAMES       CONFIG_VOX_CAMERA_SETTLE_FRAMES
#define FRAME_TIMEOUT_MS    2000
#define CAPTURE_W           1280
#define CAPTURE_H           720
#define JPEG_TIMEOUT_MS     1000
#define JPEG_OUT_CAPACITY   (64 * 1024)   /* hard ceiling; callers ask for less */

#if CONFIG_VOX_CAMERA_ROTATE_90
#define ROTATION  PPA_SRM_ROTATION_ANGLE_90
#define SWAP_AXES true
#elif CONFIG_VOX_CAMERA_ROTATE_180
#define ROTATION  PPA_SRM_ROTATION_ANGLE_180
#define SWAP_AXES false
#elif CONFIG_VOX_CAMERA_ROTATE_270
#define ROTATION  PPA_SRM_ROTATION_ANGLE_270
#define SWAP_AXES true
#else
#define ROTATION  PPA_SRM_ROTATION_ANGLE_0
#define SWAP_AXES false
#endif

#if CONFIG_VOX_CAMERA_MIRROR_X
#define MIRROR_X true
#else
#define MIRROR_X false
#endif
#if CONFIG_VOX_CAMERA_MIRROR_Y
#define MIRROR_Y true
#else
#define MIRROR_Y false
#endif

/* Quality ladder: the first that fits max_bytes wins. */
static const uint8_t QUALITIES[] = { 80, 65, 50, 35, 20 };

/* Single owner: the caller's task (the VOX session). Buffers are allocated on
 * the first capture, in PSRAM with the JPEG/DMA2D alignment, and kept. */
static struct {
    uint8_t *rgb;          /* OUT_W x OUT_H RGB565: PPA output, JPEG input */
    size_t rgb_size;
    uint8_t *jpeg;
    size_t jpeg_size;
} s;

static bool s_sensor_default_format;

void app_camera_use_sensor_default_format(bool use_default)
{
    s_sensor_default_format = use_default;
}

static esp_err_t ensure_buffers(void)
{
    if (s.rgb && s.jpeg) {
        return ESP_OK;
    }
    const jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    const jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    if (!s.rgb) {
        s.rgb = jpeg_alloc_encoder_mem((size_t)OUT_W * OUT_H * 2U, &in_cfg, &s.rgb_size);
    }
    if (!s.jpeg) {
        s.jpeg = jpeg_alloc_encoder_mem(JPEG_OUT_CAPACITY, &out_cfg, &s.jpeg_size);
    }
    return (s.rgb && s.jpeg) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* UYVY frame → cropped, scaled, rotated RGB565 in s.rgb (PPA, blocking). */
static esp_err_t convert_frame(const mosaico_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(frame->pixel_format == V4L2_PIX_FMT_UYVY, ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported camera format 0x%08lx", (unsigned long)frame->pixel_format);
    const uint32_t stride_px = frame->bytes_per_line ? frame->bytes_per_line / 2U : frame->width;
    camera_crop_t crop;
    ESP_RETURN_ON_FALSE(camera_geometry_crop(frame->width, frame->height, OUT_W, OUT_H, SWAP_AXES, &crop),
                        ESP_ERR_INVALID_SIZE, TAG, "no exact crop for %lux%lu -> %dx%d",
                        (unsigned long)frame->width, (unsigned long)frame->height, OUT_W, OUT_H);
    ESP_RETURN_ON_ERROR(esp_cache_msync((void *)frame->data, frame->size,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
                        TAG, "invalidate frame cache failed");
    ESP_RETURN_ON_ERROR(esp_cache_msync(s.rgb, s.rgb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                        TAG, "clean output cache failed");

    ppa_client_handle_t ppa = NULL;
    const ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &ppa), TAG, "PPA client failed");
    const float scale = (float)crop.scale_steps / (float)CAMERA_SCALE_STEPS;
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = frame->data,
            .pic_w = stride_px,
            .pic_h = frame->height,
            .block_w = crop.block_w,
            .block_h = crop.block_h,
            .block_offset_x = crop.offset_x,
            .block_offset_y = crop.offset_y,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = s.rgb,
            .buffer_size = s.rgb_size,
            .pic_w = OUT_W,
            .pic_h = OUT_H,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = ROTATION,
        .scale_x = scale,
        .scale_y = scale,
        .mirror_x = MIRROR_X,
        .mirror_y = MIRROR_Y,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa, &op);
    (void)ppa_unregister_client(ppa);
    ESP_RETURN_ON_ERROR(err, TAG, "PPA scale/rotate failed");
    ESP_LOGD(TAG, "crop %lux%lu at (%lu,%lu), scale %lu/16", (unsigned long)crop.block_w,
             (unsigned long)crop.block_h, (unsigned long)crop.offset_x, (unsigned long)crop.offset_y,
             (unsigned long)crop.scale_steps);
    return esp_cache_msync(s.rgb, s.rgb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

/* Open, settle, grab one frame, convert it, and always release the camera. */
static esp_err_t grab_converted_frame(void)
{
    mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();
    cfg.buffer_count = 2;
    cfg.frame_timeout_ms = FRAME_TIMEOUT_MS;
    /* Both modules support 1280x720 UYVY (SC101IOT default; the OV3640 default
     * is RGB565 640x480, which the PPA path below does not take). */
    if (!s_sensor_default_format) {
        cfg.width = CAPTURE_W;
        cfg.height = CAPTURE_H;
    }
    cfg.pixel_format = V4L2_PIX_FMT_UYVY;
    mosaico_camera_handle_t camera = NULL;
    esp_err_t err = mosaico_camera_new(&cfg, &camera);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no camera module (%s)", esp_err_to_name(err));
        return (err == ESP_ERR_NO_MEM) ? err : ESP_ERR_NOT_FOUND;
    }
    mosaico_camera_frame_t frame = { 0 };
    bool borrowed = false;
    err = mosaico_camera_open(camera);
    if (err == ESP_OK) {
        err = mosaico_camera_start_stream(camera);
    }
    if (err == ESP_OK && SETTLE_FRAMES > 0) {
        err = mosaico_camera_discard_frames(camera, SETTLE_FRAMES); /* auto exposure */
    }
    if (err == ESP_OK) {
        err = mosaico_camera_get_frame(camera, &frame);
        borrowed = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = convert_frame(&frame);
    }
    if (borrowed) {
        (void)mosaico_camera_return_frame(camera, &frame);
    }
    const esp_err_t del_err = mosaico_camera_del(camera); /* stops, closes, releases USJ pads */
    if (del_err != ESP_OK) {
        ESP_LOGE(TAG, "camera cleanup failed (%s)", esp_err_to_name(del_err));
    }
    return err != ESP_OK ? err : del_err;
}

static esp_err_t encode_jpeg(size_t max_bytes, uint32_t *out_size, uint8_t *out_quality)
{
    jpeg_encoder_handle_t enc = NULL;
    const jpeg_encode_engine_cfg_t eng_cfg = { .timeout_ms = JPEG_TIMEOUT_MS };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng_cfg, &enc), TAG, "JPEG engine failed");
    const uint32_t limit = (uint32_t)(max_bytes < s.jpeg_size ? max_bytes : s.jpeg_size);
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < sizeof(QUALITIES); ++i) {
        const jpeg_encode_cfg_t cfg = {
            .width = OUT_W,
            .height = OUT_H,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = QUALITIES[i],
        };
        uint32_t size = 0;
        /* The output buffer is the full capacity; the byte budget is checked after. */
        const esp_err_t r = jpeg_encoder_process(enc, &cfg, s.rgb, (uint32_t)OUT_W * OUT_H * 2U,
                                                 s.jpeg, (uint32_t)s.jpeg_size, &size);
        if (r == ESP_OK && size > 0 && size <= limit) {
            *out_size = size;
            *out_quality = QUALITIES[i];
            err = ESP_OK;
            break;
        }
        ESP_LOGD(TAG, "quality %u: %s, %lu B", (unsigned)QUALITIES[i], esp_err_to_name(r),
                 (unsigned long)size);
    }
    (void)jpeg_del_encoder_engine(enc);
    return err;
}

esp_err_t app_camera_capture_jpeg(size_t max_bytes, app_camera_still_t *out)
{
    if (!out || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const int64_t start = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(ensure_buffers(), TAG, "PSRAM buffers unavailable");
    ESP_RETURN_ON_ERROR(grab_converted_frame(), TAG, "capture failed");
    uint32_t size = 0;
    uint8_t quality = 0;
    ESP_RETURN_ON_ERROR(encode_jpeg(max_bytes, &size, &quality), TAG,
                        "JPEG does not fit %u bytes", (unsigned)max_bytes);
    out->data = s.jpeg;
    out->size = size;
    out->width = OUT_W;
    out->height = OUT_H;
    out->quality = quality;
    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    ESP_LOGI(TAG, "still %dx%d JPEG %lu B (q%u) in %lu ms", OUT_W, OUT_H, (unsigned long)size,
             (unsigned)quality, (unsigned long)out->elapsed_ms);
    return ESP_OK;
}

#else /* !CONFIG_VOX_CAMERA */

void app_camera_use_sensor_default_format(bool use_default)
{
    (void)use_default;
}


esp_err_t app_camera_capture_jpeg(size_t max_bytes, app_camera_still_t *out)
{
    (void)max_bytes;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
