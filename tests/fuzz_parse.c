/* pond — a numerically honest wave tank as screen candy
 * Copyright (C) 2026 Mico <https://github.com/micomrkaic>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* The text pond reads from files other people wrote, under libFuzzer:
 * scripts (script_parse) and config settings (config_set, line by line, as
 * config_load splits them).  Whatever the bytes, the answer is a script or
 * an error message — never a crash, a hang, or a sanitizer report.
 *
 *   make fuzz                  # 60 s, corpus seeded from demos/
 *   make fuzz FUZZ_TIME=600    # longer
 */
#include "../src/script.h"
#include "../src/config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *text = malloc(size + 1);
    if (!text) return 0;
    memcpy(text, data, size);
    text[size] = 0;                     /* a file stops at its first NUL for us too */

    char err[256];
    script *s = script_parse(text, "fuzz", err, sizeof err);
    script_free(s);

    /* each line as a config setting: key, then the value after the first = or : */
    pond_config c;
    config_defaults(&c);
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *sep = strpbrk(line, "=:");
        if (!sep) continue;
        *sep = 0;
        config_set(&c, line, sep + 1);
    }

    free(text);
    return 0;
}
