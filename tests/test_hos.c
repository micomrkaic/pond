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

/* HOS: the correction is quadratic in amplitude for small waves, a steep
 * standing wave grows a bound second harmonic of the Stokes size, and the
 * split scheme stays bounded over many periods. */
#include "../src/hos.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double coef_amp(const wave *w, int m, int n)   /* physical amplitude of a cos-cos mode */
{
    double A = w->A[m + w->nx * n], B = w->B[m + w->nx * n];
    double s = 4.0 / ((double)w->nx * w->ny);
    if (m == 0) s *= 0.5;
    if (n == 0) s *= 0.5;
    return sqrt(A * A + B * B) * s;
}

static void set_mode(wave *w, int m, int n, double amp)
{
    double s = (double)w->nx * w->ny / 4.0;
    if (m == 0) s *= 2.0;
    if (n == 0) s *= 2.0;
    w->A[m + w->nx * n] = (float)(amp * s);
    w->B[m + w->nx * n] = 0.0f;
}

int main(void)
{
    int fails = 0, ok;
    const int N = 128, nc = 64;
    const double L = 10.0, depth = 50.0;         /* deep water */
    wave *w = wave_create(N, N, L, L, depth);
    wave_set_damping(w, 0.0);
    w->nu = 0.0;
    hos *h = hos_create(w, nc, 3);
    if (!w || !h) { printf("create failed\n"); return 1; }

    const int m = 8;
    const double k = M_PI * m / L;

    /* --- quadratic scaling: the change of the 2k mode over one step scales as a^2.
     * Start at maximum velocity (B, i.e. psi) rather than maximum elevation: with eta = 0
     * and psi = 0 respectively, every nonlinear term contains psi. --- */
    double d[2];
    for (int t = 0; t < 2; t++) {
        double a = (t == 0) ? 0.005 : 0.010;
        wave_clear(w);
        set_mode(w, m, 0, a);
        w->B[m] = w->A[m]; w->A[m] = 0.0f;
        hos_step(h, w, 0.01);
        d[t] = coef_amp(w, 2 * m, 0);
    }
    double ratio = d[1] / d[0];
    ok = ratio > 3.6 && ratio < 4.4;
    printf("second harmonic after one step: a=5mm -> %.3e, a=10mm -> %.3e, ratio %.2f (expect 4)  %s\n", d[0], d[1], ratio, ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- a steep standing wave: run 3 periods with the split scheme --- */
    const double a = 0.1 / k;                    /* ka = 0.1 */
    const double om = wave_omega(w, k), T = 2 * M_PI / om;
    wave_clear(w);
    set_mode(w, m, 0, a);
    const double dt = T / 60.0;
    double e0 = wave_norm(w), emax = 0, a2max = 0;
    int applied = 0, steps = (int)(3.0 * T / dt);
    for (int s = 0; s < steps; s++) {
        wave_step(w, dt / 2, 1);
        applied += hos_step(h, w, dt);
        wave_step(w, dt / 2, 1);
        double e = wave_norm(w); if (e > emax) emax = e;
        double a2 = coef_amp(w, 2 * m, 0); if (a2 > a2max) a2max = a2;
    }
    double stokes = 0.5 * k * a * a;             /* bound second harmonic of a progressive Stokes wave, for scale */
    ok = applied == steps && emax < 2.0 * e0 && a2max > 0.3 * stokes && a2max < 3.0 * stokes;
    printf("standing wave ka=0.1, 3 periods: applied %d/%d steps, energy max/initial %.3f, 2k amplitude max %.2e vs Stokes k a^2/2 = %.2e  %s\n",
           applied, steps, emax / e0, a2max, stokes, ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- mirror symmetry: a mode pair stays a cos-cos field (no sine content appears) ---
     * the DCT basis cannot even represent sines, so check that energy stays in even modes: trivially true;
     * instead check that the field's x -> -x mirror (row reversal) is preserved to float precision */
    wave_realize(w);
    double asym = 0, mx = 0;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N / 2; i++) {
            double e1 = w->eta[i + N * j], e2 = w->eta[(N - 1 - i) + N * j];
            if (fabs(e1) > mx) mx = fabs(e1);
            /* a cos(8 pi x / L) field with period L/4 is symmetric about x = 0 and x = L; rows mirror exactly */
            if (fabs(e1 - e2) > asym) asym = fabs(e1 - e2);
        }
    ok = asym / mx < 1e-3;
    printf("wall mirror symmetry preserved: max asymmetry %.2e (rel)  %s\n", asym / mx, ok ? "ok" : "FAIL");
    fails += !ok;

    hos_destroy(h);
    wave_destroy(w);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "all hos tests passed");
    return fails ? 1 : 0;
}
