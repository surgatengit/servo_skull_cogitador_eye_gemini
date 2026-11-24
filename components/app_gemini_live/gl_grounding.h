// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Private to app_gemini_live: accumulates the groundingMetadata of one answer
 * (search queries and source domains). The WebSocket task writes, any task
 * reads; a dedicated mutex guards only short copies, so it never nests with
 * the client's send lock. Buffers live in PSRAM. */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    SemaphoreHandle_t lock;
    char *queries;   /* '\n'-separated unique webSearchQueries */
    char *sources;   /* '\n'-separated unique domains */
    size_t query_count;
    size_t source_count;
} gl_grounding_t;

esp_err_t gl_grounding_init(gl_grounding_t *g);
void gl_grounding_deinit(gl_grounding_t *g);

/** Merge one groundingMetadata object. Returns the number of new queries. */
size_t gl_grounding_merge(gl_grounding_t *g, const cJSON *metadata);

/** Copy whole '\n'-separated entries; returns how many fit. */
size_t gl_grounding_copy(gl_grounding_t *g, bool queries, char *out, size_t cap);

void gl_grounding_clear(gl_grounding_t *g);
