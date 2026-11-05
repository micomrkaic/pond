#define _POSIX_C_SOURCE 200809L
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
#include "../src/record.h"
#include "../src/attract.h"
#include <sys/stat.h>
#include <unistd.h>
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
        app_jolt_step(a, dt);
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
        free_app(a);
    }

    /* --- boat X,Y,HDG places and starts the boat; float X,Y and float random add floats --- */
    {
        app *a = make_app(32);
        char err[256];
        a->sc = script_parse("at 0 boat 0.25,0.75,90\nat 0 float 0.5,0.5\nat 0 float random\nat 1 boat off\nat 1 floats 0\n", "t5", err, sizeof err);
        check(a->sc != NULL, "boat / float parse");
        script_start(a->sc, a);
        run(a, 0.1, 0.01);
        check(a->boat && fabs(a->boat_x - 0.25 * a->w->Lx) < 1e-9 && fabs(a->boat_y - 0.75 * a->w->Ly) < 1e-9
              && fabs(a->boat_hdg - M_PI / 2) < 1e-9, "boat 0.25,0.75,90 puts it there, heading north, and on");
        check(a->nfloat == 2 && fabs(a->fl_x[0] - 0.5 * a->w->Lx) < 1e-9, "two floats, the first at the middle");
        run(a, 1.0, 0.01);
        check(!a->boat && a->nfloat == 0, "boat off and floats 0, the parameters, take them away again");
        script_free(a->sc);
        a->sc = script_parse("at 0 jolt 0.1,0\nat 0 shake on  shake-amp 0.8  shake-freq 6\n", "t6", err, sizeof err);
        check(a->sc != NULL, "jolt / shake parse");
        script_start(a->sc, a);
        run(a, 0.5, 0.01);
        check(a->shake && fabs(a->shake_amp - 0.8) < 1e-9 && fabs(a->shake_freq - 6) < 1e-9, "shake on, 0.8 g at 6 Hz");
        check(wave_norm(a->w) > 0, "the jolt put the water in motion");
        script_free(a->sc); a->sc = NULL;

        check(script_parse("at 0 warpp 2\n", "e", err, sizeof err) == NULL && strstr(err, "warpp"), "an unknown word is an error with its name");
        check(script_parse("at 5 warp 2\nat 3 warp 1\n", "e", err, sizeof err) == NULL && strstr(err, "backwards"), "time may not run backwards");
        check(script_parse("at 0 warp\n", "e", err, sizeof err) == NULL, "a parameter with no value is an error");
        check(script_parse("# only a comment\n", "e", err, sizeof err) == NULL, "an empty script is an error");
        free_app(a);
    }

    /* --- every demo parses and runs through twice --- */
    {
        static const char *const demos[] = { "tour", "wavemaker", "rings", "storm", "dispersion", "boat", "wind", "shake", "day", "showcase", "house", "poolside" };
        for (int d = 0; d < 12; d++) {
            char path[128], err[256];
            snprintf(path, sizeof path, "demos/%s.pond", demos[d]);
            script *s = script_load(path, err, sizeof err);
            if (!s) { printf("%s: %s\n", path, err); fails++; continue; }
            app *a = make_app(64);
            a->sc = s;
            script_start(s, a);
            /* find the loop or the end: run until the clock has wrapped twice, or 1500 s */
            int loops = 0; double last = 0, secs = 0;
            for (secs = 0; secs < 1500 && loops < 2 && script_running(s); secs += 0.05) {
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

    /* --- a session recorded is a script that replays it --- */
    {
        const char *path = "build/rec-test.pond";
        app *a = make_app(64);
        param_set(a, "sun-elev", 66); param_set(a, "sun-azim", 320);
        a->rec = rec_start(a, path);
        check(a->rec != NULL, "a recording starts");
        const double dt = 0.05;
        double t = 0;
        #define TICK_TO(T) while (t < (T) - 1e-9) { rec_tick(a->rec, a, dt); t += dt; }
        TICK_TO(1.0);
        { double b = param_get(a, "rain"); param_set(a, "rain", 1); rec_param(a->rec, a, "rain", b); }
        /* a key held: the sun swept down, 21 presses of 3 degrees over a second */
        for (int i = 0; i < 21; i++) { TICK_TO(2.0 + 0.05 * i); double b = param_get(a, "sun-elev"); param_nudge(a, "sun-elev", -1); rec_param(a->rec, a, "sun-elev", b); }
        /* and round, 21 presses of 10 degrees: 320 to 170, through 360 */
        for (int i = 0; i < 21; i++) { TICK_TO(3.2 + 0.05 * i); double b = param_get(a, "sun-azim"); param_nudge(a, "sun-azim", +1); rec_param(a->rec, a, "sun-azim", b); }
        /* the mouse orbits: yaw on through 360 while the pitch comes down, then back a little */
        for (int i = 1; i <= 20; i++) { TICK_TO(4.5 + 0.05 * i); param_set(a, "yaw", fmod(35.0 + 300.0 * i / 20.0, 360.0)); param_set(a, "pitch", 42.0 - 22.0 * i / 20.0); }
        TICK_TO(6.5);
        for (int i = 1; i <= 10; i++) { TICK_TO(6.5 + 0.05 * i); param_set(a, "yaw", fmod(335.0 - 40.0 * i / 10.0 + 360.0, 360.0)); }
        TICK_TO(8.0);
        rec_drop(a->rec, a, 0.3 * a->w->Lx, 0.6 * a->w->Ly, 0.05 * a->w->Lx);
        /* a finger drawn corner to corner over a second */
        for (int i = 0; i <= 20; i++) { TICK_TO(8.5 + 0.05 * i); double u = i / 20.0; rec_finger(a->rec, a, (0.2 + 0.6 * u) * a->w->Lx, (0.2 + 0.6 * u) * a->w->Ly); }
        TICK_TO(10.0);
        rec_jolt(a->rec, 0.05, 0.0);
        app_add_float(a); rec_param(a->rec, a, "floats", 0);
        TICK_TO(11.0);
        #undef TICK_TO
        const double yaw_end = param_get(a, "yaw"), pitch_end = param_get(a, "pitch");
        rec_stop(a->rec, a, 1); a->rec = NULL;
        free_app(a);

        /* the file: one line per sweep, not one per press */
        FILE *f = fopen(path, "rb");
        check(f != NULL, "the recording is a file");
        int n_sun = 0, n_az = 0, n_finger = 0, n_lines = 0;
        char line[512];
        while (f && fgets(line, sizeof line, f)) {
            n_lines++;
            if (strstr(line, "sun-elev")) n_sun++;
            if (strstr(line, "sun-azim")) n_az++;
            if (strstr(line, "finger ")) n_finger++;
        }
        if (f) fclose(f);
        printf("   %d lines; sun-elev on %d (the snapshot and one sweep), sun-azim on %d, finger on %d", n_lines, n_sun, n_az, n_finger);
        check(n_sun == 2 && n_az == 2 && n_finger == 1, "");

        /* replayed on a fresh app, it lands where the session did */
        char err[256];
        script *s = script_load(path, err, sizeof err);
        if (!s) printf("%s\n", err);
        check(s != NULL, "the recording parses as a script");
        if (s) {
            app *b = make_app(64);
            b->sc = s;
            script_start(s, b);
            run(b, 2.5, 0.05);
            const double mid = param_get(b, "sun-elev");
            printf("   at 2.5 s the sun is at %.1f (sweeping from 66 to 3)", mid);
            check(mid < 60.0 && mid > 10.0, "");
            run(b, 8.3, 0.05);
            printf("   at 10.8 s: rain %s, sun %.1f deg at %.0f, camera yaw %.1f pitch %.1f (session: %.1f %.1f), %d float, water %.2g",
                   param_get(b, "rain") > 0.5 ? "on" : "off", param_get(b, "sun-elev"), param_get(b, "sun-azim"),
                   param_get(b, "yaw"), param_get(b, "pitch"), yaw_end, pitch_end, b->nfloat, wave_norm(b->w));
            check(param_get(b, "rain") > 0.5 && fabs(param_get(b, "sun-elev") - 3.0) < 0.5 && fabs(param_get(b, "sun-azim") - 170.0) < 1.0
                  && fabs(fmod(param_get(b, "yaw") - yaw_end + 540.0, 360.0) - 180.0) < 1.0 && fabs(param_get(b, "pitch") - pitch_end) < 0.5
                  && b->nfloat == 1 && wave_norm(b->w) > 0.0, "");
            /* and it loops */
            double last = script_time(s); int loops = 0;
            for (double u = 0; u < 15.0 && loops < 1; u += 0.05) { script_update(s, b, 0.05); if (script_time(s) < last) loops++; last = script_time(s); }
            check(loops == 1, "the recording loops");
            free_app(b);
        }
        if (getenv("KEEP")) printf("kept %s\n", path); else remove(path);
    }

    /* --- the attract mode: a playlist, played round, each show from a clean start --- */
    {
        char err[256];
        /* with no paths: the recordings in the current directory, or else the demos.
         * Asked from an empty directory of its own, so recordings lying in the source
         * tree do not count; then with one recording in it */
        {
            char root[1024], base[1100];
            if (getcwd(root, sizeof root)) {
                snprintf(base, sizeof base, "%s/", root);
                mkdir("build", 0755); mkdir("build/attract-empty", 0755);
                remove("build/attract-empty/pond-rec-001.pond");
                if (chdir("build/attract-empty") == 0) {
                    attract *d = attract_new(NULL, 0, base, err, sizeof err);
                    printf("   no paths, no recordings: %d shows%s%s", attract_count(d), d ? "" : " -- ", d ? "" : err);
                    check(attract_count(d) == 12, " (the demos)");
                    attract_free(d, NULL);
                    FILE *f = fopen("pond-rec-001.pond", "wb");
                    if (f) { fputs("at 0 rain on\nat 5 loop\n", f); fclose(f); }
                    d = attract_new(NULL, 0, base, err, sizeof err);
                    printf("   no paths, one recording: %d show", attract_count(d));
                    check(attract_count(d) == 1, " (the recording)");
                    attract_free(d, NULL);
                    remove("pond-rec-001.pond");
                    if (chdir(root) != 0) { printf("cannot return to %s\n", root); fails++; }
                }
                rmdir("build/attract-empty");
            }
        }

        mkdir("build", 0755); mkdir("build/attract-test", 0755);
        const char *names[3] = { "build/attract-test/a.pond", "build/attract-test/b.pond", "build/attract-test/c.pond" };
        const char *texts[3] = { "at 0 rain on  boat on  sun-elev 20\nat 3 loop\n",       /* loops at 3 s */
                                 "at 0 floats 2\nat 2 end\n",                             /* ends at 2 s */
                                 "at 0 say \"long\"\nat 5000 loop\n" };                  /* would run 83 min */
        for (int i = 0; i < 3; i++) { FILE *f = fopen(names[i], "wb"); if (f) { fputs(texts[i], f); fclose(f); } }
        const char *dir[1] = { "build/attract-test" };
        attract *at = attract_new(dir, 1, NULL, err, sizeof err);
        check(at && attract_count(at) == 3, "a directory of three shows");
        if (at) {
            app *a = make_app(32);
            const double sun0 = param_get(a, "sun-elev");
            attract_begin(at, a);
            check(attract_fade(at) > 0.99f, "each show fades in from black");
            int clean = 1, seen[3] = { 0, 0, 0 }, firsts_ok = 1;
            double t = 0, longest = 0, since = 0;
            const char *cur = attract_current(at);
            int played = 0;
            for (t = 0; t < 6000 && attract_shows_played(at) < 7; t += 0.05) {
                attract_update(at, a, 0.05);
                since += 0.05;
                if (attract_current(at) != cur || attract_shows_played(at) != played) {
                    /* a new show began: nothing of the last one carried over */
                    if (since > longest) longest = since;
                    since = 0;
                    const char *now = attract_current(at);
                    if (strstr(now, "c.pond") || strstr(now, "b.pond"))
                        clean &= param_get(a, "rain") < 0.5 && param_get(a, "boat") < 0.5 && fabs(param_get(a, "sun-elev") - sun0) < 1e-9;
                    if (strstr(now, "a.pond") || strstr(now, "c.pond")) clean &= a->nfloat == 0;
                    /* a new round is a new order, but never the same show twice running */
                    if (now == cur) firsts_ok = 0;
                    if (getenv("ATTRACT_ORDER")) printf("   [%d] %s\n", attract_shows_played(at), now);
                    cur = now; played = attract_shows_played(at);
                }
                for (int i = 0; i < 3; i++) if (strstr(attract_current(at), names[i] + 19)) seen[i] = 1;
            }
            printf("   7 shows in %.0f s; the longest ran %.0f s (cap %.0f)", t, longest, ATTRACT_MAX);
            check(attract_shows_played(at) == 7 && longest <= ATTRACT_MAX + 0.1 && longest > ATTRACT_MAX - 1.0, "");
            check(seen[0] && seen[1] && seen[2], "every show comes round");
            check(clean, "each starts from the settings the attract mode began with");
            check(firsts_ok, "no show twice running, across the reshuffles too");
            check(attract_fade(at) < 0.01f || since < ATTRACT_FADE, "the fade is over after a second and a half");
            attract_free(at, a);
            check(a->sc == NULL, "and leaving stops the show");
            free_app(a);
        }
        for (int i = 0; i < 3; i++) remove(names[i]);
        rmdir("build/attract-test");
    }

    /* --- the ear's drop: the rain's spread rings at 1.2 to 3.6 kHz on every basin --- */
    {
        for (int pr = 0; pr < 4; pr++) {
            app *a = make_app(512);
            param_set(a, "preset", pr);
            const double s0 = app_drop_size(a);
            const double lo = app_acoustic_size(a, 0.5 * s0), hi = app_acoustic_size(a, 1.5 * s0);
            /* Minnaert, as audio_splash takes it: R = 0.3 size, f = 3.26 / R */
            const double f_hi = 3.26 / (0.3 * lo), f_lo = 3.26 / (0.3 * hi);
            char what[160];
            snprintf(what, sizeof what, "%-5s rain plinks at %.0f..%.0f Hz (inside 1000..4000)", presets[pr].name, f_lo, f_hi);
            check(f_lo > 1000.0 && f_hi < 4000.0, what);
            free_app(a);
        }
    }

    printf(fails ? "%d script test(s) failed\n" : "all script tests passed\n", fails);
    return fails ? 1 : 0;
}
