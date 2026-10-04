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

    /* --- a steady pressure patch settles to its hydrostatic dent, eta = -p / (rho g) --- */
    {
        wave *q = wave_create(128, 128, 2.0, 2.0, 1.0);
        /* (the rotor's damping acts on both components, so a static load sees a
         * stiffness omega^2 + gamma^2: at the real default of 0.03/s that is 1e-4,
         * but a test in a hurry with 1.5/s would be 5% low) */
        wave_set_damping(q, 0.3);
        const double p0 = 10.0, s = 0.25, dt = 1.0 / 120.0;
        for (int it = 0; it < 120 * 40; it++) {
            wave_add_pressure(q, 1.0, 1.0, s, p0 * dt);
            wave_step(q, dt, 1);
        }
        wave_realize(q);
        /* the basin keeps its volume, so the dent's volume lifts the rest of the
         * surface by its mean: measure the dent against the far corner */
        const double got = q->eta[64 + 128 * 64] - q->eta[4 + 128 * 4], ref = -p0 / (q->rho * q->g);
        ok = fabs(got - ref) / fabs(ref) < 0.03;
        printf("steady 10 Pa patch: dent %.3f mm below the far water, hydrostatic -p/(rho g) = %.3f mm  %s\n", got * 1e3, ref * 1e3, ok ? "ok" : "FAIL");
        fails += !ok;
        const double lift = p0 / (q->rho * q->g) * 2.0 * M_PI * s * s / (2.0 * 2.0);
        ok = fabs(q->eta[4 + 128 * 4] - lift) < 0.1 * lift;
        printf("   far water lifted %.4f mm, the dent's volume over the basin %.4f mm  %s\n", q->eta[4 + 128 * 4] * 1e3, lift * 1e3, ok ? "ok" : "FAIL");
        fails += !ok;
        wave_destroy(q);
    }

    /* --- a patch dragged along at U leaves a Kelvin wake: transverse waves of
     *     wavelength 2 pi U^2 / g on the track behind it, deep water --- */
    {
        const double U = 1.0, L = 24.0, W = 8.0;
        wave *q = wave_create(512, 128, L, W, 20.0);    /* deep: kh >> 1 for the wake's k = g/U^2 */
        wave_set_damping(q, 0.05);
        const double dt = 1.0 / 120.0, s = 0.12, p0 = 200.0;
        double x = 2.0;
        for (int it = 0; it < 120 * 16; it++) {
            wave_add_pressure(q, x, 0.5 * W, s, p0 * dt);
            wave_step(q, dt, 1);
            x += U * dt;
        }
        wave_realize(q);
        /* zero crossings of eta along the track, from 1.5 m to 8 m behind the hull */
        const int j = 64;
        const double dxc = L / 512;
        double first = -1, last = -1; int nzc = 0;
        for (int i = (int)((x - 8.0) / dxc); i < (int)((x - 1.5) / dxc); i++) {
            const float a = q->eta[i + 512 * j], b = q->eta[i + 1 + 512 * j];
            if ((a < 0) != (b < 0)) { const double xc = (i + 0.5) * dxc + dxc * a / (a - b); if (first < 0) first = xc; last = xc; nzc++; }
        }
        const double lam = nzc > 1 ? 2.0 * (last - first) / (nzc - 1) : 0.0, lam_ref = 2.0 * M_PI * U * U / 9.81;
        ok = nzc >= 6 && fabs(lam - lam_ref) / lam_ref < 0.12;
        printf("hull at %.1f m/s: %d zero crossings on the track, transverse wavelength %.3f m vs 2 pi U^2/g = %.3f m  %s\n",
               U, nzc, lam, lam_ref, ok ? "ok" : "FAIL");
        fails += !ok;
        /* the wake's half-angle: the amplitude 3-6 m astern rises to a caustic at the
         * cusp line and collapses beyond it.  Kelvin: 19.47 degrees in deep water */
        double best = 0; int ang_peak = 0;
        for (int a = 0; a <= 40; a++) {
            const double th = a * M_PI / 180.0;
            double rms = 0; int n = 0;
            for (double d = 3.0; d <= 6.0; d += 0.05) {
                const double px = x - d * cos(th), py = 0.5 * W + d * sin(th);
                const int i = (int)(px / dxc), jj = (int)(py / (W / 128));
                if (i < 0 || i >= 512 || jj < 0 || jj >= 128) continue;
                const double e = q->eta[i + 512 * jj];
                rms += e * e; n++;
            }
            rms = n ? sqrt(rms / n) : 0;
            if (rms > best) { best = rms; ang_peak = a; }
        }
        ok = ang_peak >= 16 && ang_peak <= 22;
        printf("   the caustic peaks at %d degrees (Kelvin's cusp line: 19.5)  %s\n", ang_peak, ok ? "ok" : "FAIL");
        fails += !ok;
        wave_destroy(q);
    }

    /* --- and in shallow water the wedge opens out as the hull nears the critical
     *     speed sqrt(g h): the same run at depth 13 cm, Froude_h = 0.89 --- */
    {
        const double U = 1.0, L = 24.0, W = 8.0, h = 0.13;
        wave *q = wave_create(512, 128, L, W, h);
        wave_set_damping(q, 0.05);
        const double dt = 1.0 / 120.0, s = 0.12, p0 = 200.0;
        double x = 2.0;
        for (int it = 0; it < 120 * 16; it++) { wave_add_pressure(q, x, 0.5 * W, s, p0 * dt); wave_step(q, dt, 1); x += U * dt; }
        wave_realize(q);
        const double dxc = L / 512;
        double best = 0; int ang_peak = 0;
        for (int a = 0; a <= 60; a++) {
            const double th = a * M_PI / 180.0;
            double rms = 0; int n = 0;
            for (double d = 3.0; d <= 6.0; d += 0.05) {
                const double px = x - d * cos(th), py = 0.5 * W + d * sin(th);
                const int i = (int)(px / dxc), jj = (int)(py / (W / 128));
                if (i < 0 || i >= 512 || jj < 0 || jj >= 128) continue;
                const double e = q->eta[i + 512 * jj];
                rms += e * e; n++;
            }
            rms = n ? sqrt(rms / n) : 0;
            if (rms > best) { best = rms; ang_peak = a; }
        }
        ok = ang_peak >= 23 && ang_peak <= 35;
        printf("shallow, Froude_h %.2f: the caustic has moved out to %d degrees (wider than Kelvin's, on its way to 90 at the critical speed)  %s\n",
               U / sqrt(9.81 * h), ang_peak, ok ? "ok" : "FAIL");
        fails += !ok;
        wave_destroy(q);
    }

    printf("%s\n", fails ? "SOME TESTS FAILED" : "all wave tests passed");
    return fails ? 1 : 0;
}
