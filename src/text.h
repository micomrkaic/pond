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

/* text.h — draw 8x8 bitmap text and filled boxes into an RGBA8 byte buffer. */
#ifndef POND_TEXT_H
#define POND_TEXT_H

#include <stdint.h>

typedef struct {
    uint8_t *rgba;   /* w*h*4 bytes, row-major, top row first */
    int w, h;
} canvas;

void canvas_clear(canvas *c);
void canvas_fill(canvas *c, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/* draws s at (x, y) with glyphs scaled by `scale`; returns the x after the last glyph */
int  canvas_text(canvas *c, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char *s);
int  text_width(const char *s, int scale);

#endif
