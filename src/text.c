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

#include "text.h"
#include "font8x8.h"
#include <string.h>

void canvas_clear(canvas *c)
{
    memset(c->rgba, 0, (size_t)c->w * c->h * 4);
}

static inline void blend_px(canvas *c, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    uint8_t *p = c->rgba + ((size_t)y * c->w + x) * 4;
    /* "over" compositing on straight-alpha RGBA */
    int oa = p[3], na = a + oa * (255 - a) / 255;
    if (na == 0) { p[0] = p[1] = p[2] = p[3] = 0; return; }
    p[0] = (uint8_t)((r * a + p[0] * oa * (255 - a) / 255) / na);
    p[1] = (uint8_t)((g * a + p[1] * oa * (255 - a) / 255) / na);
    p[2] = (uint8_t)((b * a + p[2] * oa * (255 - a) / 255) / na);
    p[3] = (uint8_t)na;
}

void canvas_fill(canvas *c, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) blend_px(c, i, j, r, g, b, a);
}

int canvas_text(canvas *c, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char *s)
{
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch >= 128) ch = '?';
        const unsigned char *gl = font8x8_basic[ch];
        for (int row = 0; row < 8; row++) {
            unsigned bits = gl[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1u << col))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        blend_px(c, x + col * scale + sx, y + row * scale + sy, r, g, b, a);
            }
        }
        x += 8 * scale;
    }
    return x;
}

int text_width(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}
