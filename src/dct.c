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

#include "dct.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int bitrev(int x, int bits)
{
    int r = 0;
    for (int i = 0; i < bits; i++) { r = (r << 1) | (x & 1); x >>= 1; }
    return r;
}

int dct_plan_init(dct_plan *p, int n)
{
    if (n < 4 || (n & (n - 1)) != 0) return -1;
    memset(p, 0, sizeof *p);
    p->n = n;
    int bits = 0;
    while ((1 << bits) < n) bits++;

    p->rev = malloc(sizeof(int) * (size_t)n);
    p->wr  = malloc(sizeof(float) * (size_t)(n / 2));
    p->wi  = malloc(sizeof(float) * (size_t)(n / 2));
    p->cr  = malloc(sizeof(float) * (size_t)n);
    p->ci  = malloc(sizeof(float) * (size_t)n);
    p->re  = malloc(sizeof(float) * (size_t)n);
    p->im  = malloc(sizeof(float) * (size_t)n);
    if (!p->rev || !p->wr || !p->wi || !p->cr || !p->ci || !p->re || !p->im) {
        dct_plan_free(p);
        return -1;
    }
    for (int i = 0; i < n; i++) p->rev[i] = bitrev(i, bits);
    for (int k = 0; k < n / 2; k++) {
        double th = -2.0 * M_PI * k / n;
        p->wr[k] = (float)cos(th);
        p->wi[k] = (float)sin(th);
    }
    for (int k = 0; k < n; k++) {
        double th = -M_PI * k / (2.0 * n);
        p->cr[k] = (float)cos(th);
        p->ci[k] = (float)sin(th);
    }
    return 0;
}

void dct_plan_free(dct_plan *p)
{
    free(p->rev); free(p->wr); free(p->wi);
    free(p->cr);  free(p->ci); free(p->re); free(p->im);
    memset(p, 0, sizeof *p);
}

/* In-place iterative radix-2 DIT. inverse=1 uses e^{+i...} (unnormalized). */
static void fft(const dct_plan *p, float *re, float *im, int inverse)
{
    const int n = p->n;
    const float sgn = inverse ? -1.0f : 1.0f;

    for (int i = 0; i < n; i++) {
        int j = p->rev[i];
        if (j > i) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            float *r0 = re + i, *i0 = im + i, *r1 = re + i + half, *i1 = im + i + half;
            for (int j = 0; j < half; j++) {
                const float wr = p->wr[j * step], wi = sgn * p->wi[j * step];
                const float vr = r1[j] * wr - i1[j] * wi;
                const float vi = r1[j] * wi + i1[j] * wr;
                const float ur = r0[j], ui = i0[j];
                r0[j] = ur + vr; i0[j] = ui + vi;
                r1[j] = ur - vr; i1[j] = ui - vi;
            }
        }
    }
}

void dct_fft(const dct_plan *p, float *re, float *im, int inverse) { fft(p, re, im, inverse); }

void dct_forward(const dct_plan *p, const float *x, float *X)
{
    const int n = p->n, h = n / 2;
    float *re = p->re, *im = p->im;
    for (int i = 0; i < h; i++) {
        re[i]         = x[2 * i];
        re[n - 1 - i] = x[2 * i + 1];
    }
    memset(im, 0, sizeof(float) * (size_t)n);
    fft(p, re, im, 0);
    for (int k = 0; k < n; k++)
        X[k] = re[k] * p->cr[k] - im[k] * p->ci[k];   /* Re(V_k e^{-i pi k/2n}) */
}

void dct_inverse(const dct_plan *p, const float *X, float *x)
{
    const int n = p->n, h = n / 2;
    float *re = p->re, *im = p->im;
    re[0] = X[0]; im[0] = 0.0f;
    for (int k = 1; k < n; k++) {
        /* V_k = (X_k - i X_{n-k}) e^{+i pi k/2n} */
        const float a = X[k], b = -X[n - k];
        re[k] = a * p->cr[k] + b * p->ci[k];
        im[k] = b * p->cr[k] - a * p->ci[k];
    }
    fft(p, re, im, 1);
    const float s = 1.0f / (float)n;
    for (int j = 0; j < h; j++) {
        x[2 * j]     = re[j] * s;
        x[2 * j + 1] = re[n - 1 - j] * s;
    }
}

static void dct2_apply(const dct_plan *px, const dct_plan *py, float *f, float *tmp,
                       void (*t1)(const dct_plan *, const float *, float *))
{
    const int nx = px->n, ny = py->n;
    for (int j = 0; j < ny; j++) t1(px, f + (size_t)j * nx, f + (size_t)j * nx);
    for (int i = 0; i < nx; i++) {
        for (int j = 0; j < ny; j++) tmp[j] = f[i + (size_t)j * nx];
        t1(py, tmp, tmp);
        for (int j = 0; j < ny; j++) f[i + (size_t)j * nx] = tmp[j];
    }
}

void dct2_forward(const dct_plan *px, const dct_plan *py, float *f, float *tmp)
{
    dct2_apply(px, py, f, tmp, dct_forward);
}

void dct2_inverse(const dct_plan *px, const dct_plan *py, float *f, float *tmp)
{
    dct2_apply(px, py, f, tmp, dct_inverse);
}
