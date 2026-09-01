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

/* disk.c — the circular basin.
 *
 * Polar grid: nt angles (columns) x nr rings (rows), cell centres at
 * rho_i = (i + 1/2) / nr of the radius and theta_j = 2 pi j / nt.
 *
 * Basis: an FFT in theta gives angular modes m = 0 .. nt/2, each with a real
 * (cos) and an imaginary (sin) plane.  In the radial direction the modes are
 * the eigenvectors of the discrete radial Laplacian
 *     (1/rho) d/drho (rho d/drho) - m^2 / rho^2
 * with a rigid wall at rho = 1 (finite volumes, so regularity at the centre
 * and the Neumann wall both come out of the flux form).  Symmetrised with
 * sqrt(rho) that is a symmetric tridiagonal matrix per m, so its eigenvectors
 * are exactly orthonormal and the forward transform is exactly the inverse
 * of the backward one - the disk analogue of the DCT.  The eigenvalues are
 * -kappa^2; kappa approximates the Bessel roots x'_mn (J_m'(x) = 0) to O(h^2)
 * and the physical wavenumber is k = kappa / R.
 *
 * Eigenpairs: bisection on the Sturm sequence, then inverse iteration.
 */
#include "wave.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 4-lane float vectors (GCC/clang extension; maps to SSE, NEON and wasm SIMD).
 * Loads go through memcpy so alignment is never an issue. */
typedef float v4 __attribute__((vector_size(16)));
static inline v4 ld4(const float *p) { v4 v; memcpy(&v, p, sizeof v); return v; }
static inline void st4(float *p, v4 v) { memcpy(p, &v, sizeof v); }
static inline float hsum4(v4 v) { return v[0] + v[1] + v[2] + v[3]; }

/* number of eigenvalues of the symmetric tridiagonal (d, e) that are < x */
static int sturm_count(int n, const double *d, const double *e, double x)
{
    int cnt = 0;
    double q = d[0] - x;
    if (q < 0) cnt++;
    for (int i = 1; i < n; i++) {
        if (fabs(q) < 1e-300) q = (q < 0) ? -1e-300 : 1e-300;
        q = d[i] - x - e[i - 1] * e[i - 1] / q;
        if (q < 0) cnt++;
    }
    return cnt;
}

/* Solve (T - s I) x = b for the symmetric tridiagonal T, Gaussian elimination
 * with partial pivoting (fill-in of one extra super-diagonal). */
static void tridiag_solve_shifted(int n, const double *d, const double *e, double s, double *x,
                                  double *a, double *b, double *c)
{
    /* bands: a = diagonal, b = super, c = super-super, all after pivoting; x holds rhs then solution */
    for (int i = 0; i < n; i++) { a[i] = d[i] - s; b[i] = (i < n - 1) ? e[i] : 0.0; c[i] = 0.0; }
    for (int i = 0; i < n - 1; i++) {
        double sub = e[i];                 /* T[i+1][i] */
        if (fabs(sub) > fabs(a[i])) {
            /* swap rows i and i+1 */
            double t;
            t = a[i]; a[i] = sub; sub = t;
            t = b[i]; b[i] = a[i + 1]; a[i + 1] = t;
            t = c[i]; c[i] = b[i + 1]; b[i + 1] = t;
            t = x[i]; x[i] = x[i + 1]; x[i + 1] = t;
        }
        if (fabs(a[i]) < 1e-300) a[i] = 1e-300;
        double f = sub / a[i];
        a[i + 1] -= f * b[i];
        b[i + 1] -= f * c[i];
        x[i + 1] -= f * x[i];
    }
    if (fabs(a[n - 1]) < 1e-300) a[n - 1] = 1e-300;
    x[n - 1] /= a[n - 1];
    for (int i = n - 2; i >= 0; i--) {
        double v = x[i] - b[i] * x[i + 1];
        if (i + 2 < n) v -= c[i] * x[i + 2];
        x[i] = v / a[i];
    }
}

/* Eigenvalues only, implicit QL with Wilkinson shifts (the classic tqli, without
 * the eigenvector accumulation).  d in/out, e is destroyed.  Returns -1 if it
 * fails to converge, in which case the caller falls back to bisection. */
static int ql_eigenvalues(int n, double *d, double *e)
{
    e[n - 1] = 0.0;
    for (int l = 0; l < n; l++) {
        int iter = 0, m;
        do {
            for (m = l; m < n - 1; m++) {
                double dd = fabs(d[m]) + fabs(d[m + 1]);
                if (fabs(e[m]) <= 1e-15 * dd) break;
            }
            if (m != l) {
                if (iter++ == 100) return -1;
                double g = (d[l + 1] - d[l]) / (2.0 * e[l]);
                double r = hypot(g, 1.0);
                g = d[m] - d[l] + e[l] / (g + (g >= 0 ? r : -r));
                double s = 1.0, c = 1.0, p = 0.0;
                int i;
                for (i = m - 1; i >= l; i--) {
                    double f = s * e[i], b = c * e[i];
                    r = hypot(f, g);
                    e[i + 1] = r;
                    if (r == 0.0) { d[i + 1] -= p; e[m] = 0.0; break; }
                    s = f / r; c = g / r;
                    g = d[i + 1] - p;
                    r = (d[i] - g) * s + 2.0 * c * b;
                    p = s * r;
                    d[i + 1] = g + p;
                    g = c * r - b;
                }
                if (r == 0.0 && i >= l) continue;
                d[l] -= p; e[l] = g; e[m] = 0.0;
            }
        } while (m != l);
    }
    return 0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void bisection_eigenvalues(int n, const double *d, const double *e, double *lam)
{
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < n; i++) {
        double r = (i > 0 ? fabs(e[i - 1]) : 0.0) + (i < n - 1 ? fabs(e[i]) : 0.0);
        if (d[i] - r < lo) lo = d[i] - r;
        if (d[i] + r > hi) hi = d[i] + r;
    }
    double a0 = lo;
    for (int k = 0; k < n; k++) {
        double a = a0, b = hi;
        for (int it = 0; it < 200 && b - a > 1e-13 * (fabs(a) + fabs(b)) + 1e-300; it++) {
            double mid = 0.5 * (a + b);
            if (sturm_count(n, d, e, mid) > k) b = mid; else a = mid;
        }
        lam[k] = 0.5 * (a + b);
        a0 = a;
    }
}

/* All eigenpairs of the symmetric tridiagonal (d, e), eigenvalues ascending.
 * G receives the eigenvectors as columns: G[i*n + k] = component i of vector k. */
static void tridiag_eig(int n, const double *d, const double *e, double *lam, float *G,
                        double *wa, double *wb, double *wc, double *wx)
{
    /* eigenvalues: QL on copies, bisection if QL misbehaves */
    for (int i = 0; i < n; i++) { wa[i] = d[i]; wb[i] = (i < n - 1) ? e[i] : 0.0; }
    if (ql_eigenvalues(n, wa, wb) == 0) {
        qsort(wa, (size_t)n, sizeof(double), cmp_double);
        memcpy(lam, wa, sizeof(double) * (size_t)n);
    } else {
        bisection_eigenvalues(n, d, e, lam);
    }
    /* eigenvectors by inverse iteration */
    unsigned long long seed = 0x2545F4914F6CDD1DULL;
    for (int k = 0; k < n; k++) {
        double shift = lam[k] + 1e-10 * (fabs(lam[k]) + 1.0);
        for (int i = 0; i < n; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            wx[i] = (double)(seed >> 11) / 9007199254740992.0 - 0.5;
        }
        for (int it = 0; it < 3; it++) {
            tridiag_solve_shifted(n, d, e, shift, wx, wa, wb, wc);
            /* orthogonalise against a close neighbour, then normalise */
            if (k > 0 && fabs(lam[k] - lam[k - 1]) < 1e-6 * (fabs(lam[k]) + 1.0)) {
                double dot = 0;
                for (int i = 0; i < n; i++) dot += wx[i] * G[(size_t)i * n + k - 1];
                for (int i = 0; i < n; i++) wx[i] -= dot * G[(size_t)i * n + k - 1];
            }
            double nrm = 0;
            for (int i = 0; i < n; i++) nrm += wx[i] * wx[i];
            nrm = 1.0 / sqrt(nrm > 0 ? nrm : 1.0);
            for (int i = 0; i < n; i++) wx[i] *= nrm;
        }
        for (int i = 0; i < n; i++) G[(size_t)i * n + k] = (float)wx[i];
    }
}

/* Build the radial basis for the unit disk: for each m < M, nr eigenvectors and
 * kappa (>= 0).  G is M * nr * nr floats, kappa M * nr.  Returns 0 or -1. */
int disk_basis_build(int nr, int M, float *G, float *kappa)
{
    double *d = malloc(sizeof(double) * nr), *e = malloc(sizeof(double) * nr);
    double *lam = malloc(sizeof(double) * nr);
    double *wa = malloc(sizeof(double) * nr), *wb = malloc(sizeof(double) * nr);
    double *wc = malloc(sizeof(double) * nr), *wx = malloc(sizeof(double) * nr);
    if (!d || !e || !lam || !wa || !wb || !wc || !wx) { free(d); free(e); free(lam); free(wa); free(wb); free(wc); free(wx); return -1; }
    const double h = 1.0 / nr;
    for (int m = 0; m < M; m++) {
        /* T (flux form, multiplied by rho_i), then S = W^-1/2 T W^-1/2 */
        for (int i = 0; i < nr; i++) {
            const double rho = (i + 0.5) * h, rin = i * h, rout = (i + 1) * h;
            double diag = -rin / (h * h) - (double)m * m / rho;
            if (i < nr - 1) diag -= rout / (h * h);          /* outer wall: no flux */
            d[i] = diag / rho;
            if (i < nr - 1) e[i] = (rout / (h * h)) / sqrt(rho * ((i + 1.5) * h));
        }
        tridiag_eig(nr, d, e, lam, G + (size_t)m * nr * nr, wa, wb, wc, wx);
        for (int n = 0; n < nr; n++) {
            /* eigenvalues are -kappa^2, ascending: n = 0 is the most negative.
             * reorder so that n counts up from the lowest kappa */
            double l = lam[nr - 1 - n];
            kappa[(size_t)m * nr + n] = (float)sqrt(l < 0 ? -l : 0.0);
        }
        /* reverse the column order to match, and flush the exponentially small tails of
         * localised eigenvectors to exact zeros: denormals cost a hundred times a multiply */
        float *Gm = G + (size_t)m * nr * nr;
        for (int i = 0; i < nr; i++)
            for (int k = 0; k < nr / 2; k++) {
                float t = Gm[(size_t)i * nr + k];
                Gm[(size_t)i * nr + k] = Gm[(size_t)i * nr + nr - 1 - k];
                Gm[(size_t)i * nr + nr - 1 - k] = t;
            }
        for (size_t q = 0; q < (size_t)nr * nr; q++) if (fabsf(Gm[q]) < 1e-20f) Gm[q] = 0.0f;
    }
    free(d); free(e); free(lam); free(wa); free(wb); free(wc); free(wx);
    return 0;
}

/* ---- transforms between the polar field (nr rows of nt) and the mode arrays ---- */

/* modes (2 planes x M x nr) -> field */
void disk_inverse(const wave *w, const float *modes, float *field)
{
    const int nr = w->nr, nt = w->nt, M = w->M;
    float *sre = w->spec_re, *sim = w->spec_im;      /* nr x M */
    for (int m = 0; m < M; m++) {
        const float *Gm = w->G + (size_t)m * nr * nr;
        const float *c0 = modes + (size_t)m * nr, *c1 = modes + (size_t)M * nr + (size_t)m * nr;
        for (int i = 0; i < nr; i++) {
            const float *g = Gm + (size_t)i * nr;
            v4 s0 = { 0, 0, 0, 0 }, s1 = { 0, 0, 0, 0 }, t0 = { 0, 0, 0, 0 }, t1 = { 0, 0, 0, 0 };
            for (int n = 0; n < nr; n += 8) {
                const v4 g0 = ld4(g + n), g1 = ld4(g + n + 4);
                s0 += g0 * ld4(c0 + n); t0 += g1 * ld4(c0 + n + 4);
                s1 += g0 * ld4(c1 + n); t1 += g1 * ld4(c1 + n + 4);
            }
            sre[(size_t)i * M + m] = hsum4(s0 + t0) * w->isq_rho[i];
            sim[(size_t)i * M + m] = hsum4(s1 + t1) * w->isq_rho[i];
        }
    }
    float *re = w->fre, *im = w->fim;
    const float scale = 1.0f / (float)nt;
    for (int i = 0; i < nr; i++) {
        const float *r = sre + (size_t)i * M, *q = sim + (size_t)i * M;
        re[0] = r[0]; im[0] = 0.0f;
        for (int m = 1; m < nt / 2; m++) {
            re[m] = r[m]; im[m] = q[m];
            re[nt - m] = r[m]; im[nt - m] = -q[m];
        }
        re[nt / 2] = r[nt / 2]; im[nt / 2] = 0.0f;
        dct_fft(&w->pt, re, im, 1);
        float *row = field + (size_t)i * nt;
        for (int j = 0; j < nt; j++) row[j] = re[j] * scale;
    }
}

/* c += g * a (over one radial row), two planes at once */
static inline void axpy2(float *c0, float *c1, const float *g, float a, float b, int nr)
{
    const v4 va = { a, a, a, a }, vb = { b, b, b, b };
    for (int n = 0; n < nr; n += 4) {
        const v4 gv = ld4(g + n);
        st4(c0 + n, ld4(c0 + n) + gv * va);
        st4(c1 + n, ld4(c1 + n) + gv * vb);
    }
}

/* field -> modes, accumulated (modes += transform) */
void disk_forward_add(const wave *w, const float *field, float *modes)
{
    const int nr = w->nr, nt = w->nt, M = w->M;
    float *sre = w->spec_re, *sim = w->spec_im;
    float *re = w->fre, *im = w->fim;
    for (int i = 0; i < nr; i++) {
        const float *row = field + (size_t)i * nt;
        for (int j = 0; j < nt; j++) { re[j] = row[j]; im[j] = 0.0f; }
        dct_fft(&w->pt, re, im, 0);
        const float sq = w->sq_rho[i];
        for (int m = 0; m < M; m++) { sre[(size_t)i * M + m] = re[m] * sq; sim[(size_t)i * M + m] = im[m] * sq; }
    }
    for (int m = 0; m < M; m++) {
        const float *Gm = w->G + (size_t)m * nr * nr;
        float *c0 = modes + (size_t)m * nr, *c1 = modes + (size_t)M * nr + (size_t)m * nr;
        for (int i = 0; i < nr; i++)
            axpy2(c0, c1, Gm + (size_t)i * nr, sre[(size_t)i * M + m], sim[(size_t)i * M + m], nr);
    }
}

/* Separable source f(rho) * g(theta): the theta part is a short FFT, and only the
 * angular modes it actually contains need the dense radial product.  Adds the
 * transform to `modes` (nothing goes through the real-space buffers). */
void disk_add_separable(const wave *w, const float *f_r, const float *g_th, float *modes)
{
    const int nr = w->nr, nt = w->nt, M = w->M;
    float *re = w->fre, *im = w->fim;
    for (int j = 0; j < nt; j++) { re[j] = g_th[j]; im[j] = 0.0f; }
    dct_fft(&w->pt, re, im, 0);
    float mx = 0;
    for (int m = 0; m < M; m++) { float a = fabsf(re[m]) + fabsf(im[m]); if (a > mx) mx = a; }
    for (int m = 0; m < M; m++) {
        const float a = re[m], b = im[m];
        if (fabsf(a) + fabsf(b) < 1e-5f * mx) continue;
        const float *Gm = w->G + (size_t)m * nr * nr;
        float *c0 = modes + (size_t)m * nr, *c1 = modes + (size_t)M * nr + (size_t)m * nr;
        for (int i = 0; i < nr; i++) {
            const float fi = f_r[i] * w->sq_rho[i];
            if (fi == 0.0f) continue;
            axpy2(c0, c1, Gm + (size_t)i * nr, fi * a, fi * b, nr);
        }
    }
}
