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

/* app.h — the running program's state, and the setters that keep it
 * consistent.  main.c owns the window, input and the frame loop; param.c
 * owns the named parameters that keys, the config file, the command line
 * and (later) scripts all go through.  Nothing in here needs a window: an
 * app with win, v3 and au all NULL is a valid headless one, which is how
 * --write-config takes its snapshot. */
#ifndef POND_APP_H
#define POND_APP_H

#include "wave.h"
#include "render.h"
#include "view3d.h"
#include "hos.h"
#include "audio.h"

#include <SDL.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { const char *name; double L, depth; int floor; } preset;
extern const preset presets[];
#define NPRESETS 4

typedef struct app {
    SDL_Window *win;
    /* 2-D path */
    SDL_Renderer *ren;
    SDL_Texture *tex;
    render_params rp;
    uint32_t *pix;
    SDL_Rect dst;
    /* 3-D path */
    view3d *v3;
    view3d_params p3;
    int mode3d, show_help, show_hud, fullscreen;
    float cam_yaw, cam_pitch, cam_dist;   /* the camera when there is no view to keep it */

    wave *w;
    int nx, ny;                /* grid parameter (nx = ny = --grid) */
    int shape;                 /* WAVE_RECT or WAVE_DISK */
    hos *hs;                   /* nonlinear correction (rectangle only) */
    int hos_on, hos_nc, hos_order, hos_skipped;
    audio *au;                 /* NULL when there is no audio device or --no-audio */
    double volume; int mute;   /* shadowed, so they survive having no device */
    double knob[SND_NUM];
    double finger_t;           /* simulated time of the last finger plink */
    int frame_no;
    int running, paused, hud_dirty;
    int rain, breeze, paddle;
    double rain_rate;          /* drops per simulated second */
    double warp;               /* simulated seconds per real second */
    double acc;                /* simulated time not yet stepped */
    double paddle_div;         /* wavelengths across the basin: the frequency, kept basin-relative
                                  so a change of preset or size keeps the same picture */
    int    paddle_wall;        /* 0 x=0, 1 x=Lx, 2 y=0, 3 y=Ly */
    double paddle_pos;         /* 0..1 along that wall (disk: turns around the rim) */
    double paddle_span;        /* 0..1 of the wall; 1 = the whole wall */
    double paddle_gain, breeze_gain, finger_gain;
    int preset;

    int dragging, orbiting, mx, my;
    int touch_active; float tx, ty;   /* two-finger gesture (touch screens, browsers) */
    Uint64 prev;
    int shots, shot_pending;
    const char *snap_path;     /* --snap3d: save the last frame here */
    int frames_left;           /* --frames N: quit after N frames (0 = never) */
} app;

/* param.c: the setters with side effects, used by the parameter table and by main */
wave *app_make_wave(int shape, int grid, double Lx, double Ly, double depth);
int   app_set_shape(app *a, int shape);
void  app_set_preset(app *a, int p);
void  app_pool_changed(app *a);         /* after wave_set_pool: view, paddle band, HUD */
void  app_set_fullscreen(app *a, int on);
double app_paddle_k(const app *a);
double app_paddle_hz(const app *a);
void  app_set_paddle_hz(app *a, double f);
void  app_clamp_paddle(app *a);
void  app_reset_camera(app *a);

#endif
