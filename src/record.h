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

/* record.h — a session written down as a script, to be played back as a show.
 *
 * What the person does at the keyboard and the mouse is turned into the lines
 * a script is made of, against the same parameter table, so a recording is an
 * ordinary .pond file: it plays with --script, it loops, and it can be edited.
 * It opens with a snapshot of every parameter, so it replays the same from any
 * start.  Key presses become settings; a key held down, sweeping a value,
 * becomes one "over T" line; clicks become drops; a finger dragged through the
 * water becomes a "finger" path; the camera, however it was moved, becomes
 * "camera" keyframes wherever it paused.  Rain and the breeze are random, so a
 * replay is the same show, not the same frames. */
#ifndef POND_RECORD_H
#define POND_RECORD_H

#include "app.h"
#include <stdio.h>

typedef struct recorder recorder;

/* begin: the snapshot is written at once.  path NULL: a fresh pond-rec-NNN.pond
 * in the current directory; "-": standard output.  NULL if the file cannot be made */
recorder *rec_start(app *a, const char *path);
/* finish: what is pending is written, then "loop" (or "end"), and the file closed */
void      rec_stop(recorder *r, app *a, int loop);
/* once a frame, after the camera and the sources have moved: the clock, the
 * camera's keyframes, sweeps that have ended, a finger that has lifted */
void      rec_tick(recorder *r, app *a, double dt);

/* the person did something: a parameter changed (its value before, and the app
 * now holds the value after), a
 * drop at x, y metres of size s metres, a clearing, a jolt, the finger at x, y
 * this frame */
void      rec_param(recorder *r, app *a, const char *name, double before);
void      rec_drop(recorder *r, app *a, double x, double y, double s);
void      rec_clear(recorder *r);
void      rec_jolt(recorder *r, double dux, double duy);
void      rec_finger(recorder *r, app *a, double x, double y);

const char *rec_path(const recorder *r);
double      rec_time(const recorder *r);

#endif
