// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Persistent ring of the latest JPEG stills in the "photos" flash partition
 * (contracts: ../README.md). Single owner: call every function from one task
 * with an internal-RAM stack (the VOX session task); flash erase/write must not
 * run on a task whose stack lives in PSRAM.
 */

/** Scan the partition. ESP_ERR_NOT_FOUND when the partition is missing
 * (old partition table): the other calls then report no photos. */
esp_err_t app_photos_init(void);

/** Valid photos stored (0..app_photos_capacity()). */
size_t app_photos_count(void);

/** Slots in the ring (oldest is replaced when full). */
size_t app_photos_capacity(void);

/** Largest JPEG a slot can hold. */
size_t app_photos_max_size(void);

/** Store one JPEG (erases one slot; about 0.5 s). */
esp_err_t app_photos_save(const uint8_t *jpeg, size_t size);

/** Erase every slot completely (not only the headers): the stills are gone
 * from flash. Takes a few seconds. */
esp_err_t app_photos_clear(void);

/** Copy photo `position` (0 = oldest .. count-1 = newest) into buf and check
 * its CRC. ESP_ERR_INVALID_CRC for a damaged slot. */
esp_err_t app_photos_read(size_t position, uint8_t *buf, size_t cap, size_t *out_size);

#ifdef __cplusplus
}
#endif
