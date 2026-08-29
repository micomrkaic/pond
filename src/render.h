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

/* render.h — CPU shading of the height field into an ARGB8888 buffer.
 *
 * Everything is driven by the surface geometry (slopes, curvature) in real
 * units, so a 1 mm ripple in a 30 cm tray and a 10 cm swell in a 12 m pool
 * look the way they should relative to each other.  Effects:
 *   - refraction of a floor pattern through the surface (paraxial Snell)
 *   - caustics from the Jacobian of the surface-to-floor ray map
 *   - Fresnel-weighted sky reflection and a soft sun glint
 *   - depth-dependent absorption (red first, then green)
 */
#ifndef POND_RENDER_H
#define POND_RENDER_H

#include "wave.h"
#include <stdint.h>

typedef struct {
    int view;          /* 0 = shaded water, 1 = height map */
    int floor_style;   /* 0 = tiles, 1 = checkerboard, 2 = sand */
    float gain;        /* multiplies the height field for display (1 = physical) */
    float ior;         /* 1.333 for water */
    float sun[3];      /* unit vector towards the sun */
} render_params;

void render_defaults(render_params *rp);
/* pix: nx*ny pixels, 0xAARRGGBB */
void render_frame(const wave *w, const render_params *rp, uint32_t *pix);

#endif
