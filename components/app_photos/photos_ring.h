// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Private to app_photos: pure ring bookkeeping over the slot headers, also
 * built on the PC (tools/host_tests/test_photos_ring.c). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHOTOS_RING_MAX_SLOTS 16

typedef struct {
    bool valid;      /* header magic/version/size accepted */
    uint32_t seq;    /* monotonic save counter, larger = newer */
} photos_slot_info_t;

typedef struct {
    size_t count;                         /* valid photos */
    uint8_t order[PHOTOS_RING_MAX_SLOTS]; /* slot index by age: [0] oldest .. [count-1] newest */
    uint32_t max_seq;                     /* 0 when empty */
} photos_ring_t;

/** Sort the valid slots by sequence number (oldest first). */
void photos_ring_build(const photos_slot_info_t *slots, size_t slot_count, photos_ring_t *ring);

/** Slot for the next save: the first invalid slot, otherwise the oldest photo. */
size_t photos_ring_next_slot(const photos_slot_info_t *slots, size_t slot_count,
                             const photos_ring_t *ring);
