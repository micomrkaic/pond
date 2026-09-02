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

/* config.h — the settings that have to be known before the program exists
 * (the grid, the window, which renderer, whether to open a sound device, and
 * the basin it starts with), plus a list of everything else the config file
 * and command line asked for, to be handed to the parameter table once there
 * is a running program to apply them to.
 *
 * Three passes: built-in defaults, then the config file, then the command
 * line.  --write-config writes the lot back: these creation settings, then
 * every parameter's current value from a headless app, so the file is always
 * a complete, commented picture of what the program does. */
#ifndef POND_CONFIG_H
#define POND_CONFIG_H

#include <stddef.h>

struct app;                    /* the live program, see app.h */

#define CONFIG_MAX_LATE 160

typedef struct {
    /* before the program exists */
    int    grid;
    int    winw, winh;
    int    mode3d;             /* 0: the --2d CPU renderer */
    int    msaa, cpu_caustics;
    int    no_audio;
    int    preset;             /* 0..3 */
    int    shape;              /* WAVE_RECT / WAVE_DISK */
    double Lx, Ly, depth;      /* metres; 0 = the preset's */
    int    hos_nc, hos_order;

    /* everything else: parameter settings, applied in order once it does */
    struct { char name[32]; char val[64]; } late[CONFIG_MAX_LATE];
    int    nlate;
} pond_config;

void config_defaults(pond_config *c);

/* The file pond reads without being asked, in order: $POND_CONFIG,
 * $XDG_CONFIG_HOME/pond/pond.conf, ~/.config/pond/pond.conf, ~/.pondrc,
 * ./pond.conf.  for_writing = 0 returns the first that exists (NULL if none);
 * for_writing = 1 returns where --write-config would put one. */
const char *config_path(int for_writing);

int  config_load(pond_config *c, const char *path);         /* 0 ok, -1 no such file */
/* 0 ok, -1 unknown key.  Creation keys land in the struct; parameter names are queued. */
int  config_set(pond_config *c, const char *key, const char *value);
/* Push the queued settings through the parameter table.  Returns how many were refused. */
int  config_apply(const pond_config *c, struct app *a);
/* Creation settings from c, then every parameter's value from a.  0 ok, -1 could not write. */
int  config_write(const pond_config *c, const struct app *a, const char *path);

#endif
