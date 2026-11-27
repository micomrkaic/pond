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

/* The breaking hiss, listened to: an offline audio (no device) renders exactly
 * what the device's callback would, frame by frame, while the setting is refreshed
 * each 16 ms frame as the program does.  Silence, then a breaker off to the right
 * (it swells in, sits in its band, and is heard on the right), then it stops (and
 * trails off rather than cutting), then a point in the middle (left and right the
 * same) and a breaking spread all round (left and right independent). */
#include "../src/audio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define RATE 44100

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", what, ok ? " ok" : " FAILED");
    if (!ok) fails++;
}

static float *buf;
static long nbuf;

/* hold a setting for `secs`: 16 ms frames, each set then rendered, as the program does */
static void hold(audio *a, double secs, double level, double pan, double width)
{
    const int frame = (int)(0.016 * RATE);
    const long end = nbuf + (long)(secs * RATE);
    while (nbuf < end) {
        const int n = (int)(end - nbuf < frame ? end - nbuf : frame);
        audio_set_break(a, level, pan, 1.0, width, n / (double)RATE);
        audio_render(a, buf + 2 * nbuf, n);
        nbuf += n;
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
    audio *a = audio_open_offline(RATE);
    buf = calloc((size_t)(6 * RATE) * 2, sizeof(float));
    if (!a || !buf) { check(0, "an offline audio"); return 1; }
    audio_set_volume(a, 1.0);

    hold(a, 0.5, 0.0, 0.0, 0.0);           /* 0.0 .. 0.5  nothing */
    hold(a, 1.5, 1.0, 0.8, 0.0);           /* 0.5 .. 2.0  a breaker, to the right */
    hold(a, 1.5, 0.0, 0.8, 0.0);           /* 2.0 .. 3.5  it stops */
    hold(a, 1.0, 1.0, 0.0, 0.0);           /* 3.5 .. 4.5  a point, in the middle */
    hold(a, 1.0, 1.0, 0.0, 1.0);           /* 4.5 .. 5.5  spread all round */
    audio_close(a);

    const long frames = nbuf;
    const float *s = buf;
    const double R = RATE;
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

    free(buf);
    printf(fails ? "%d audio test(s) failed\n" : "all audio tests passed\n", fails);
    return fails ? 1 : 0;
}
