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

/* The parameter table and the script runner, on a headless app: no window,
 * no view, no sound device.  Tweens land where they should and when; relative
 * values, wrap-around, loops and "the person wins" all behave; and every demo
 * in demos/ parses and runs through twice without complaint. */
#include "../src/app.h"
#include "../src/param.h"
#include "../src/script.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", what, ok ? " ok" : " FAILED");
    if (!ok) fails++;
}

static app *make_app(int grid)
{
    app *a = calloc(1, sizeof *a);
    a->nx = a->ny = grid;
    a->running = 1;
    a->warp = 1.0; a->rain_rate = 2.0;
    a->paddle_div = 8.0; a->paddle_pos = 0.5; a->paddle_span = 1.0;
    a->paddle_gain = a->breeze_gain = a->finger_gain = 1.0;
    a->volume = 0.7;
    a->knob[SND_DROPS] = 1; a->knob[SND_BED] = 1; a->knob[SND_BREEZE] = 1; a->knob[SND_HARSH] = 0.15;
    a->show_hud = 1;
    render_defaults(&a->rp);
    a->p3.gain = 1.0f;
    a->pix = malloc((size_t)grid * grid * sizeof(uint32_t));
    a->shape = WAVE_RECT;
    a->preset = 1;
    a->w = app_make_wave(WAVE_RECT, grid, presets[1].L, presets[1].L, presets[1].depth);
    a->rp.floor_style = a->p3.floor_style = presets[1].floor;
    app_reset_camera(a);
    return a;
}

static void free_app(app *a)
{
    if (a->sc) script_free(a->sc);
    if (a->hs) hos_destroy(a->hs);
    wave_destroy(a->w); free(a->pix); free(a);
}

/* run a script for `secs` of wall clock in steps of dt, stepping the water too */
static void run(app *a, double secs, double dt)
{
    for (double t = 0; t < secs; t += dt) {
        script_update(a->sc, a, dt);
        wave_step(a->w, 1.0 / 240.0, 1);
    }
}

int main(void)
{
    /* --- the table itself --- */
    {
        app *a = make_app(32);
        param_set_str(a, "glass", "glass+bottom");
        check(param_get(a, "glass") == 3, "enum by name: glass = glass+bottom is 3");
        param_nudge(a, "glass", +1); param_nudge(a, "glass", +1);
        check(param_get(a, "glass") == 0, "enum cycles round through all five: 3, 4, 0");
        param_set_str(a, "preset", "3");
        check(a->preset == 2 && fabs(a->w->Lx - 12.0) < 1e-9, "preset counts from 1: preset 3 is the 12 m pool");
        param_set(a, "yaw", 400);
        check(fabs(param_get(a, "yaw") - 40) < 1e-6, "yaw wraps: 400 is 40");
        param_set(a, "paddle-span", 5);
        check(param_get(a, "paddle-span") == 1.0, "a real is clamped to its range");
        param_set(a, "sound.brown", 0);
        param_nudge(a, "sound.brown", +1);
        check(fabs(param_get(a, "sound.brown") - 0.05) < 1e-9, "a level nudged up from zero snaps to its floor");
        param_nudge(a, "sound.brown", -1);
        check(param_get(a, "sound.brown") == 0, "and back down to nothing");
        char buf[32];
        param_get_str(a, "paddle-wall", buf, sizeof buf);
        check(!strcmp(buf, "x=0"), "enum formats as its name");
        check(param_set_str(a, "no-such-thing", "1") == -1, "an unknown name is refused");
        check(param_set_str(a, "warp", "fast") == -2, "and so is a value that is not a number");
        free_app(a);
    }

    /* --- a tween lands on time, eased --- */
    {
        app *a = make_app(32);
        char err[256];
        a->sc = script_parse("at 0 warp 1\nat 1 warp 4 over 2\n", "t1", err, sizeof err);
        check(a->sc != NULL, "a two-line script parses");
        script_start(a->sc, a);
        run(a, 2.0, 0.01);        /* t = 2: halfway through the tween */
        const double mid = param_get(a, "warp");
        run(a, 1.5, 0.01);        /* t = 3.5: past the end */
        const double end = param_get(a, "warp");
        printf("halfway through 1 -> 4 the warp is %.3f (smoothstep says 2.5), at the end %.3f ", mid, end);
        check(fabs(mid - 2.5) < 0.05 && fabs(end - 4.0) < 1e-9, "");
        check(!script_running(a->sc), "a script with no loop stops when it runs out");
        free_app(a);
    }

    /* --- relative values, a full turn, and a loop --- */
    {
        app *a = make_app(32);
        char err[256];
        a->sc = script_parse("at 0  yaw 30\nat 0  yaw += 360 over 4\nat 5  paddle-pos 0.5\nat 5 paddle-span -= 0.5\nat 6  loop\n", "t2", err, sizeof err);
        check(a->sc != NULL, "relative values parse");
        script_start(a->sc, a);
        run(a, 2.0, 0.01);
        const double y2 = param_get(a, "yaw");
        run(a, 2.5, 0.01);
        const double y4 = param_get(a, "yaw");
        printf("yaw 30 += 360 over 4 s: at 2 s it is %.1f (past 180), at the end %.1f (back to 30) ", y2, y4);
        check(y2 > 150 && y2 < 270 && fabs(y4 - 30) < 1e-3, "");
        run(a, 1.0, 0.01);        /* t = 5.5: the -= line has fired */
        check(fabs(param_get(a, "paddle-span") - 0.5) < 1e-9, "paddle-span -= 0.5 from 1 is 0.5");
        run(a, 1.0, 0.01);        /* t = 6.5: looped, the at-0 lines ran again, the tween restarted */
        check(script_running(a->sc) && script_time(a->sc) < 1.0, "loop rewinds the clock");
        free_app(a);
    }

    /* --- the person wins: a key on a tweened parameter cancels the tween --- */
    {
        app *a = make_app(32);
        char err[256];
        a->sc = script_parse("at 0 rain-rate 1\nat 0 rain-rate 10 over 4\nat 10 rain on\n", "t3", err, sizeof err);
        script_start(a->sc, a);
        run(a, 1.0, 0.01);
        param_set(a, "rain-rate", 3.0);             /* as a key press would */
        run(a, 4.0, 0.01);
        printf("rain-rate set by hand mid-tween stays at %.3g, not 10 ", param_get(a, "rain-rate"));
        check(fabs(param_get(a, "rain-rate") - 3.0) < 1e-9, "");
        check(script_running(a->sc), "and the script itself carries on");
        free_app(a);
    }

    /* --- drops, clear, camera, say, and errors --- */
    {
        app *a = make_app(32);
        char err[256];
        a->sc = script_parse("at 0 clear\nat 0.5 drop 0.5,0.5 0.05\nat 1 say \"a drop\"\nat 2 camera 10,20,0.5 over 1\n", "t4", err, sizeof err);
        check(a->sc != NULL, "drop / say / camera parse");
        script_start(a->sc, a);
        run(a, 0.7, 0.01);
        check(wave_norm(a->w) > 0, "a drop puts water in");
        run(a, 0.5, 0.01);
        check(!strcmp(a->caption, "a drop"), "say sets the caption");
        run(a, 2.5, 0.01);
        check(fabs(param_get(a, "yaw") - 10) < 1e-3 && fabs(param_get(a, "dist") - 0.5) < 1e-3, "camera tweens all three");
        script_free(a->sc); a->sc = NULL;

        check(script_parse("at 0 warpp 2\n", "e", err, sizeof err) == NULL && strstr(err, "warpp"), "an unknown word is an error with its name");
        check(script_parse("at 5 warp 2\nat 3 warp 1\n", "e", err, sizeof err) == NULL && strstr(err, "backwards"), "time may not run backwards");
        check(script_parse("at 0 warp\n", "e", err, sizeof err) == NULL, "a parameter with no value is an error");
        check(script_parse("# only a comment\n", "e", err, sizeof err) == NULL, "an empty script is an error");
        free_app(a);
    }

    /* --- every demo parses and runs through twice --- */
    {
        static const char *const demos[] = { "tour", "wavemaker", "rings", "storm", "dispersion" };
        for (int d = 0; d < 5; d++) {
            char path[128], err[256];
            snprintf(path, sizeof path, "demos/%s.pond", demos[d]);
            script *s = script_load(path, err, sizeof err);
            if (!s) { printf("%s: %s\n", path, err); fails++; continue; }
            app *a = make_app(64);
            a->sc = s;
            script_start(s, a);
            /* find the loop or the end: run until the clock has wrapped twice, or 1000 s */
            int loops = 0; double last = 0, secs = 0;
            for (secs = 0; secs < 1000 && loops < 2 && script_running(s); secs += 0.05) {
                script_update(s, a, 0.05);
                wave_step(a->w, 1.0 / 240.0, 1);
                if (script_time(s) < last) loops++;
                last = script_time(s);
            }
            printf("%-16s ran %.0f s headless, looped %d times, ends on %s %.3g x %.3g m ", path, secs, loops,
                   a->shape == WAVE_DISK ? "a disk" : "a rectangle", a->w->Lx, a->w->Ly);
            check(loops == 2, "");
            free_app(a);
        }
    }

    printf(fails ? "%d script test(s) failed\n" : "all script tests passed\n", fails);
    return fails ? 1 : 0;
}
