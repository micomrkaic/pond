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

/* param.h — every setting that can change while the program runs, by name.
 *
 * A parameter is a number: booleans are 0/1, enumerations are an index with
 * names, reals and integers are themselves.  Each has a getter and a setter
 * on the live app (the setter carries whatever side effects keep the program
 * consistent), a range, and a nudge -- the step a key press takes, either a
 * factor or an increment.  Keys nudge, the config file and command line set,
 * scripts will tween.  All of them land in the same place. */
#ifndef POND_PARAM_H
#define POND_PARAM_H

#include "app.h"
#include <stdio.h>

typedef enum { PK_BOOL, PK_INT, PK_REAL, PK_ENUM } param_kind;

typedef struct {
    const char *name;
    param_kind kind;
    const char *group;          /* basin, sources, wavemaker, display, camera, sound */
    const char *help;           /* one line, for --list-params and the config file */
    double lo, hi;              /* the range a set is clamped to (enum: 0 .. n-1) */
    int    wrap;                /* 0 clamp at the ends; 1 wrap round; 2 the setter decides */
    double step;                /* a nudge: v * step (mult) or v + step */
    int    mult;
    double snap;                /* mult only: a nudge up from below snap lands on it, a
                                   nudge down from below it lands on 0 (levels that may be off) */
    const char *const *names;   /* enum: the names, NULL-terminated */
    double (*get)(const app *a);
    void   (*set)(app *a, double v);
} param;

int          param_count(void);
const param *param_at(int i);
const param *param_find(const char *name);   /* case-insensitive; '_' and '-' are the same */

double param_get(const app *a, const char *name);                  /* 0 for unknown */
int    param_get_str(const app *a, const char *name, char *buf, size_t n);   /* formatted for humans and files */
int    param_set(app *a, const char *name, double v);              /* 0 ok, -1 unknown */
int    param_set_str(app *a, const char *name, const char *val);   /* 0 ok, -1 unknown, -2 bad value */
int    param_nudge(app *a, const char *name, int dir);             /* +1 up, -1 down, 0 toggle / cycle */

/* 1 if the value is the one the preset (or the basin) implies -- width, length,
 * depth, floor, the default paddle frequency -- so a written file can leave it
 * commented out and keep following the preset when that is edited */
int    param_is_derived(const app *a, const char *name);

/* --list-params: name, kind, range, value and help, one per line */
void   param_list(const app *a, FILE *f);

#endif
