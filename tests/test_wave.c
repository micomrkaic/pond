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

/* Checks that a single basin mode oscillates at the analytic Airy frequency,
 * decays at 2 nu k^2 + gamma0, and that an injected drop is reproduced exactly. */
#include "../src/wave.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(void)
{
    int fails = 0;
    const int nx = 64, ny = 32;
    wave *w = wave_create(nx, ny, 1.0, 0.5, 0.1);
    if (!w) { printf("create failed\n"); return 1; }
    wave_set_damping(w, 0.2);

    /* --- mode (m,n) = (4,3): compare eta at a cell with A cos(wt) e^{-gt} --- */
    const int m = 4, n = 3;
    const double k = M_PI * hypot(m / w->Lx, n / w->Ly);
    const double om = wave_omega(w, k), gam = 2 * w->nu * k * k + w->gamma0;
    wave_clear(w);
    wave_set_mode(w, m, n, 1.0f, 0.0f);
    /* value of that mode at cell (i,j): (2/nx)(2/ny) cos cos */
    const int ci = 5, cj = 7;
    const double shape = (4.0 / (nx * ny)) * cos(M_PI * m * (ci + 0.5) / nx) * cos(M_PI * n * (cj + 0.5) / ny);

    const double dt = 0.00731;   /* deliberately odd */
    double maxerr = 0;
    for (int s = 0; s <= 400; s++) {
        wave_realize(w);
        const double t = s * dt;
        const double ref = shape * cos(om * t) * exp(-gam * t);
        const double got = w->eta[ci + nx * cj];
        if (fabs(got - ref) > maxerr) maxerr = fabs(got - ref);
        wave_step(w, dt, 1);
    }
    int ok = maxerr / fabs(shape) < 1e-5;
    printf("mode (%d,%d): k=%.3f omega=%.4f rad/s (T=%.3f s) gamma=%.4f  max rel err %.2e  %s\n",
           m, n, k, om, 2 * M_PI / om, gam, maxerr / fabs(shape), ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- one big step must equal many small ones (exactness in time) --- */
    wave_clear(w); wave_set_mode(w, m, n, 0.7f, -0.2f);
    wave_step(w, 1.234, 1);
    float a1 = w->A[m + nx * n], b1 = w->B[m + nx * n];
    wave_clear(w); wave_set_mode(w, m, n, 0.7f, -0.2f);
    wave_step(w, 1.234 / 1000, 1000);
    float a2 = w->A[m + nx * n], b2 = w->B[m + nx * n];
    ok = fabs(a1 - a2) < 1e-4 && fabs(b1 - b2) < 1e-4;
    printf("1 x 1.234 s vs 1000 x 1.234 ms: (%.6f,%.6f) vs (%.6f,%.6f)  %s\n", a1, b1, a2, b2, ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- drop injection: realize(inject(f)) == f (minus the mean, which is zeroed) --- */
    wave_clear(w);
    wave_add_drop(w, 0.4, 0.2, 0.03, -0.002);
    float *ref = malloc(sizeof(float) * nx * ny);
    memcpy(ref, w->src_d, sizeof(float) * nx * ny);
    double mean = 0;
    for (int i = 0; i < nx * ny; i++) mean += ref[i];
    mean /= nx * ny;
    wave_step(w, dt, 0);
    wave_realize(w);
    double err = 0, mx = 0;
    for (int i = 0; i < nx * ny; i++) {
        double d = fabs(w->eta[i] - (ref[i] - mean));
        if (d > err) err = d;
        if (fabs(ref[i]) > mx) mx = fabs(ref[i]);
    }
    ok = err / mx < 1e-4;
    printf("drop injection round trip: max rel err %.2e (mean removed: %.2e)  %s\n", err / mx, mean, ok ? "ok" : "FAIL");
    fails += !ok;
    free(ref);

    /* --- dispersion sanity: deep-water gravity and capillary limits --- */
    wave_set_pool(w, 10.0, 5.0, 100.0);
    double kk = 2 * M_PI / 1.0;   /* 1 m wave, deep */
    double o = wave_omega(w, kk), oref = sqrt(9.81 * kk);
    ok = fabs(o - oref) / oref < 1e-3;
    printf("deep gravity 1 m wave: omega %.4f vs sqrt(gk) %.4f  %s\n", o, oref, ok ? "ok" : "FAIL");
    fails += !ok;
    kk = 2 * M_PI / 0.002;        /* 2 mm capillary */
    o = wave_omega(w, kk); oref = sqrt(0.072 / 1000 * kk * kk * kk);
    ok = fabs(o - oref) / oref < 2e-2;
    printf("2 mm capillary wave: omega %.1f vs sqrt(sigma k^3/rho) %.1f  %s\n", o, oref, ok ? "ok" : "FAIL");
    fails += !ok;

    wave_destroy(w);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "all wave tests passed");
    return fails ? 1 : 0;
}
