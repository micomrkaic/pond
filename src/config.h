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

/* config.h — every startup setting in one struct, filled in three passes:
 * built-in defaults, then the config file, then the command line.  The same
 * struct is written back out by --write-config, so the file is always a
 * complete, commented picture of what the program is doing.
 */
#ifndef POND_CONFIG_H
#define POND_CONFIG_H

#include "audio.h"      /* SND_NUM */

typedef struct {
    /* window and view */
    int    grid;
    int    winw, winh;
    int    fullscreen;
    int    mode3d;             /* 0: the --2d CPU renderer */
    int    glass;              /* 0..4, see view3d.h */
    int    floor_style;        /* -1: whatever the preset asks for */
    int    msaa, cpu_caustics;
    int    show_hud, show_help;
    int    cam_set;
    float  cam_yaw, cam_pitch, cam_dist;

    /* basin */
    int    preset;             /* 0..3 */
    int    shape;              /* WAVE_RECT / WAVE_DISK */
    double Lx, Ly, depth;      /* metres; 0 = take the preset's */
    int    hos_on, hos_nc, hos_order;

    /* sources */
    int    rain, breeze, paddle;
    double rain_rate, warp;
    double paddle_freq;        /* Hz; 0 = the old default of 8 wavelengths across the basin */
    double paddle_pos, paddle_span, paddle_stroke;
    int    paddle_wall;

    /* sound */
    int    no_audio;           /* do not open a device at all */
    int    mute;               /* device open, starts muted (m unmutes) */
    double volume;
    double knob[SND_NUM];
} pond_config;

void config_defaults(pond_config *c);

/* The file pond reads without being asked, in order: $POND_CONFIG,
 * $XDG_CONFIG_HOME/pond/pond.conf, ~/.config/pond/pond.conf, ~/.pondrc,
 * ./pond.conf.  for_writing = 0 returns the first that exists (NULL if none);
 * for_writing = 1 returns where --write-config would put one. */
const char *config_path(int for_writing);

int  config_load(pond_config *c, const char *path);        /* 0 ok, -1 no such file */
int  config_set(pond_config *c, const char *key, const char *value);   /* 0 ok, -1 unknown key */
int  config_write(const pond_config *c, const char *path); /* 0 ok, -1 could not write */

#endif
