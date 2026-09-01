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

/* dct.h — DCT-II / DCT-III (forward / inverse) via a radix-2 complex FFT.
 *
 * Conventions (N a power of two, N >= 4):
 *   forward:  X[k] = sum_{n} x[n] cos(pi k (2n+1) / 2N),            k = 0..N-1
 *   inverse:  x[n] = (2/N) [ X[0]/2 + sum_{k>=1} X[k] cos(pi k (2n+1) / 2N) ]
 * so inverse(forward(x)) == x.  Samples sit at half-integer positions
 * x_n = (n + 1/2) dx, which is exactly the even ("Neumann") extension a
 * rigid wall wants.  Mode m has wavenumber k_m = pi m / L.
 *
 * Makhoul's trick: an N-point DCT is one N-point complex FFT plus a
 * pre/post twiddle, no zero padding.  C17, no dependencies.
 */
#ifndef POND_DCT_H
#define POND_DCT_H

typedef struct {
    int n;
    int *rev;          /* bit-reversal permutation */
    float *wr, *wi;    /* FFT twiddles e^{-2 pi i k / n}, k < n/2 */
    float *cr, *ci;    /* DCT twiddles e^{-i pi k / 2n}, k < n */
    float *re, *im;    /* scratch */
} dct_plan;

int  dct_plan_init(dct_plan *p, int n);   /* 0 on success, -1 if n invalid / OOM */
void dct_plan_free(dct_plan *p);

/* The underlying complex FFT of length n, in place, unnormalised (inverse=1 uses e^{+i}). */
void dct_fft(const dct_plan *p, float *re, float *im, int inverse);

/* 1-D; x and X may alias. */
void dct_forward(const dct_plan *p, const float *x, float *X);
void dct_inverse(const dct_plan *p, const float *X, float *x);

/* 2-D, in place. f is ny rows of nx floats (row-major, index i + nx*j).
 * tmp must hold max(nx, ny) floats. */
void dct2_forward(const dct_plan *px, const dct_plan *py, float *f, float *tmp);
void dct2_inverse(const dct_plan *px, const dct_plan *py, float *f, float *tmp);

#endif
