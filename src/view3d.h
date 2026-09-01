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

/* view3d.h — OpenGL 3.3 core / WebGL2 view of the basin: water surface with
 * refraction to the floor and walls, CPU-computed caustic light map, glass
 * walls and bottom on demand, orbit camera, text overlay. */
#ifndef POND_VIEW3D_H
#define POND_VIEW3D_H

#include "wave.h"
#include <SDL.h>
#include <stdint.h>

typedef struct {
    float gain;        /* display multiplier on the height field */
    int floor_style;   /* 0 tiles, 1 checkerboard, 2 sand */
    int glass;         /* 0 opaque; 1 floor only (walls invisible, floor continues as a table);
                          2 glass walls; 3 glass walls and bottom; 4 no container at all */
    float sun[3];      /* unit vector towards the sun, y up */
    int cpu_caustics;  /* 1: force the CPU splat instead of the GPU pass */
} view3d_params;

typedef struct view3d view3d;

/* Call SDL_GL_SetAttribute through view3d_gl_attributes() before SDL_CreateWindow. */
void     view3d_gl_attributes(int msaa);
view3d  *view3d_create(SDL_Window *win, const wave *w, int cpu_caustics);   /* grid size and shape come from the wave */
void     view3d_destroy(view3d *v);

void     view3d_set_pool(view3d *v, const wave *w);    /* rebuild the basin geometry */
void     view3d_reset_camera(view3d *v, const wave *w);
void     view3d_orbit(view3d *v, float dyaw_deg, float dpitch_deg);
void     view3d_zoom(view3d *v, float factor);
void     view3d_set_camera(view3d *v, float yaw_deg, float pitch_deg, float dist_rel);

/* where a basin point is relative to the listener: pan -1..1 (left..right), att 0..1 (distance) */
void     view3d_listen(const view3d *v, double x, double z, double *pan, double *att);
/* window pixel -> basin metres on the mean surface; 0 if the ray misses the water */
int      view3d_pick(const view3d *v, int mx, int my, double *x, double *y);

void     view3d_set_overlay(view3d *v, const char *hud, const char *const *help, int nhelp,
                            int show_help, int show_hud);
void     view3d_render(view3d *v, const wave *w, const view3d_params *p);

/* screenshots: request before a render, collect after it (RGBA8, top row first; caller frees) */
void     view3d_request_capture(view3d *v);
uint8_t *view3d_take_capture(view3d *v, int *w, int *h);

#endif
