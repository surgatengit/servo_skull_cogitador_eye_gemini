// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Map UTF-8 text to the secret card charset (ASCII, Spanish accents, ñ, ü,
 * ¡ ¿ ·): typographic quotes/dashes become ASCII, line breaks become spaces,
 * runs of spaces collapse, other code points (emoji...) are dropped. With
 * upper, letters are upper-cased (accented ones too). Always NUL-terminates.
 * Returns the bytes written.
 */
size_t secret_text_sanitize(const char *src, char *dst, size_t cap, bool upper);

/**
 * Word-wrap sanitized text into at most max_lines lines of `columns` glyphs,
 * separated by '\n'. Words longer than a line are split; text beyond the last
 * line is dropped. Always NUL-terminates. Returns the number of lines.
 */
size_t secret_text_wrap(const char *clean, size_t columns, size_t max_lines, char *dst,
                        size_t cap);

#ifdef __cplusplus
}
#endif
