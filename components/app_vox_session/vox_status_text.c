// SPDX-License-Identifier: Apache-2.0

#include "vox_status_text.h"

#include <stdbool.h>
#include <string.h>

#define ELLIPSIS     "..."
#define ELLIPSIS_LEN 3

typedef struct {
    char *out;
    size_t cap;
    size_t used;    /* bytes */
    size_t chars;   /* characters */
    size_t max_chars;
    bool cut;
} line_t;

/* Append one character of `bytes` bytes; false when the line is full. */
static bool put(line_t *l, const char *bytes, size_t n)
{
    if (l->chars + 1 > l->max_chars || l->used + n + 1 > l->cap) {
        l->cut = true;
        return false;
    }
    memcpy(l->out + l->used, bytes, n);
    l->used += n;
    l->chars++;
    l->out[l->used] = '\0';
    return true;
}

/* Map one input character to the font's charset. Returns the bytes consumed
 * (>= 1) and fills mapped / mapped_len (0 length = drop, " " = separator). */
static size_t map_char(const unsigned char *p, size_t left, char mapped[2], size_t *mapped_len)
{
    const unsigned char c = p[0];
    *mapped_len = 0;
    if (c < 0x80) {
        if (c >= 'a' && c <= 'z') {
            mapped[0] = (char)(c - 'a' + 'A');
            *mapped_len = 1;
        } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                   c == ':' || c == '/') {
            mapped[0] = (char)c;
            *mapped_len = 1;
        } else {
            mapped[0] = ' '; /* punctuation and spaces separate words */
            *mapped_len = 1;
        }
        return 1;
    }
    size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len > left) {
        len = left;
    }
    if (c == 0xC3 && len == 2) {
        /* Latin-1 supplement: lower → upper for the accented letters the font has. */
        static const unsigned char from[] = { 0xA1, 0xA9, 0xAD, 0xB3, 0xBA, 0xB1,
                                              0x81, 0x89, 0x8D, 0x93, 0x9A, 0x91 };
        static const unsigned char to[] = { 0x81, 0x89, 0x8D, 0x93, 0x9A, 0x91,
                                            0x81, 0x89, 0x8D, 0x93, 0x9A, 0x91 };
        for (size_t i = 0; i < sizeof(from); ++i) {
            if (p[1] == from[i]) {
                mapped[0] = (char)c;
                mapped[1] = (char)to[i];
                *mapped_len = 2;
                return len;
            }
        }
        if (p[1] == 0xBC || p[1] == 0x9C) { /* ü Ü */
            mapped[0] = 'U';
            *mapped_len = 1;
        }
    }
    return len;
}

size_t vox_status_text(const char *prefix, const char *text, size_t text_len,
                       char *out, size_t cap, size_t max_chars)
{
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    line_t l = { .out = out, .cap = cap, .max_chars = max_chars };
    if (prefix) {
        for (const char *p = prefix; *p && put(&l, p, 1); ++p) {
        }
    }
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    size_t left = text ? text_len : 0;
    bool pending_space = false;
    size_t last_word_end = 0;        /* where to cut when ellipsizing */
    size_t last_word_chars = 0;
    while (left > 0 && *p && !l.cut) {
        char mapped[2];
        size_t mapped_len;
        const size_t n = map_char(p, left, mapped, &mapped_len);
        p += n;
        left -= n;
        if (mapped_len == 0) {
            continue;
        }
        if (mapped_len == 1 && mapped[0] == ' ') {
            pending_space = l.chars > 0 && l.out[l.used - 1] != ' ';
            continue;
        }
        if (pending_space) {
            if (l.chars + ELLIPSIS_LEN <= max_chars) { /* "..." would still fit here */
                last_word_end = l.used;
                last_word_chars = l.chars;
            }
            if (!put(&l, " ", 1)) {
                break;
            }
            pending_space = false;
        }
        (void)put(&l, mapped, mapped_len);
    }
    if (l.cut) {
        /* Cut at a word boundary when one exists, then add "..." within the limit. */
        if (last_word_chars >= max_chars / 2) {
            l.used = last_word_end;
            l.chars = last_word_chars;
        }
        while (l.chars + ELLIPSIS_LEN > max_chars && l.used > 0) {
            do {
                l.used--;
            } while (l.used > 0 && ((unsigned char)l.out[l.used] & 0xC0) == 0x80);
            l.chars--;
        }
        while (l.used > 0 && l.out[l.used - 1] == ' ') {
            l.used--;
            l.chars--;
        }
        if (l.used + ELLIPSIS_LEN + 1 <= cap && l.chars + ELLIPSIS_LEN <= max_chars) {
            memcpy(l.out + l.used, ELLIPSIS, ELLIPSIS_LEN);
            l.used += ELLIPSIS_LEN;
            l.chars += ELLIPSIS_LEN;
        }
        l.out[l.used] = '\0';
    }
    return l.chars;
}
