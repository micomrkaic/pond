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

/* The breaking hiss, listened to: SDL's dummy audio driver runs the callback in
 * real time with no device, and POND_WAV writes down what it played.  Silence,
 * then a breaker off to the right (it swells in, sits in its band, and is heard
 * on the right), then it stops (and trails off rather than cutting), then a point
 * in the middle (left and right the same) and a breaking spread all round (left
 * and right independent). */
#define _POSIX_C_SOURCE 200809L
#include "../src/audio.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", what, ok ? " ok" : " FAILED");
    if (!ok) fails++;
}

/* hold a setting for `secs`, refreshing it each 16 ms frame as the program does */
static void hold(audio *a, double secs, double level, double pan, double width)
{
    const Uint32 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < (Uint32)(secs * 1000.0)) {
        audio_set_break(a, level, pan, 1.0, width, 0.016);
        SDL_Delay(16);
    }
}

typedef struct { double l, r, lr, lo; long n; } stats;

/* rms of left and right, their correlation, and the share below ~200 Hz, over [t0, t1) seconds */
static stats measure(const float *s, long frames, double rate, double t0, double t1)
{
    stats st = { 0, 0, 0, 0, 0 };
    long a = (long)(t0 * rate), b = (long)(t1 * rate);
    if (b > frames) b = frames;
    double ll = 0, rr = 0, lr = 0, lo2 = 0, lp = 0;
    const double c = 1.0 - exp(-2.0 * PI * 200.0 / rate);
    for (long i = a; i < b; i++) {
        const double l = s[2 * i], r = s[2 * i + 1];
        ll += l * l; rr += r * r; lr += l * r;
        lp += c * (0.5 * (l + r) - lp); lo2 += lp * lp;
        st.n++;
    }
    if (st.n == 0) return st;
    st.l = sqrt(ll / st.n); st.r = sqrt(rr / st.n);
    st.lr = (ll > 0 && rr > 0) ? lr / sqrt(ll * rr) : 0.0;
    const double mid = 0.25 * (ll + rr + 2 * lr) / st.n;
    st.lo = mid > 0 ? sqrt(lo2 / st.n / mid) : 0.0;
    return st;
}

int main(void)
{
    const char *path = "build/test-break.wav";
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("POND_WAV", path, 1);
    audio *a = audio_open();
    if (!a) { printf("no audio (SDL's dummy driver): SKIPPED\nall audio tests passed (skipped)\n"); return 0; }
    audio_set_volume(a, 1.0);

    /* the phases, in seconds of wall clock (the dummy driver keeps real time) */
    hold(a, 0.5, 0.0, 0.0, 0.0);           /* 0.0 .. 0.5  nothing */
    hold(a, 1.5, 1.0, 0.8, 0.0);           /* 0.5 .. 2.0  a breaker, to the right */
    hold(a, 1.5, 0.0, 0.8, 0.0);           /* 2.0 .. 3.5  it stops */
    hold(a, 1.0, 1.0, 0.0, 0.0);           /* 3.5 .. 4.5  a point, in the middle */
    hold(a, 1.0, 1.0, 0.0, 1.0);           /* 4.5 .. 5.5  spread all round */
    audio_close(a);

    /* the recording: a 44-byte float WAV header, then stereo floats */
    FILE *f = fopen(path, "rb");
    if (!f) { check(0, "the recording is there"); return 1; }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f) - 44;
    fseek(f, 24, SEEK_SET);
    uint32_t rate = 0;
    if (fread(&rate, 4, 1, f) != 1) rate = 44100;
    fseek(f, 44, SEEK_SET);
    const long frames = bytes / 8;
    float *s = malloc((size_t)bytes);
    if (!s || fread(s, 1, (size_t)bytes, f) != (size_t)bytes) { check(0, "the recording reads"); fclose(f); return 1; }
    fclose(f);
    remove(path);
    printf("   %.2f s recorded at %u Hz\n", frames / (double)rate, rate);

    const double R = rate;
    stats quiet = measure(s, frames, R, 0.1, 0.4);
    stats on    = measure(s, frames, R, 0.9, 1.9);
    stats tail0 = measure(s, frames, R, 2.05, 2.25);
    stats tail1 = measure(s, frames, R, 3.1, 3.4);
    stats mid   = measure(s, frames, R, 3.9, 4.4);
    stats wide  = measure(s, frames, R, 4.9, 5.4);
    char what[200];

    snprintf(what, sizeof what, "silent before (rms %.1e)", 0.5 * (quiet.l + quiet.r));
    check(quiet.l < 1e-4 && quiet.r < 1e-4, what);
    snprintf(what, sizeof what, "a breaker is heard (rms %.3f), on the right (%.1f times the left)", on.r, on.r / (on.l + 1e-12));
    check(on.r > 0.01 && on.r > 2.5 * on.l, what);
    snprintf(what, sizeof what, "a hiss, not a rumble: %.0f%% of it below 200 Hz", 100.0 * on.lo);
    check(on.lo < 0.25, what);
    const double e_on = hypot(on.l, on.r), e0 = hypot(tail0.l, tail0.r), e1 = hypot(tail1.l, tail1.r);
    snprintf(what, sizeof what, "when it stops it trails off: %.0f%% just after, %.0f%% a second later", 100 * e0 / e_on, 100 * e1 / e_on);
    check(e0 > 0.5 * e_on && e1 < 0.25 * e_on, what);
    snprintf(what, sizeof what, "a point in the middle: left and right alike (correlation %.2f, balance %.2f)", mid.lr, mid.r / (mid.l + 1e-12));
    check(mid.lr > 0.85 && fabs(mid.r / mid.l - 1.0) < 0.15, what);
    snprintf(what, sizeof what, "spread all round: left and right independent (correlation %.2f)", wide.lr);
    check(fabs(wide.lr) < 0.3 && wide.l > 0.01 && wide.r > 0.01, what);

    free(s);
    printf(fails ? "%d audio test(s) failed\n" : "all audio tests passed\n", fails);
    return fails ? 1 : 0;
}
