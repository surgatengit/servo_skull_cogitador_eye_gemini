// SPDX-License-Identifier: Apache-2.0

#include "gl_grounding.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gemini_live";

#define GROUNDING_CAPACITY  384   /* per list; a few queries or domains per answer */
#define ENTRY_MAX           120   /* longer queries/domains are cut at a UTF-8 boundary */

esp_err_t gl_grounding_init(gl_grounding_t *g)
{
    memset(g, 0, sizeof(*g));
    g->lock = xSemaphoreCreateMutex();
    g->queries = heap_caps_calloc(1, GROUNDING_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g->sources = heap_caps_calloc(1, GROUNDING_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g->lock || !g->queries || !g->sources) {
        gl_grounding_deinit(g);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gl_grounding_deinit(gl_grounding_t *g)
{
    if (g->lock) {
        vSemaphoreDelete(g->lock);
    }
    heap_caps_free(g->queries);
    heap_caps_free(g->sources);
    memset(g, 0, sizeof(*g));
}

/* Is `entry` (len bytes) already a line of `list`? */
static bool contains(const char *list, const char *entry, size_t len)
{
    for (const char *p = list; *p;) {
        const size_t n = strcspn(p, "\n");
        if (n == len && memcmp(p, entry, len) == 0) {
            return true;
        }
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
    return false;
}

/* Append a unique entry; false when it is a duplicate or does not fit. */
static bool append(char *list, size_t *count, const char *entry, size_t len)
{
    if (len == 0 || contains(list, entry, len)) {
        return false;
    }
    const size_t used = strlen(list);
    const size_t sep = used ? 1 : 0;
    if (used + sep + len + 1 > GROUNDING_CAPACITY) {
        return false;
    }
    if (sep) {
        list[used] = '\n';
    }
    memcpy(list + used + sep, entry, len);
    list[used + sep + len] = '\0';
    (*count)++;
    return true;
}

/* Trim to max bytes without splitting a UTF-8 sequence; newlines become spaces. */
static size_t clean_text(const char *in, char *out, size_t max)
{
    size_t len = strnlen(in, max);
    while (len > 0 && len < strlen(in) && ((unsigned char)in[len] & 0xC0) == 0x80) {
        len--;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = (in[i] == '\n' || in[i] == '\r') ? ' ' : in[i];
    }
    return len;
}

/* Domain from a grounding chunk title (the API puts the site there, e.g.
 * "wikipedia.org"); lowercase ASCII, "www." stripped. URIs are redirect links
 * through vertexaisearch.cloud.google.com and are not useful on the panel. */
static size_t clean_domain(const char *in, char *out, size_t max)
{
    if (strncmp(in, "www.", 4) == 0) {
        in += 4;
    }
    size_t len = 0;
    for (; *in && len < max; ++in) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-') {
            out[len++] = c;
        } else if (c == ' ' || c == '/' || c == '|') {
            break; /* a page title rather than a domain: keep its first word */
        }
    }
    return len;
}

size_t gl_grounding_merge(gl_grounding_t *g, const cJSON *metadata)
{
    char entry[ENTRY_MAX];
    size_t new_queries = 0;
    xSemaphoreTake(g->lock, portMAX_DELAY);
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(metadata, "webSearchQueries")) {
        if (cJSON_IsString(item)) {
            const size_t n = clean_text(item->valuestring, entry, sizeof(entry));
            new_queries += append(g->queries, &g->query_count, entry, n) ? 1U : 0U;
        }
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(metadata, "groundingChunks")) {
        const cJSON *web = cJSON_GetObjectItemCaseSensitive(item, "web");
        const cJSON *domain = cJSON_GetObjectItemCaseSensitive(web, "domain");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(web, "title");
        const cJSON *name = cJSON_IsString(domain) ? domain : title;
        if (cJSON_IsString(name)) {
            const size_t n = clean_domain(name->valuestring, entry, sizeof(entry));
            (void)append(g->sources, &g->source_count, entry, n);
        }
    }
    xSemaphoreGive(g->lock);
    const cJSON *entry_point = cJSON_GetObjectItemCaseSensitive(metadata, "searchEntryPoint");
    const cJSON *rendered = cJSON_GetObjectItemCaseSensitive(entry_point, "renderedContent");
    ESP_LOGD(TAG, "groundingMetadata: %u new queries, searchEntryPoint %u bytes",
             (unsigned)new_queries,
             cJSON_IsString(rendered) ? (unsigned)strlen(rendered->valuestring) : 0U);
    return new_queries;
}

size_t gl_grounding_copy(gl_grounding_t *g, bool queries, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t copied = 0;
    size_t used = 0;
    xSemaphoreTake(g->lock, portMAX_DELAY);
    for (const char *p = queries ? g->queries : g->sources; *p;) {
        const size_t n = strcspn(p, "\n");
        const size_t sep = used ? 1 : 0;
        if (used + sep + n + 1 > cap) {
            break;
        }
        if (sep) {
            out[used++] = '\n';
        }
        memcpy(out + used, p, n);
        used += n;
        out[used] = '\0';
        copied++;
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
    xSemaphoreGive(g->lock);
    return copied;
}

void gl_grounding_clear(gl_grounding_t *g)
{
    xSemaphoreTake(g->lock, portMAX_DELAY);
    g->queries[0] = '\0';
    g->sources[0] = '\0';
    g->query_count = 0;
    g->source_count = 0;
    xSemaphoreGive(g->lock);
}
