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

/* script.h — a timed sequence of parameter settings: the config file with a
 * time axis.  One event per line, its actions after the time:
 *
 *   at 0     preset 2  paddle on  paddle-freq 1.2  paddle-span 0.15
 *   at 5     yaw += 360 over 40        # a full turn, taking forty seconds
 *   +10      paddle-pos 0.8 over 15    # ten seconds after the line before
 *   at 30    rain on  rain-rate 0.5
 *   at 45    drop 0.3,0.6              # x, y as fractions of the basin
 *            say "the ring maker"      # no time: same moment as the line before
 *   at 90    shape disk  paddle-span 1
 *   at 120   loop
 *
 * Every parameter name is a verb (see --list-params); a value may be
 * absolute, or relative with += and -=; a real may take "over T" to tween
 * there, eased, over T seconds.  The other verbs: drop X,Y [SIZE] or drop
 * random, clear, camera Y,P,D [over T], say "text", loop, end.  # starts a
 * comment; ; separates actions like whitespace does.  Times are wall-clock
 * seconds from the start, so a script can change the time warp itself.
 *
 * A key press on a parameter the script is tweening cancels that tween: the
 * person wins.  Everything else the script does carries on. */
#ifndef POND_SCRIPT_H
#define POND_SCRIPT_H

#include "app.h"
#include <stddef.h>

typedef struct script script;

/* err receives a message on failure (with the line number); NULL on success too */
script *script_load(const char *path, char *err, size_t errn);
script *script_parse(const char *text, const char *name, char *err, size_t errn);
void    script_free(script *s);

void    script_start(script *s, app *a);              /* rewind to time 0 and begin */
void    script_update(script *s, app *a, double dt);  /* once a frame, wall-clock dt */
void    script_stop(script *s);
int     script_running(const script *s);
const char *script_name(const script *s);
double  script_time(const script *s);

/* param.c calls this on a set that is not the script's own */
void    script_user_set(script *s, const char *name);

#endif
