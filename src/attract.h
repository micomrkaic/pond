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

/* attract.h — the attract mode: shows played one after another, as a screen saver.
 *
 * A playlist of scripts (recordings, demos, anything --script takes), shuffled;
 * each plays once through -- to its loop or its end, or for at most ATTRACT_MAX
 * seconds -- and the next fades in from black.  Before each, every parameter is put
 * back as it stood when the attract mode began, so no show inherits the last one's
 * boat or rain.  Nothing here touches the window: main.c goes full screen, hides
 * the HUD and the cursor, draws the fade, and leaves on any key. */
#ifndef POND_ATTRACT_H
#define POND_ATTRACT_H

#include "app.h"
#include <stddef.h>

#define ATTRACT_MAX  720.0     /* seconds: a show that never loops gives way after this */
#define ATTRACT_FADE 1.5       /* seconds of fade-in from black */

typedef struct attract attract;

/* The playlist: each path a .pond file or a directory of them.  With none: the
 * recordings (pond-rec-*.pond) in the current directory, or failing those the
 * demos, looked for in base/demos, base/../share/pond/demos and ./demos (base:
 * the program's own directory, or NULL).  NULL, with err filled, if nothing. */
attract    *attract_new(const char *const *paths, int npaths, const char *base, char *err, size_t errn);
int         attract_count(const attract *at);
const char *attract_file(const attract *at, int i);     /* in play order */

void        attract_begin(attract *at, app *a);         /* remember the settings; start the first show */
void        attract_update(attract *at, app *a, double dt);   /* runs the show (a->sc); moves on when it is done */
float       attract_fade(const attract *at);            /* 0 .. 1: how black the picture is now */
const char *attract_current(const attract *at);
int         attract_shows_played(const attract *at);
void        attract_free(attract *at, app *a);          /* stops the show; a->sc is cleared */

#endif
