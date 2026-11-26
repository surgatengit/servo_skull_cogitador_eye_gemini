// SPDX-License-Identifier: Apache-2.0
/* Pure text helpers for the secret card (host-tested, no ESP-IDF). */
#include "secret_text.h"

#include <string.h>

/* Copy one UTF-8 code point from src into dst, mapping it to the card charset.
 * Returns bytes consumed from src; *written gets bytes written (0 = dropped). */
static size_t map_codepoint(const unsigned char *src, char *dst, size_t *written, bool upper)
{
    const unsigned char c = src[0];
    *written = 0;
    if (c < 0x80) {
        if (c == '\n' || c == '\r' || c == '\t') {
            dst[0] = ' ';
        } else if (c < 0x20) {
            return 1;
        } else {
            dst[0] = (char)((upper && c >= 'a' && c <= 'z') ? c - 32 : c);
        }
        *written = 1;
        return 1;
    }
    if (c == 0xC2 && src[1] >= 0x80) {          /* ¡ ¿ · and friends */
        if (src[1] == 0xA1 || src[1] == 0xBF || src[1] == 0xB7) {
            dst[0] = (char)c;
            dst[1] = (char)src[1];
            *written = 2;
        }
        return 2;
    }
    if (c == 0xC3 && src[1] >= 0x80) {          /* Latin-1 letters */
        unsigned char d = src[1];
        /* lowercase á é í ó ú ñ ü */
        const bool lower = d == 0xA1 || d == 0xA9 || d == 0xAD || d == 0xB3 || d == 0xBA ||
                           d == 0xB1 || d == 0xBC;
        const bool capital = d == 0x81 || d == 0x89 || d == 0x8D || d == 0x93 || d == 0x9A ||
                             d == 0x91 || d == 0x9C;
        if (lower || capital) {
            if (upper && lower) {
                d = (unsigned char)(d - 0x20);
            }
            dst[0] = (char)c;
            dst[1] = (char)d;
            *written = 2;
        } else if (d == 0xA0 || d == 0xA2 || d == 0xA4 || d == 0xA8 || d == 0xAA) {
            dst[0] = upper ? 'A' : 'a';           /* à â ä è ê -> plain vowels */
            if (d >= 0xA8) {
                dst[0] = upper ? 'E' : 'e';
            }
            *written = 1;
        }
        return 2;
    }
    if (c == 0xE2 && src[1] == 0x80 && src[2] >= 0x80) {   /* typographic punctuation */
        const unsigned char d = src[2];
        if (d == 0x9C || d == 0x9D) {            /* “ ” */
            dst[0] = '"';
            *written = 1;
        } else if (d == 0x98 || d == 0x99) {     /* ‘ ’ */
            dst[0] = '\'';
            *written = 1;
        } else if (d == 0x93 || d == 0x94) {     /* – — */
            dst[0] = '-';
            *written = 1;
        } else if (d == 0xA6) {                  /* … */
            memcpy(dst, "...", 3);
            *written = 3;
        }
        return 3;
    }
    /* Anything else (emoji, other scripts): skip the whole sequence. */
    size_t len = 1;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
    }
    for (size_t i = 1; i < len; ++i) {
        if (src[i] == 0) {
            return i;
        }
    }
    return len;
}

size_t secret_text_sanitize(const char *src, char *dst, size_t cap, bool upper)
{
    if (!dst || cap == 0) {
        return 0;
    }
    size_t out = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p) {
        char tmp[4];
        size_t written;
        p += map_codepoint(p, tmp, &written, upper);
        if (written == 0) {
            continue;
        }
        if (out + written >= cap) {
            break;
        }
        /* Collapse runs of spaces. */
        if (written == 1 && tmp[0] == ' ' && (out == 0 || dst[out - 1] == ' ')) {
            continue;
        }
        memcpy(dst + out, tmp, written);
        out += written;
    }
    while (out > 0 && dst[out - 1] == ' ') {
        out--;
    }
    dst[out] = '\0';
    return out;
}

static size_t glyphs(const char *s, size_t bytes)
{
    size_t n = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

size_t secret_text_wrap(const char *clean, size_t columns, size_t max_lines, char *dst,
                        size_t cap)
{
    if (!dst || cap == 0) {
        return 0;
    }
    dst[0] = '\0';
    if (!clean || columns == 0 || max_lines == 0) {
        return 0;
    }
    size_t out = 0;
    size_t lines = 0;
    size_t line_glyphs = 0;
    const char *p = clean;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = p;
        while (*end && *end != ' ') {
            end++;
        }
        size_t word_bytes = (size_t)(end - p);
        size_t word_glyphs = glyphs(p, word_bytes);
        if (word_glyphs > columns) {               /* hard-split very long words */
            size_t b = 0;
            size_t g = 0;
            while (b < word_bytes && g < columns) {
                b++;
                while (b < word_bytes && ((unsigned char)p[b] & 0xC0) == 0x80) {
                    b++;
                }
                g++;
            }
            word_bytes = b;
            word_glyphs = g;
            end = p + b;
        }
        const bool fits = line_glyphs == 0 || line_glyphs + 1 + word_glyphs <= columns;
        if (!fits) {
            if (lines + 1 >= max_lines) {
                break;
            }
            if (out + 1 >= cap) {
                break;
            }
            dst[out++] = '\n';
            lines++;
            line_glyphs = 0;
        } else if (line_glyphs > 0) {
            if (out + 1 >= cap) {
                break;
            }
            dst[out++] = ' ';
            line_glyphs++;
        }
        if (out + word_bytes >= cap) {
            break;
        }
        memcpy(dst + out, p, word_bytes);
        out += word_bytes;
        line_glyphs += word_glyphs;
        p = end;
    }
    dst[out] = '\0';
    return lines + (out > 0 ? 1 : 0);
}
