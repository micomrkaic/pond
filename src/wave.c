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

#include "wave.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- small xorshift64* RNG, Box-Muller for Gaussians ---- */
static inline uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static inline double rng_uniform(uint64_t *s)      /* [0,1) */
{
    return (double)(rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}
static inline double rng_gauss(uint64_t *s)
{
    double u1 = 1.0 - rng_uniform(s), u2 = rng_uniform(s);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

double wave_omega(const wave *w, double k)
{
    if (k <= 0) return 0;
    return sqrt((w->g * k + w->sigma * k * k * k / w->rho) * tanh(k * w->depth));
}

static void build_dispersion(wave *w)
{
    const int nx = w->nx, ny = w->ny;
    for (int n = 0; n < ny; n++) {
        const double ky = M_PI * n / w->Ly;
        for (int m = 0; m < nx; m++) {
            const double kx = M_PI * m / w->Lx;
            const double k = sqrt(kx * kx + ky * ky);
            const size_t idx = (size_t)m + (size_t)nx * n;
            w->kmag[idx]  = (float)k;
            w->omega[idx] = (float)wave_omega(w, k);
            w->gamma[idx] = (float)(2.0 * w->nu * k * k + w->gamma0);
        }
    }
    w->dt_rotor = -1.0;
}

static void build_rotor(wave *w, double dt)
{
    const size_t N = (size_t)w->nx * w->ny;
    for (size_t i = 0; i < N; i++) {
        const double d = exp(-(double)w->gamma[i] * dt);
        const double th = (double)w->omega[i] * dt;
        w->Rr[i] = (float)(d * cos(th));
        w->Ri[i] = (float)(-d * sin(th));
    }
    w->dt_rotor = dt;
    w->rotor_pow_valid = 1;   /* only R^1 */
}

/* R^p = R^{p-1} * R, computed lazily; exact up to float rounding */
static void ensure_rotor_pow(wave *w, int p)
{
    const size_t N = (size_t)w->nx * w->ny;
    while (w->rotor_pow_valid < p) {
        const int q = w->rotor_pow_valid + 1;
        const float *pr = (q == 2) ? w->Rr : w->Rpr[q - 1];
        const float *pi = (q == 2) ? w->Ri : w->Rpi[q - 1];
        float *qr = w->Rpr[q], *qi = w->Rpi[q];
        for (size_t i = 0; i < N; i++) {
            const float ar = pr[i], ai = pi[i], br = w->Rr[i], bi = w->Ri[i];
            qr[i] = ar * br - ai * bi;
            qi[i] = ar * bi + ai * br;
        }
        w->rotor_pow_valid = q;
    }
}

wave *wave_create(int nx, int ny, double L, double depth)
{
    wave *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->nx = nx; w->ny = ny;
    w->g = 9.81; w->sigma = 0.072; w->rho = 1000.0; w->nu = 1.0e-6;
    w->gamma0 = 0.03;
    w->rng = 0x9E3779B97F4A7C15ULL;
    w->dt_rotor = -1.0;

    const size_t N = (size_t)nx * ny;
    w->A = calloc(N, sizeof(float));   w->B = calloc(N, sizeof(float));
    w->omega = malloc(N * sizeof(float)); w->gamma = malloc(N * sizeof(float));
    w->kmag = malloc(N * sizeof(float));
    w->Rr = malloc(N * sizeof(float)); w->Ri = malloc(N * sizeof(float));
    w->eta = calloc(N, sizeof(float));
    w->src_d = calloc(N, sizeof(float)); w->src_v = calloc(N, sizeof(float));
    w->tmp = malloc((size_t)(nx > ny ? nx : ny) * sizeof(float));
    int ok = 1;
    for (int p = 2; p <= WAVE_MAXPOW; p++) {
        w->Rpr[p] = malloc(N * sizeof(float)); w->Rpi[p] = malloc(N * sizeof(float));
        if (!w->Rpr[p] || !w->Rpi[p]) ok = 0;
    }
    w->bz_idx = malloc(N * sizeof(int)); w->bz_w = malloc(N * sizeof(float));
    w->bz_k0 = -1.0;
    if (!ok || !w->bz_idx || !w->bz_w ||
        !w->A || !w->B || !w->omega || !w->gamma || !w->kmag || !w->Rr || !w->Ri ||
        !w->eta || !w->src_d || !w->src_v || !w->tmp ||
        dct_plan_init(&w->px, nx) || dct_plan_init(&w->py, ny)) {
        wave_destroy(w);
        return NULL;
    }
    wave_set_pool(w, L, depth);
    return w;
}

void wave_destroy(wave *w)
{
    if (!w) return;
    free(w->A); free(w->B); free(w->omega); free(w->gamma); free(w->kmag);
    free(w->Rr); free(w->Ri); free(w->eta); free(w->src_d); free(w->src_v); free(w->tmp);
    for (int p = 2; p <= WAVE_MAXPOW; p++) { free(w->Rpr[p]); free(w->Rpi[p]); }
    free(w->bz_idx); free(w->bz_w);
    dct_plan_free(&w->px); dct_plan_free(&w->py);
    free(w);
}

void wave_set_pool(wave *w, double L, double depth)
{
    w->Lx = L;
    w->dx = L / w->nx;
    w->Ly = w->dx * w->ny;
    w->depth = depth;
    build_dispersion(w);
    w->bz_k0 = -1.0;      /* breeze band cache depends on the pool */
}

void wave_set_damping(wave *w, double gamma0)
{
    w->gamma0 = gamma0;
    build_dispersion(w);
}

void wave_clear(wave *w)
{
    const size_t N = (size_t)w->nx * w->ny;
    memset(w->A, 0, N * sizeof(float));
    memset(w->B, 0, N * sizeof(float));
    memset(w->src_d, 0, N * sizeof(float));
    memset(w->src_v, 0, N * sizeof(float));
    w->dirty_d = w->dirty_v = 0;
    w->t = 0;
}

static void flush_sources(wave *w)
{
    const size_t N = (size_t)w->nx * w->ny;
    if (w->dirty_d) {
        dct2_forward(&w->px, &w->py, w->src_d, w->tmp);
        for (size_t i = 0; i < N; i++) w->A[i] += w->src_d[i];
        memset(w->src_d, 0, N * sizeof(float));
        w->dirty_d = 0;
    }
    if (w->dirty_v) {
        dct2_forward(&w->px, &w->py, w->src_v, w->tmp);
        for (size_t i = 1; i < N; i++)
            if (w->omega[i] > 0.0f) w->B[i] += w->src_v[i] / w->omega[i];
        memset(w->src_v, 0, N * sizeof(float));
        w->dirty_v = 0;
    }
    w->A[0] = w->B[0] = 0.0f;   /* the pool does not change its mean level */
}

void wave_step(wave *w, double dt, int nsub)
{
    flush_sources(w);
    if (nsub <= 0) return;
    if (w->dt_rotor != dt) build_rotor(w, dt);

    const size_t N = (size_t)w->nx * w->ny;
    float *restrict A = w->A, *restrict B = w->B;
    int left = nsub;
    while (left > 0) {
        const int p = left > WAVE_MAXPOW ? WAVE_MAXPOW : left;
        ensure_rotor_pow(w, p);
        const float *restrict Rr = (p == 1) ? w->Rr : w->Rpr[p];
        const float *restrict Ri = (p == 1) ? w->Ri : w->Rpi[p];
        for (size_t i = 0; i < N; i++) {
            const float a = A[i], b = B[i], rr = Rr[i], ri = Ri[i];
            A[i] = a * rr - b * ri;
            B[i] = a * ri + b * rr;
        }
        left -= p;
    }
    w->t += nsub * dt;
}

void wave_realize(wave *w)
{
    const size_t N = (size_t)w->nx * w->ny;
    memcpy(w->eta, w->A, N * sizeof(float));
    dct2_inverse(&w->px, &w->py, w->eta, w->tmp);
}

void wave_add_drop(wave *w, double x, double y, double s, double amp)
{
    const double reach = 5.0 * s, dx = w->dx;
    int i0 = (int)floor((x - reach) / dx), i1 = (int)ceil((x + reach) / dx);
    int j0 = (int)floor((y - reach) / dx), j1 = (int)ceil((y + reach) / dx);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > w->nx - 1) i1 = w->nx - 1;
    if (j1 > w->ny - 1) j1 = w->ny - 1;
    if (i0 > i1 || j0 > j1) return;
    const double inv2s2 = 1.0 / (2.0 * s * s);
    for (int j = j0; j <= j1; j++) {
        const double yy = (j + 0.5) * dx - y;
        for (int i = i0; i <= i1; i++) {
            const double xx = (i + 0.5) * dx - x;
            const double q = (xx * xx + yy * yy) * inv2s2;
            if (q < 12.0)
                w->src_d[(size_t)i + (size_t)w->nx * j] += (float)(amp * (1.0 - q) * exp(-q));
        }
    }
    w->dirty_d = 1;
}

void wave_add_paddle(wave *w, double width, double accel, double dt)
{
    /* The forcing is profile(x) * 1(y): its 2-D DCT is dct_x(profile) (x) dct_y(1),
     * and dct_y of a constant is ny * c at n = 0 only.  So the paddle excites
     * only the plane-wave modes (m, 0) and needs one 1-D transform, no buffer. */
    const int nx = w->nx;
    const double dx = w->dx, imp = accel * dt;
    float *row = w->tmp;
    for (int i = 0; i < nx; i++) {
        const double x = (i + 0.5) * dx / width;
        row[i] = x < 6.0 ? (float)(imp * exp(-x * x)) : 0.0f;
    }
    dct_forward(&w->px, row, row);
    const float ny = (float)w->ny;
    for (int m = 1; m < nx; m++)
        if (w->omega[m] > 0.0f) w->B[m] += row[m] * ny / w->omega[m];
}

void wave_breeze(wave *w, double k0, double amp, double dt)
{
    const size_t N = (size_t)w->nx * w->ny;
    if (w->bz_k0 != k0) {
        /* rebuild the band: indices, spectral weights, and the normalisation that
         * turns a per-mode kick into an rms surface elevation.
         * eta = (4/N^2) sum A c(x), <c^2> = 1/4  =>  eta_rms = (2/N^2) sqrt(sum A^2) */
        int n = 0;
        double W = 0;
        for (int nn = 0; nn < w->ny; nn++) {
            const double ky = M_PI * nn / w->Ly;
            for (int m = 0; m < w->nx; m++) {
                const double kx = M_PI * m / w->Lx;
                const double k = sqrt(kx * kx + ky * ky);
                if (k <= 0 || k > 6.0 * k0) continue;
                double wt;
                if (k < k0) { const double d = (k - k0) / (0.35 * k0); wt = exp(-d * d); }
                else        { wt = (k0 / k) * (k0 / k); }
                wt *= (kx * kx) / (k * k);              /* cos^2 spread about the x axis */
                if (wt < 1e-3) continue;
                w->bz_idx[n] = (int)((size_t)m + (size_t)w->nx * nn);
                w->bz_w[n] = (float)wt;
                n++;
                W += wt * wt;
            }
        }
        w->bz_n = n; w->bz_k0 = k0;
        w->bz_norm = W > 0 ? (float)(0.5 * (double)N / sqrt(W)) : 0.0f;
    }
    const float sq = (float)(amp * sqrt(dt)) * w->bz_norm;
    for (int j = 0; j < w->bz_n; j++)
        w->A[w->bz_idx[j]] += sq * w->bz_w[j] * (float)rng_gauss(&w->rng);
}

void wave_set_mode(wave *w, int m, int n, float a, float b)
{
    const size_t idx = (size_t)m + (size_t)w->nx * n;
    w->A[idx] = a; w->B[idx] = b;
}

double wave_norm(const wave *w)
{
    const size_t N = (size_t)w->nx * w->ny;
    double s = 0;
    for (size_t i = 0; i < N; i++) s += (double)w->A[i] * w->A[i] + (double)w->B[i] * w->B[i];
    return s;
}

double wave_rms_slope(const wave *w)
{
    const int nx = w->nx, ny = w->ny;
    const float *e = w->eta;
    double s = 0;
    for (int j = 0; j < ny - 1; j++)
        for (int i = 0; i < nx - 1; i++) {
            const double ex = (e[i + 1 + nx * j] - e[i + nx * j]) / w->dx;
            const double ey = (e[i + nx * (j + 1)] - e[i + nx * j]) / w->dx;
            s += ex * ex + ey * ey;
        }
    return sqrt(s / ((double)(nx - 1) * (ny - 1)));
}
