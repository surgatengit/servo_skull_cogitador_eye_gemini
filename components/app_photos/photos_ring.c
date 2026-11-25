// SPDX-License-Identifier: Apache-2.0

#include "photos_ring.h"

#include <string.h>

void photos_ring_build(const photos_slot_info_t *slots, size_t slot_count, photos_ring_t *ring)
{
    memset(ring, 0, sizeof(*ring));
    if (!slots || slot_count > PHOTOS_RING_MAX_SLOTS) {
        return;
    }
    for (size_t i = 0; i < slot_count; ++i) {
        if (!slots[i].valid) {
            continue;
        }
        /* Insertion sort by seq: at most 16 entries. */
        size_t pos = ring->count;
        while (pos > 0 && slots[ring->order[pos - 1]].seq > slots[i].seq) {
            ring->order[pos] = ring->order[pos - 1];
            pos--;
        }
        ring->order[pos] = (uint8_t)i;
        ring->count++;
        if (slots[i].seq > ring->max_seq) {
            ring->max_seq = slots[i].seq;
        }
    }
}

size_t photos_ring_next_slot(const photos_slot_info_t *slots, size_t slot_count,
                             const photos_ring_t *ring)
{
    for (size_t i = 0; i < slot_count; ++i) {
        if (!slots[i].valid) {
            return i;
        }
    }
    return ring->count > 0 ? ring->order[0] : 0; /* full: replace the oldest */
}
