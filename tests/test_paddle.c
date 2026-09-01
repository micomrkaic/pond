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

/* The wavemaker.  Driven at a frequency, it should radiate the wavelength the
 * dispersion relation names for that frequency; a full-wall paddle should make
 * a wave that does not vary along the wall; the four walls should give the same
 * picture turned round; and a short paddle should put its energy where it sits.
 */
#include "../src/wave.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", what, ok ? " ok" : " FAILED");
    if (!ok) fails++;
}

/* run a paddle for `secs` of simulated time */
static void drive(wave *w, int wall, double pos, double span, double hz, double secs)
{
    const double L = sqrt(w->Lx * w->Ly), dt = 1.0 / 400.0;
    const double om = 2.0 * M_PI * hz;
    for (int i = 0; i < (int)(secs / dt); i++) {
        const double accel = 0.0025 * L * om * om * cos(om * w->t);
        wave_add_paddle(w, wall, pos, span, 0.006 * L, accel, dt);
        wave_step(w, dt, 1);
    }
    wave_realize(w);
}

/* mean square surface height over a rectangle of cells */
static double energy_box(const wave *w, int i0, int i1, int j0, int j1)
{
    double s = 0;
    int n = 0;
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++) { const double e = w->eta[i + w->nx * j]; s += e * e; n++; }
    return n ? s / n : 0.0;
}

int main(void)
{
    const int nx = 128, ny = 128;
    const double Lx = 3.0, Ly = 3.0, depth = 0.6;

    /* --- the inverse dispersion relation round-trips --- */
    {
        wave *w = wave_create(nx, ny, Lx, Ly, depth);
        if (!w) { printf("create failed\n"); return 1; }
        double worst = 0;
        for (double k = 0.5; k < 2000.0; k *= 1.7) {
            const double back = wave_k_of_omega(w, wave_omega(w, k));
            const double rel = fabs(back - k) / k;
            if (rel > worst) worst = rel;
        }
        printf("k -> omega -> k over 0.5..2000 rad/m: worst rel err %.2e ", worst);
        check(worst < 1e-9, "");
        wave_destroy(w);
    }

    /* --- driven at f, the peak mode should be the one at k(2 pi f) --- */
    {
        const double hz = 3.0;
        wave *w = wave_create(nx, ny, Lx, Ly, depth);
        wave_set_damping(w, 0.05);
        const double k_want = wave_k_of_omega(w, 2.0 * M_PI * hz);
        drive(w, 0, 0.5, 1.0, hz, 6.0);
        /* the plane-wave modes are (m, 0), i.e. index m */
        int best = 0;
        double bestE = 0;
        for (int m = 1; m < nx; m++) {
            const double e = (double)w->A[m] * w->A[m] + (double)w->B[m] * w->B[m];
            if (e > bestE) { bestE = e; best = m; }
        }
        const double k_got = M_PI * best / w->Lx;
        const double rel = fabs(k_got - k_want) / k_want;
        printf("driven at %.3g Hz: peak mode m=%d, k=%.4g rad/m (dispersion says %.4g, lambda %.3g m) rel %.1e ",
               hz, best, k_got, k_want, 2.0 * M_PI / k_want, rel);
        check(rel < 0.03, "");
        wave_destroy(w);
    }

    /* the same tank driven twice as fast should make a shorter wave, and by the
     * factor finite-depth gravity waves ask for, not some other one */
    {
        wave *w1 = wave_create(nx, ny, Lx, Ly, depth), *w2 = wave_create(nx, ny, Lx, Ly, depth);
        const double k1 = wave_k_of_omega(w1, 2.0 * M_PI * 1.5), k2 = wave_k_of_omega(w2, 2.0 * M_PI * 3.0);
        printf("1.5 Hz -> %.3g m, 3 Hz -> %.3g m in %.2g m of water ", 2 * M_PI / k1, 2 * M_PI / k2, depth);
        check(k2 > k1 && wave_omega(w1, k2) / wave_omega(w1, k1) > 1.99 && wave_omega(w1, k2) / wave_omega(w1, k1) < 2.01, "");
        wave_destroy(w1); wave_destroy(w2);
    }

    /* --- a full-wall paddle makes a wave that does not vary along the wall --- */
    {
        wave *w = wave_create(nx, ny, Lx, Ly, depth);
        drive(w, 0, 0.5, 1.0, 3.0, 2.0);
        double worst = 0, scale = 0;
        for (int i = 0; i < nx; i++) {
            double lo = w->eta[i], hi = w->eta[i];
            for (int j = 0; j < ny; j++) {
                const double e = w->eta[i + nx * j];
                if (e < lo) lo = e;
                if (e > hi) hi = e;
                if (fabs(e) > scale) scale = fabs(e);
            }
            if (hi - lo > worst) worst = hi - lo;
        }
        printf("full-wall paddle: variation along the wall %.1e of the wave height ", worst / scale);
        check(worst / scale < 1e-4, "");
        wave_destroy(w);
    }

    /* --- wall 1 is wall 0 mirrored; wall 2 is wall 0 transposed --- */
    {
        wave *w0 = wave_create(nx, ny, Lx, Ly, depth);
        wave *w1 = wave_create(nx, ny, Lx, Ly, depth);
        wave *w2 = wave_create(nx, ny, Lx, Ly, depth);
        drive(w0, 0, 0.5, 1.0, 3.0, 2.0);
        drive(w1, 1, 0.5, 1.0, 3.0, 2.0);
        drive(w2, 2, 0.5, 1.0, 3.0, 2.0);
        double dm = 0, dt_ = 0, scale = 0;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                const double a = w0->eta[i + nx * j];
                const double b = w1->eta[(nx - 1 - i) + nx * j];
                const double c = w2->eta[j + nx * i];
                if (fabs(a - b) > dm) dm = fabs(a - b);
                if (fabs(a - c) > dt_) dt_ = fabs(a - c);
                if (fabs(a) > scale) scale = fabs(a);
            }
        printf("x=Lx is x=0 mirrored (%.1e), y=0 is it transposed (%.1e) ", dm / scale, dt_ / scale);
        check(dm / scale < 1e-5 && dt_ / scale < 1e-5, "");
        wave_destroy(w0); wave_destroy(w1); wave_destroy(w2);
    }

    /* --- a short paddle puts its energy where it sits --- */
    {
        wave *w = wave_create(nx, ny, Lx, Ly, depth);
        wave_set_damping(w, 0.3);
        drive(w, 0, 0.25, 0.15, 4.0, 2.0);
        /* look in the strip next to the driven wall, near the paddle and far from it */
        const double near = energy_box(w, 0, nx / 8, ny / 8, 3 * ny / 8);
        const double far  = energy_box(w, 0, nx / 8, 5 * ny / 8, 7 * ny / 8);
        printf("15%% paddle at 25%% of the wall: mean square height near it %.2e, at the far end %.2e ", near, far);
        check(near > 20.0 * far, "");
        wave_destroy(w);
    }

    /* --- moving it moves the waves --- */
    {
        wave *wa = wave_create(nx, ny, Lx, Ly, depth), *wb = wave_create(nx, ny, Lx, Ly, depth);
        wave_set_damping(wa, 0.3); wave_set_damping(wb, 0.3);
        drive(wa, 0, 0.25, 0.15, 4.0, 2.0);
        drive(wb, 0, 0.75, 0.15, 4.0, 2.0);
        const double a_up = energy_box(wa, 0, nx / 8, ny / 8, 3 * ny / 8);
        const double b_up = energy_box(wb, 0, nx / 8, ny / 8, 3 * ny / 8);
        const double a_dn = energy_box(wa, 0, nx / 8, 5 * ny / 8, 7 * ny / 8);
        const double b_dn = energy_box(wb, 0, nx / 8, 5 * ny / 8, 7 * ny / 8);
        printf("at 25%% vs 75%%: the loud end swaps (%.2e/%.2e vs %.2e/%.2e) ", a_up, a_dn, b_up, b_dn);
        check(a_up > 20.0 * a_dn && b_dn > 20.0 * b_up, "");
        wave_destroy(wa); wave_destroy(wb);
    }

    /* --- the disk: a sector is local, the whole rim is axisymmetric --- */
    {
        wave *w = wave_create_disk(128, 64, 3.0, 0.6);
        if (!w) { printf("disk create failed\n"); return 1; }
        drive(w, 0, 0.0, 1.0, 3.0, 2.0);
        /* the disk field is (angle, ring): a rim-wide paddle must not vary with angle */
        double worst = 0, scale = 0;
        for (int r = 0; r < w->nr; r++) {
            double lo = w->eta[w->nx * r], hi = lo;
            for (int t = 0; t < w->nt; t++) {
                const double e = w->eta[t + w->nx * r];
                if (e < lo) lo = e;
                if (e > hi) hi = e;
                if (fabs(e) > scale) scale = fabs(e);
            }
            if (hi - lo > worst) worst = hi - lo;
        }
        printf("disk, whole rim: variation with angle %.1e of the wave height ", worst / scale);
        check(worst / scale < 1e-3, "");
        wave_destroy(w);
    }

    printf(fails ? "%d paddle test(s) failed\n" : "all paddle tests passed\n", fails);
    return fails ? 1 : 0;
}
