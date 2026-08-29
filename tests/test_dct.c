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

/* Checks the FFT-based DCT against the O(N^2) definition, plus 2-D round trip. */
#include "../src/dct.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double urand(void) { return rand() / (RAND_MAX + 1.0) * 2.0 - 1.0; }

static int test_1d(int n)
{
    dct_plan p;
    if (dct_plan_init(&p, n)) { printf("plan init failed for n=%d\n", n); return 1; }
    float *x = malloc(sizeof(float) * n), *X = malloc(sizeof(float) * n), *y = malloc(sizeof(float) * n);
    double *Xn = malloc(sizeof(double) * n);
    for (int i = 0; i < n; i++) x[i] = (float)urand();

    dct_forward(&p, x, X);
    double maxabs = 0, err = 0;
    for (int k = 0; k < n; k++) {
        double s = 0;
        for (int i = 0; i < n; i++) s += x[i] * cos(M_PI * k * (2 * i + 1) / (2.0 * n));
        Xn[k] = s;
        if (fabs(s) > maxabs) maxabs = fabs(s);
        if (fabs(s - X[k]) > err) err = fabs(s - X[k]);
    }
    double rel_fwd = err / maxabs;

    dct_inverse(&p, X, y);
    double rt = 0;
    for (int i = 0; i < n; i++) if (fabs(y[i] - x[i]) > rt) rt = fabs(y[i] - x[i]);

    /* inverse against naive DCT-III of the exact coefficients */
    double inv_err = 0;
    for (int i = 0; i < n; i++) {
        double s = Xn[0] / 2;
        for (int k = 1; k < n; k++) s += Xn[k] * cos(M_PI * k * (2 * i + 1) / (2.0 * n));
        s *= 2.0 / n;
        if (fabs(s - x[i]) > inv_err) inv_err = fabs(s - x[i]);
    }

    int ok = rel_fwd < 1e-5 && rt < 1e-5 && inv_err < 1e-9;
    printf("n=%4d  fwd rel err %.2e  roundtrip %.2e  naive-inverse %.2e  %s\n",
           n, rel_fwd, rt, inv_err, ok ? "ok" : "FAIL");
    free(x); free(X); free(y); free(Xn);
    dct_plan_free(&p);
    return !ok;
}

static int test_2d(int nx, int ny)
{
    dct_plan px, py;
    dct_plan_init(&px, nx);
    dct_plan_init(&py, ny);
    float *f = malloc(sizeof(float) * nx * ny), *g = malloc(sizeof(float) * nx * ny);
    float *g0 = malloc(sizeof(float) * nx * ny);
    float *tmp = malloc(sizeof(float) * (nx > ny ? nx : ny));
    for (int i = 0; i < nx * ny; i++) g0[i] = g[i] = (float)urand();

    /* a single 2-D cosine mode must land in one coefficient */
    int m = 3, n = 5;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            f[i + nx * j] = (float)(cos(M_PI * m * (i + 0.5) / nx) * cos(M_PI * n * (j + 0.5) / ny));
    dct2_forward(&px, &py, f, tmp);
    double expect = 0.25 * nx * ny, off = 0;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            if (!(i == m && j == n) && fabs(f[i + nx * j]) > off) off = fabs(f[i + nx * j]);
    double diag = f[m + nx * n];
    int ok1 = fabs(diag - expect) / expect < 1e-5 && off / expect < 1e-5;
    printf("2d %dx%d mode (%d,%d): coeff %.3f (expect %.3f), max off-mode %.2e  %s\n",
           nx, ny, m, n, diag, expect, off, ok1 ? "ok" : "FAIL");

    dct2_forward(&px, &py, g, tmp);
    dct2_inverse(&px, &py, g, tmp);
    double rt = 0;
    for (int i = 0; i < nx * ny; i++) if (fabs(g[i] - g0[i]) > rt) rt = fabs(g[i] - g0[i]);
    int ok2 = rt < 1e-4;
    printf("2d %dx%d roundtrip max err %.2e  %s\n", nx, ny, rt, ok2 ? "ok" : "FAIL");

    free(f); free(g); free(g0); free(tmp);
    dct_plan_free(&px); dct_plan_free(&py);
    return !(ok1 && ok2);
}

int main(void)
{
    int fails = 0;
    srand(1);
    fails += test_1d(4);
    fails += test_1d(8);
    fails += test_1d(16);
    fails += test_1d(64);
    fails += test_1d(512);
    fails += test_2d(64, 32);
    fails += test_2d(256, 256);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "all dct tests passed");
    return fails ? 1 : 0;
}
