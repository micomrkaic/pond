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

/* Disk basin: discrete radial eigenvalues against the roots of J_m', exact
 * transform round trip, and a single mode against J_m(kappa rho) cos(m theta). */
#define _XOPEN_SOURCE 700   /* jn() */
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
    const int nt = 128, nr = 128;
    wave *w = wave_create_disk(nt, nr, 2.0, 0.5);     /* R = 1 */
    if (!w) { printf("create failed\n"); return 1; }

    /* --- kappa_mn vs roots of J_m' (Abramowitz & Stegun 9.5) --- */
    struct { int m, n; double x; } ref[] = {
        { 0, 1, 3.8317060 }, { 0, 2, 7.0155867 }, { 1, 1, 1.8411838 },
        { 1, 2, 5.3314428 }, { 2, 1, 3.0542370 }, { 3, 1, 4.2011889 }, { 5, 2, 10.5198608 },
    };
    double worst = 0;
    for (size_t k = 0; k < sizeof ref / sizeof ref[0]; k++) {
        /* for m = 0 the n-th nonzero root is index n (index 0 is kappa = 0);
         * for m > 0 the n-th root is index n - 1 */
        int idx = ref[k].m == 0 ? ref[k].n : ref[k].n - 1;
        double got = w->kappa[(size_t)ref[k].m * nr + idx];
        double rel = fabs(got - ref[k].x) / ref[k].x;
        if (rel > worst) worst = rel;
        printf("  kappa(m=%d,n=%d) = %.5f  (J_m' root %.5f)  rel %.1e\n", ref[k].m, ref[k].n, got, ref[k].x, rel);
    }
    int ok = worst < 3e-4;
    printf("eigenvalues vs Bessel roots: worst rel err %.1e  %s\n", worst, ok ? "ok" : "FAIL");
    fails += !ok;
    ok = w->kappa[0] < 1e-4;
    printf("m=0 n=0 is the mean level: kappa %.2e  %s\n", w->kappa[0], ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- round trip on the retained subspace: modes beyond the radial Nyquist are
     * dropped at injection, so project a random field once and check that the
     * projected field then survives inject -> realize exactly --- */
    float *f = malloc(sizeof(float) * nt * nr), *g = malloc(sizeof(float) * nt * nr);
    srand(7);
    for (int i = 0; i < nt * nr; i++) f[i] = (float)(rand() / (RAND_MAX + 1.0) - 0.5);
    wave_clear(w);
    memcpy(w->src_d, f, sizeof(float) * nt * nr);
    w->dirty_d = 1;
    wave_step(w, 0.001, 0);
    wave_realize(w);
    memcpy(f, w->eta, sizeof(float) * nt * nr);        /* the resolvable part */
    wave_clear(w);
    memcpy(w->src_d, f, sizeof(float) * nt * nr);
    w->dirty_d = 1;
    wave_step(w, 0.001, 0);
    wave_realize(w);
    double err = 0, mx = 0;
    for (int i = 0; i < nt * nr; i++) {
        if (fabs(w->eta[i] - f[i]) > err) err = fabs(w->eta[i] - f[i]);
        if (fabs(f[i]) > mx) mx = fabs(f[i]);
    }
    ok = err / mx < 1e-3;
    printf("transform round trip (retained modes): max rel err %.2e  %s\n", err / mx, ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- a single mode (m=3, n=2) against J_3(kappa rho) cos(3 theta) --- */
    wave_clear(w);
    const int m = 3, nidx = 1;
    const size_t idx = (size_t)m * nr + nidx;
    w->A[idx] = 1.0f;
    wave_realize(w);
    const double kap = w->kappa[idx];
    double c = 0, ss = 0, ee = 0;
    for (int i = 0; i < nr; i++) {
        const double rho = (i + 0.5) / nr;
        for (int j = 0; j < nt; j++) {
            const double th = 2 * M_PI * j / nt;
            const double refv = jn(m, kap * rho) * cos(m * th);
            c += refv * w->eta[j + nt * i]; ss += refv * refv; ee += (double)w->eta[j + nt * i] * w->eta[j + nt * i];
        }
    }
    double corr = fabs(c) / sqrt(ss * ee);     /* eigenvector sign is arbitrary */
    ok = corr > 0.9999;
    printf("mode (m=3,n=2) shape vs J_3(kappa rho) cos(3 theta): correlation %.6f  %s\n", corr, ok ? "ok" : "FAIL");
    fails += !ok;

    /* --- exactness in time on the disk: frequency of that mode --- */
    const double k = kap / w->R, om = wave_omega(w, k), gam = 2 * w->nu * k * k + w->gamma0;
    wave_clear(w); w->A[idx] = 1.0f;
    const int ci = 40, cj = 5;
    wave_realize(w);
    const double shape = w->eta[cj + nt * ci];
    double maxe = 0;
    const double dt = 0.0113;
    for (int s = 0; s <= 300; s++) {
        wave_realize(w);
        const double refv = shape * cos(om * s * dt) * exp(-gam * s * dt);
        if (fabs(w->eta[cj + nt * ci] - refv) > maxe) maxe = fabs(w->eta[cj + nt * ci] - refv);
        wave_step(w, dt, 1);
    }
    ok = maxe / fabs(shape) < 1e-4;
    printf("disk mode oscillates at omega=%.4f (T=%.3f s): max rel err %.2e  %s\n", om, 2 * M_PI / om, maxe / fabs(shape), ok ? "ok" : "FAIL");
    fails += !ok;

    free(f); free(g);
    wave_destroy(w);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "all disk tests passed");
    return fails ? 1 : 0;
}
