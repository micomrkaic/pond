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

/* Breaking (wave_break): a gentle wave is left exactly alone; a steep one loses
 * its excess and comes down to about the breaking slope; it foams where it broke,
 * and the foam fades once it stops; a raindrop's crater, steep but small, does not
 * count; the disk breaks too; and clear takes the foam away. */
#include "../src/wave.h"
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

/* the steepest the surface gets, sampled over a period, from a cell-centred gradient */
static double max_slope(wave *w)
{
    wave_realize(w);
    double s = 0.0;
    const int nx = w->nx, ny = w->ny;
    for (int j = 1; j < ny - 1; j++)
        for (int i = 1; i < nx - 1; i++) {
            const double ex = (w->eta[i + 1 + nx * j] - w->eta[i - 1 + nx * j]) / (2.0 * w->dx);
            const double ey = (w->eta[i + nx * (j + 1)] - w->eta[i + nx * (j - 1)]) / (2.0 * w->dy);
            const double q = sqrt(ex * ex + ey * ey);
            if (q > s) s = q;
        }
    return s;
}

/* run for `secs` at 60 frames a second, breaking at `slope` (0: off), as the program does */
static double run(wave *w, double secs, double slope, double *peak_slope)
{
    const double dt = 1.0 / 60.0, sub = 1.0 / 240.0;
    double peak = 0.0, frac = 0.0;
    const int frames = (int)floor(secs / dt + 0.5);
    for (int f = 0; f < frames; f++) {
        wave_step(w, sub, 4);
        wave_realize(w);
        frac += wave_break(w, slope, dt);
        if (peak_slope && f >= frames - 60) { const double s = max_slope(w); if (s > peak) peak = s; }
    }
    if (peak_slope) *peak_slope = peak;
    return frac / frames;
}

/* a standing wave across a 3 m pond, scaled to the given steepest slope */
static wave *standing(double slope)
{
    wave *w = wave_create(128, 128, 3.0, 3.0, 0.6);
    wave_set_mode(w, 12, 0, 1.0f, 0.0f);
    const double s = max_slope(w);
    wave_set_mode(w, 12, 0, (float)(slope / s), 0.0f);
    return w;
}

int main(void)
{
    /* --- a gentle wave: nothing happens, to the bit --- */
    {
        wave *a = standing(0.08), *b = standing(0.08);
        run(a, 5.0, 0.0, NULL);
        const double frac = run(b, 5.0, 0.15, NULL);
        check(frac == 0.0 && wave_norm(a) == wave_norm(b) && !b->foam_live,
              "a wave of slope 0.08 with breaking at 0.15: untouched, no foam");
        wave_destroy(a); wave_destroy(b);
    }

    /* --- a steep one: it breaks, loses its excess, and comes down to the breaking slope --- */
    {
        wave *a = standing(0.45), *b = standing(0.45);
        const double e0 = wave_norm(a);
        double peak_a, peak_b;
        run(a, 6.0, 0.0, &peak_a);
        const double frac = run(b, 6.0, 0.15, &peak_b);
        char what[200];
        snprintf(what, sizeof what, "slope 0.45, unbroken: %.0f%% of its energy left after 6 s, steepest %.2f",
                 100.0 * wave_norm(a) / e0, peak_a);
        check(wave_norm(a) > 0.6 * e0 && peak_a > 0.3, what);
        snprintf(what, sizeof what, "breaking at 0.15: %.0f%% left, steepest %.2f (%.1f%% of the surface broke, on average)",
                 100.0 * wave_norm(b) / e0, peak_b, 100.0 * frac);
        check(wave_norm(b) < 0.4 * wave_norm(a) && peak_b < 0.15 * 1.6 && frac > 0.0, what);
        check(b->foam_live, "and it foamed");

        /* breaking off: the foam fades, from its whitest to nothing (1e-3) in WAVE_FOAM_LIFE x ln(peak / 1e-3) */
        double fmax = 0.0;
        for (int i = 0; i < b->nx * b->ny; i++) if (b->foam[i] > fmax) fmax = b->foam[i];
        const double expect = WAVE_FOAM_LIFE * log(fmax / 1e-3);
        double t = 0.0;
        const double dt = 1.0 / 60.0;
        while (b->foam_live && t < 60.0) { wave_break(b, 0.0, dt); t += dt; }
        snprintf(what, sizeof what, "with breaking off the foam (at most %.2f) is gone in %.1f s (%.1f expected)", fmax, t, expect);
        check(!b->foam_live && fabs(t - expect) < 0.1, what);
        wave_destroy(a); wave_destroy(b);
    }

    /* --- the damping does not care how time is cut into frames (much) --- */
    {
        wave *a = standing(0.45), *b = standing(0.45);
        const double sub = 1.0 / 240.0;
        for (int f = 0; f < 240; f++) { wave_step(a, sub, 4); wave_realize(a); wave_break(a, 0.15, 1.0 / 60.0); }
        for (int f = 0; f < 480; f++) { wave_step(b, sub, 2); wave_realize(b); wave_break(b, 0.15, 1.0 / 120.0); }
        const double ra = wave_norm(a), rb = wave_norm(b);
        char what[160];
        snprintf(what, sizeof what, "4 s at 60 and at 120 frames a second: energies within %.1f%%", 100.0 * fabs(ra - rb) / ra);
        check(fabs(ra - rb) < 0.1 * ra, what);
        wave_destroy(a); wave_destroy(b);
    }

    /* --- raindrops, as the program makes them (a 2 cm crater, or 2.5 cells on a coarse
     *     grid, at up to 1.5 times that, -0.15 of its size deep): steep at the crater,
     *     but small; on the pond's fine grid and the browser's coarse one, no foam --- */
    for (int grid = 512; grid >= 256; grid /= 2) {
        wave *w = wave_create(grid, grid, 3.0, 3.0, 0.6);
        const double s = fmax(0.02, 2.5 * w->dx) * 1.5;
        double steep = 0.0, frac = 0.0;
        for (int k = 0; k < 20; k++) {
            wave_add_drop(w, 0.3 + 0.12 * k, 1.5 + 0.4 * sin(k), s, -0.15 * s);
            wave_step(w, 1.0 / 240.0, 0);
            wave_realize(w);
            const double q = max_slope(w);
            if (q > steep) steep = q;
            frac += wave_break(w, 0.15, 1.0 / 60.0);
            wave_step(w, 1.0 / 240.0, 4);
        }
        char what[160];
        snprintf(what, sizeof what, "twenty raindrops on a %d grid (craters as steep as %.2f): no breaking, no foam", grid, steep);
        check(frac == 0.0 && !w->foam_live, what);
        wave_destroy(w);
    }

    /* --- the disk breaks too, and clear takes the foam away --- */
    {
        wave *w = wave_create_disk(128, 64, 3.0, 0.6);
        /* a big drop in the middle of a small round pond: a steep ring */
        wave_add_drop(w, 1.5, 1.5, 0.12, 0.12);
        double frac = 0.0;
        int foamed = 0;
        const double e0 = (wave_step(w, 1.0 / 240.0, 0), wave_norm(w));
        for (int f = 0; f < 120; f++) { wave_step(w, 1.0 / 240.0, 4); wave_realize(w); frac += wave_break(w, 0.15, 1.0 / 60.0); foamed |= w->foam_live; }
        char what[160];
        snprintf(what, sizeof what, "the disk: a steep ring breaks (%.2f%% of the surface on average) and foams", 100.0 * frac / 120);
        check(frac > 0.0 && foamed && wave_norm(w) < e0, what);
        for (int f = 0; f < 10; f++) { wave_step(w, 1.0 / 240.0, 4); wave_realize(w); wave_break(w, 0.15, 1.0 / 60.0); }
        wave_add_drop(w, 1.5, 1.5, 0.12, 0.12);
        for (int f = 0; f < 10 && !w->foam_live; f++) { wave_step(w, 1.0 / 240.0, 4); wave_realize(w); wave_break(w, 0.15, 1.0 / 60.0); }
        wave_clear(w);
        int any = 0;
        for (int i = 0; i < w->nx * w->ny; i++) any |= w->foam[i] != 0.0f;
        check(!w->foam_live && !any && w->brk_frac == 0.0, "clear takes the foam away");
        wave_destroy(w);
    }

    printf(fails ? "%d breaking test(s) failed\n" : "all breaking tests passed\n", fails);
    return fails ? 1 : 0;
}
