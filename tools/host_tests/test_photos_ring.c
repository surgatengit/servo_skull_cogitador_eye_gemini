// SPDX-License-Identifier: Apache-2.0
/* PC tests for the photo ring bookkeeping (components/app_photos/photos_ring.c):
 * order by age, next slot while filling, replacing the oldest when full, and
 * holes left by damaged slots. Built and run by tools/host_tests/run.ps1. */

#include <stdio.h>
#include <string.h>

#include "photos_ring.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

enum { SLOTS = 10 };

/* Simulate app_photos_save(): pick a slot, give it the next sequence number. */
static size_t save(photos_slot_info_t *slots)
{
    photos_ring_t ring;
    photos_ring_build(slots, SLOTS, &ring);
    const size_t slot = photos_ring_next_slot(slots, SLOTS, &ring);
    slots[slot].valid = true;
    slots[slot].seq = ring.max_seq + 1U;
    return slot;
}

int main(void)
{
    photos_slot_info_t slots[SLOTS];
    memset(slots, 0, sizeof(slots));
    photos_ring_t ring;

    /* Empty partition (garbage headers are simply invalid). */
    photos_ring_build(slots, SLOTS, &ring);
    CHECK(ring.count == 0 && ring.max_seq == 0);
    CHECK(photos_ring_next_slot(slots, SLOTS, &ring) == 0);

    /* Filling: slots 0..9 in order, newest last. */
    for (size_t i = 0; i < SLOTS; ++i) {
        CHECK(save(slots) == i);
    }
    photos_ring_build(slots, SLOTS, &ring);
    CHECK(ring.count == SLOTS && ring.max_seq == SLOTS);
    CHECK(ring.order[0] == 0 && ring.order[SLOTS - 1] == SLOTS - 1);

    /* Full: the 11th still replaces the oldest (slot 0), then slot 1, ... */
    CHECK(save(slots) == 0);
    CHECK(save(slots) == 1);
    photos_ring_build(slots, SLOTS, &ring);
    CHECK(ring.count == SLOTS && ring.max_seq == SLOTS + 2);
    CHECK(ring.order[0] == 2);              /* oldest is now slot 2 */
    CHECK(ring.order[SLOTS - 1] == 1);      /* newest is slot 1 */
    CHECK(ring.order[SLOTS - 2] == 0);

    /* A damaged slot (cut save) becomes a hole and is reused first. */
    slots[5].valid = false;
    photos_ring_build(slots, SLOTS, &ring);
    CHECK(ring.count == SLOTS - 1);
    CHECK(photos_ring_next_slot(slots, SLOTS, &ring) == 5);
    CHECK(save(slots) == 5);
    photos_ring_build(slots, SLOTS, &ring);
    CHECK(ring.order[SLOTS - 1] == 5 && ring.max_seq == SLOTS + 3);

    /* Order survives an arbitrary scan order of sequence numbers. */
    photos_slot_info_t mixed[4] = { { true, 40 }, { true, 10 }, { false, 99 }, { true, 25 } };
    photos_ring_build(mixed, 4, &ring);
    CHECK(ring.count == 3 && ring.order[0] == 1 && ring.order[1] == 3 && ring.order[2] == 0);
    CHECK(ring.max_seq == 40);

    /* Too many slots is refused (empty ring). */
    photos_slot_info_t many[PHOTOS_RING_MAX_SLOTS + 1];
    memset(many, 0, sizeof(many));
    photos_ring_build(many, PHOTOS_RING_MAX_SLOTS + 1, &ring);
    CHECK(ring.count == 0);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all photo ring host tests passed\n");
    return 0;
}
