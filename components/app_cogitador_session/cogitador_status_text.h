// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Private to app_cogitador_session: format free UTF-8 text (search queries, source
 * domains) for the eye's status line, whose font only has
 * " ABCDEFGHIJKLMNOPQRSTUVWXYZÁÉÍÓÚÑ0123456789-·.:/". Pure functions, no
 * allocation; also built on the PC for tests (tools/test_status_text.c). */

#include <stddef.h>

/** Write prefix + text into out: Spanish letters uppercased (á→Á, ñ→Ñ, ü→U),
 * other unsupported characters dropped, spaces collapsed, and the whole line
 * limited to max_chars characters (ending in "..." when cut). out always ends
 * NUL-terminated (cap > 0). Returns the number of characters written. */
size_t cogitador_status_text(const char *prefix, const char *text, size_t text_len,
                       char *out, size_t cap, size_t max_chars);
