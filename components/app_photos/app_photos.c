// SPDX-License-Identifier: Apache-2.0

#include "app_photos.h"
#include "photos_ring.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "sdkconfig.h"

static const char *TAG = "app_photos";

#define PHOTOS_SUBTYPE      0x41
#define PHOTOS_LABEL        "photos"
#define SLOT_COUNT          CONFIG_VOX_PHOTOS_SLOTS
#define SLOT_SIZE           (48 * 1024)          /* 12 flash sectors */
#define HEADER_SIZE         32
#define PHOTO_MAGIC         0x50584F56u          /* "VOXP" little endian */
#define PHOTO_VERSION       1u

_Static_assert(SLOT_COUNT <= PHOTOS_RING_MAX_SLOTS, "too many photo slots");

/* Slot layout: header (written last, so a cut save leaves no valid header)
 * followed by the JPEG bytes. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t size;
    uint32_t crc;      /* esp_rom_crc32_le over the JPEG bytes */
    uint8_t reserved[HEADER_SIZE - 20];
} photo_header_t;

_Static_assert(sizeof(photo_header_t) == HEADER_SIZE, "photo header size");

/* Single owner (the session task): no locking. */
static struct {
    const esp_partition_t *part;
    photos_slot_info_t slots[SLOT_COUNT];
    photos_ring_t ring;
} s;

static bool header_ok(const photo_header_t *h)
{
    return h->magic == PHOTO_MAGIC && h->version == PHOTO_VERSION && h->size > 0 &&
           h->size <= SLOT_SIZE - HEADER_SIZE;
}

esp_err_t app_photos_init(void)
{
    memset(&s, 0, sizeof(s));
    s.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PHOTOS_SUBTYPE, PHOTOS_LABEL);
    if (!s.part) {
        ESP_LOGW(TAG, "no \"photos\" partition; stills are not stored");
        return ESP_ERR_NOT_FOUND;
    }
    if (s.part->size < (size_t)SLOT_COUNT * SLOT_SIZE) {
        ESP_LOGE(TAG, "\"photos\" partition too small (%lu B)", (unsigned long)s.part->size);
        s.part = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < SLOT_COUNT; ++i) {
        photo_header_t h;
        if (esp_partition_read(s.part, i * SLOT_SIZE, &h, sizeof(h)) == ESP_OK && header_ok(&h)) {
            s.slots[i].valid = true;
            s.slots[i].seq = h.seq;
        }
    }
    photos_ring_build(s.slots, SLOT_COUNT, &s.ring);
    ESP_LOGI(TAG, "%u stored still(s) of %d", (unsigned)s.ring.count, SLOT_COUNT);
    return ESP_OK;
}

size_t app_photos_count(void)
{
    return s.part ? s.ring.count : 0;
}

size_t app_photos_capacity(void)
{
    return SLOT_COUNT;
}

size_t app_photos_max_size(void)
{
    return SLOT_SIZE - HEADER_SIZE;
}

esp_err_t app_photos_save(const uint8_t *jpeg, size_t size)
{
    if (!jpeg || size == 0 || size > SLOT_SIZE - HEADER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.part) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t slot = photos_ring_next_slot(s.slots, SLOT_COUNT, &s.ring);
    const size_t base = slot * SLOT_SIZE;
    const uint32_t seq = s.ring.max_seq + 1U; /* before the slot leaves the ring */
    s.slots[slot].valid = false; /* until the header is written */
    photos_ring_build(s.slots, SLOT_COUNT, &s.ring);
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s.part, base, SLOT_SIZE), TAG, "erase slot %u",
                        (unsigned)slot);
    ESP_RETURN_ON_ERROR(esp_partition_write(s.part, base + HEADER_SIZE, jpeg, size), TAG,
                        "write photo");
    const photo_header_t h = {
        .magic = PHOTO_MAGIC,
        .version = PHOTO_VERSION,
        .seq = seq,
        .size = (uint32_t)size,
        .crc = esp_rom_crc32_le(0, jpeg, size),
    };
    ESP_RETURN_ON_ERROR(esp_partition_write(s.part, base, &h, sizeof(h)), TAG, "write header");
    s.slots[slot].valid = true;
    s.slots[slot].seq = h.seq;
    photos_ring_build(s.slots, SLOT_COUNT, &s.ring);
    ESP_LOGI(TAG, "still saved in slot %u (%u B); %u stored", (unsigned)slot, (unsigned)size,
             (unsigned)s.ring.count);
    return ESP_OK;
}

esp_err_t app_photos_read(size_t position, uint8_t *buf, size_t cap, size_t *out_size)
{
    if (!buf || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = 0;
    if (!s.part || position >= s.ring.count) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t base = (size_t)s.ring.order[position] * SLOT_SIZE;
    photo_header_t h;
    ESP_RETURN_ON_ERROR(esp_partition_read(s.part, base, &h, sizeof(h)), TAG, "read header");
    if (!header_ok(&h)) {
        return ESP_ERR_INVALID_CRC;
    }
    if (h.size > cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(esp_partition_read(s.part, base + HEADER_SIZE, buf, h.size), TAG,
                        "read photo");
    if (esp_rom_crc32_le(0, buf, h.size) != h.crc) {
        ESP_LOGW(TAG, "photo %u damaged (CRC)", (unsigned)position);
        return ESP_ERR_INVALID_CRC;
    }
    *out_size = h.size;
    return ESP_OK;
}
