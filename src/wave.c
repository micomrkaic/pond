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

double wave_k_of_omega(const wave *w, double omega)
{
    if (omega <= 0) return 0;
    /* omega(k) rises monotonically from 0, so bracket by doubling and bisect */
    double hi = 1e-4;
    while (wave_omega(w, hi) < omega && hi < 1e9) hi *= 2.0;
    double lo = hi / 2.0;
    for (int it = 0; it < 60; it++) {
        const double mid = 0.5 * (lo + hi);
        if (wave_omega(w, mid) < omega) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

static void build_dispersion(wave *w)
{
    if (w->shape == WAVE_DISK) {
        const int nr = w->nr, M = w->M;
        for (int plane = 0; plane < 2; plane++)
            for (int m = 0; m < M; m++)
                for (int n = 0; n < nr; n++) {
                    const double k = w->kappa[(size_t)m * nr + n] / w->R;
                    const size_t idx = (size_t)plane * M * nr + (size_t)m * nr + n;
                    w->kmag[idx]  = (float)k;
                    w->omega[idx] = (float)wave_omega(w, k);
                    w->gamma[idx] = (float)(2.0 * w->nu * k * k + w->gamma0);
                }
        w->dt_rotor = -1.0;
        return;
    }
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
    const size_t N = (size_t)w->nmodes;
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
    const size_t N = (size_t)w->nmodes;
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

static wave *alloc_common(int nx, int ny, int nmodes)
{
    wave *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->nx = nx; w->ny = ny; w->nmodes = nmodes;
    w->g = 9.81; w->sigma = 0.072; w->rho = 1000.0; w->nu = 1.0e-6;
    w->gamma0 = 0.03;
    w->rng = 0x9E3779B97F4A7C15ULL;
    w->dt_rotor = -1.0;

    const size_t N = (size_t)nx * ny, NM = (size_t)nmodes;
    w->A = calloc(NM, sizeof(float));   w->B = calloc(NM, sizeof(float));
    w->omega = malloc(NM * sizeof(float)); w->gamma = malloc(NM * sizeof(float));
    w->kmag = malloc(NM * sizeof(float));
    w->Rr = malloc(NM * sizeof(float)); w->Ri = malloc(NM * sizeof(float));
    w->eta = calloc(N, sizeof(float));
    w->src_d = calloc(N, sizeof(float)); w->src_v = calloc(N, sizeof(float));
    w->tmp = malloc((size_t)(nx > ny ? nx : ny) * sizeof(float));
    int ok = 1;
    for (int p = 2; p <= WAVE_MAXPOW; p++) {
        w->Rpr[p] = malloc(NM * sizeof(float)); w->Rpi[p] = malloc(NM * sizeof(float));
        if (!w->Rpr[p] || !w->Rpi[p]) ok = 0;
    }
    w->bz_idx = malloc(NM * sizeof(int)); w->bz_w = malloc(NM * sizeof(float));
    w->bz_k0 = -1.0;
    if (!ok || !w->bz_idx || !w->bz_w ||
        !w->A || !w->B || !w->omega || !w->gamma || !w->kmag || !w->Rr || !w->Ri ||
        !w->eta || !w->src_d || !w->src_v || !w->tmp) {
        wave_destroy(w);
        return NULL;
    }
    return w;
}

wave *wave_create(int nx, int ny, double Lx, double Ly, double depth)
{
    wave *w = alloc_common(nx, ny, nx * ny);
    if (!w) return NULL;
    w->shape = WAVE_RECT;
    if (dct_plan_init(&w->px, nx) || dct_plan_init(&w->py, ny)) { wave_destroy(w); return NULL; }
    wave_set_pool(w, Lx, Ly, depth);
    return w;
}

wave *wave_create_disk(int nt, int nr, double D, double depth)
{
    const int M = nt / 2 + 1;
    wave *w = alloc_common(nt, nr, 2 * M * nr);
    if (!w) return NULL;
    w->shape = WAVE_DISK;
    w->nt = nt; w->nr = nr; w->M = M;
    w->G = malloc((size_t)M * nr * nr * sizeof(float));
    w->kappa = malloc((size_t)M * nr * sizeof(float));
    w->sq_rho = malloc((size_t)nr * sizeof(float)); w->isq_rho = malloc((size_t)nr * sizeof(float));
    w->spec_re = malloc((size_t)nr * M * sizeof(float)); w->spec_im = malloc((size_t)nr * M * sizeof(float));
    w->fre = malloc((size_t)nt * sizeof(float)); w->fim = malloc((size_t)nt * sizeof(float));
    w->tmpm = malloc((size_t)w->nmodes * sizeof(float));
    w->ncut = malloc((size_t)M * sizeof(int));
    if (!w->G || !w->kappa || !w->sq_rho || !w->isq_rho || !w->spec_re || !w->spec_im || !w->fre || !w->fim || !w->tmpm || !w->ncut ||
        dct_plan_init(&w->pt, nt) || disk_basis_build(nr, M, w->G, w->kappa)) {
        wave_destroy(w);
        return NULL;
    }
    for (int i = 0; i < nr; i++) {
        const double rho = (i + 0.5) / nr;
        w->sq_rho[i] = (float)sqrt(rho); w->isq_rho[i] = (float)(1.0 / sqrt(rho));
    }
    /* modes beyond the radial Nyquist of the unit disk are artefacts of the tiny inner
     * cells (their eigenvectors live on one or two rings); they are never propagated */
    for (int m = 0; m < M; m++) {
        int n = 0;
        while (n < nr && w->kappa[(size_t)m * nr + n] <= (float)(M_PI * nr)) n++;
        w->ncut[m] = n;
    }
    wave_set_pool(w, D, D, depth);
    return w;
}

void wave_destroy(wave *w)
{
    if (!w) return;
    free(w->A); free(w->B); free(w->omega); free(w->gamma); free(w->kmag);
    free(w->Rr); free(w->Ri); free(w->eta); free(w->src_d); free(w->src_v); free(w->tmp);
    for (int p = 2; p <= WAVE_MAXPOW; p++) { free(w->Rpr[p]); free(w->Rpi[p]); }
    free(w->bz_idx); free(w->bz_w);
    free(w->G); free(w->kappa); free(w->sq_rho); free(w->isq_rho);
    free(w->spec_re); free(w->spec_im); free(w->fre); free(w->fim); free(w->tmpm); free(w->ncut);
    if (w->px.n) dct_plan_free(&w->px);
    if (w->py.n) dct_plan_free(&w->py);
    if (w->pt.n) dct_plan_free(&w->pt);
    free(w);
}

void wave_set_pool(wave *w, double Lx, double Ly, double depth)
{
    if (w->shape == WAVE_DISK) Ly = Lx;           /* diameter */
    w->Lx = Lx; w->Ly = Ly;
    w->dx = Lx / w->nx;
    w->dy = Ly / w->ny;
    if (w->shape == WAVE_DISK) {
        w->R = 0.5 * Lx;
        w->dr = w->R / w->nr;
        w->dth = 2.0 * M_PI / w->nt;
    }
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
    const size_t N = (size_t)w->nx * w->ny, NM = (size_t)w->nmodes;
    memset(w->A, 0, NM * sizeof(float));
    memset(w->B, 0, NM * sizeof(float));
    memset(w->src_d, 0, N * sizeof(float));
    memset(w->src_v, 0, N * sizeof(float));
    w->dirty_d = w->dirty_v = 0;
    w->t = 0;
}

static void flush_sources(wave *w)
{
    const size_t N = (size_t)w->nx * w->ny, NM = (size_t)w->nmodes;
    if (w->dirty_d) {
        if (w->shape == WAVE_DISK) disk_forward_add(w, w->src_d, w->A);
        else {
            dct2_forward(&w->px, &w->py, w->src_d, w->tmp);
            for (size_t i = 0; i < NM; i++) w->A[i] += w->src_d[i];
        }
        memset(w->src_d, 0, N * sizeof(float));
        w->dirty_d = 0;
    }
    if (w->dirty_v) {
        if (w->shape == WAVE_DISK) {
            /* transform into a scratch mode array, then scale by 1/omega */
            memset(w->tmpm, 0, NM * sizeof(float));
            disk_forward_add(w, w->src_v, w->tmpm);
            for (size_t i = 0; i < NM; i++) if (w->omega[i] > 0.0f) w->B[i] += w->tmpm[i] / w->omega[i];
        } else {
            dct2_forward(&w->px, &w->py, w->src_v, w->tmp);
            for (size_t i = 1; i < NM; i++)
                if (w->omega[i] > 0.0f) w->B[i] += w->src_v[i] / w->omega[i];
        }
        memset(w->src_v, 0, N * sizeof(float));
        w->dirty_v = 0;
    }
    /* the pool does not change its mean level: pin the k = 0 mode(s) */
    for (size_t i = 0; i < NM; i++) if (w->omega[i] <= 0.0f) w->A[i] = w->B[i] = 0.0f;
    if (w->shape == WAVE_DISK) {
        for (int plane = 0; plane < 2; plane++)
            for (int m = 0; m < w->M; m++) {
                const size_t off = (size_t)plane * w->M * w->nr + (size_t)m * w->nr;
                const int nc = w->ncut[m];
                if (nc < w->nr) {
                    memset(w->A + off + nc, 0, (size_t)(w->nr - nc) * sizeof(float));
                    memset(w->B + off + nc, 0, (size_t)(w->nr - nc) * sizeof(float));
                }
            }
    }
}

void wave_step(wave *w, double dt, int nsub)
{
    flush_sources(w);
    if (nsub <= 0) return;
    if (w->dt_rotor != dt) build_rotor(w, dt);

    const size_t N = (size_t)w->nmodes;
    float *restrict A = w->A, *restrict B = w->B;
    int left = nsub;
    while (left > 0) {
        const int p = left > WAVE_MAXPOW ? WAVE_MAXPOW : left;
        ensure_rotor_pow(w, p);
        const float *restrict Rr = (p == 1) ? w->Rr : w->Rpr[p];
        const float *restrict Ri = (p == 1) ? w->Ri : w->Rpi[p];
        for (size_t i = 0; i < N; i++) {
            const float a = A[i], b = B[i], rr = Rr[i], ri = Ri[i];
            float na = a * rr - b * ri, nb = a * ri + b * rr;
            /* modes that have decayed to nothing become exact zeros: denormal floats
             * would otherwise cost a hundred times a normal multiply in the transforms */
            A[i] = (fabsf(na) < 1e-30f) ? 0.0f : na;
            B[i] = (fabsf(nb) < 1e-30f) ? 0.0f : nb;
        }
        left -= p;
    }
    w->t += nsub * dt;
}

void wave_realize(wave *w)
{
    if (w->shape == WAVE_DISK) { disk_inverse(w, w->A, w->eta); return; }
    const size_t N = (size_t)w->nx * w->ny;
    memcpy(w->eta, w->A, N * sizeof(float));
    dct2_inverse(&w->px, &w->py, w->eta, w->tmp);
}

void wave_add_drop(wave *w, double x, double y, double s, double amp)
{
    const double reach = 5.0 * s, dx = w->dx, dy = w->dy;
    if (w->shape == WAVE_DISK) {
        /* (x, y) in the bounding square; stamp onto the rings that can reach it */
        const double px = x - w->R, py = y - w->R, rc = sqrt(px * px + py * py);
        const double inv2s2 = 1.0 / (2.0 * s * s);
        int i0 = (int)floor((rc - reach) / w->dr), i1 = (int)ceil((rc + reach) / w->dr);
        if (i0 < 0) i0 = 0;
        if (i1 > w->nr - 1) i1 = w->nr - 1;
        for (int i = i0; i <= i1; i++) {
            const double r = (i + 0.5) * w->dr;
            for (int j = 0; j < w->nt; j++) {
                const double th = j * w->dth;
                const double xx = r * cos(th) - px, yy = r * sin(th) - py;
                const double q = (xx * xx + yy * yy) * inv2s2;
                if (q < 12.0)
                    w->src_d[(size_t)j + (size_t)w->nt * i] += (float)(amp * (1.0 - q) * exp(-q));
            }
        }
        w->dirty_d = 1;
        return;
    }
    int i0 = (int)floor((x - reach) / dx), i1 = (int)ceil((x + reach) / dx);
    int j0 = (int)floor((y - reach) / dy), j1 = (int)ceil((y + reach) / dy);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > w->nx - 1) i1 = w->nx - 1;
    if (j1 > w->ny - 1) j1 = w->ny - 1;
    if (i0 > i1 || j0 > j1) return;
    const double inv2s2 = 1.0 / (2.0 * s * s);
    for (int j = j0; j <= j1; j++) {
        const double yy = (j + 0.5) * dy - y;
        for (int i = i0; i <= i1; i++) {
            const double xx = (i + 0.5) * dx - x;
            const double q = (xx * xx + yy * yy) * inv2s2;
            if (q < 12.0)
                w->src_d[(size_t)i + (size_t)w->nx * j] += (float)(amp * (1.0 - q) * exp(-q));
        }
    }
    w->dirty_d = 1;
}

void wave_add_paddle(wave *w, int wall, double pos, double span, double width, double accel, double dt)
{
    const double imp = accel * dt;
    if (span > 1.0) span = 1.0;
    if (span < 0.01) span = 0.01;
    const int full = span >= 1.0;

    if (w->shape == WAVE_DISK) {
        /* A strip inside the rim, over a sector centred at pos turns.  Separable in
         * (r, theta), so it goes straight into mode space.  span = 1 forces the whole
         * rim at once and makes rings that converge on the centre. */
        float *fr = malloc(sizeof(float) * (size_t)w->nr), *gt = malloc(sizeof(float) * (size_t)w->nt);
        if (!fr || !gt) { free(fr); free(gt); return; }
        double wd = width < 2.0 * w->dr ? 2.0 * w->dr : width;
        for (int i = 0; i < w->nr; i++) {
            const double x = (w->R - (i + 0.5) * w->dr) / wd;
            fr[i] = x > 6.0 ? 0.0f : (float)(imp * exp(-x * x));
        }
        const double th0 = 2.0 * M_PI * pos, sig = M_PI * span;
        for (int j = 0; j < w->nt; j++) {
            if (full) { gt[j] = 1.0f; continue; }
            double th = j * w->dth - th0;
            while (th >  M_PI) th -= 2.0 * M_PI;
            while (th < -M_PI) th += 2.0 * M_PI;
            const double a = th / sig;
            gt[j] = (a > -2.0 && a < 2.0) ? (float)exp(-a * a) : 0.0f;
        }
        memset(w->tmpm, 0, (size_t)w->nmodes * sizeof(float));
        disk_add_separable(w, fr, gt, w->tmpm);
        for (int i = 0; i < w->nmodes; i++) if (w->omega[i] > 0.0f) w->B[i] += w->tmpm[i] / w->omega[i];
        free(fr); free(gt);
        return;
    }

    /* Rectangle.  p is the axis across the strip (the one the wall is normal to),
     * q the axis along it.  The forcing is f(p) * g(q), and the 2-D DCT-II of a
     * separable function is the outer product of the two 1-D transforms. */
    if (wall < 0 || wall > 3) wall = 0;
    const int along_y = wall < 2;               /* walls 0,1 are x = const: the strip runs along y */
    const int np = along_y ? w->nx : w->ny, nq = along_y ? w->ny : w->nx;
    const double dp = along_y ? w->dx : w->dy, dq = along_y ? w->dy : w->dx;
    const double Lp = along_y ? w->Lx : w->Ly, Lq = along_y ? w->Ly : w->Lx;
    const dct_plan *pp = along_y ? &w->px : &w->py, *pq = along_y ? &w->py : &w->px;
    const int far = (wall & 1);                 /* the far wall: x = Lx or y = Ly */

    double wd = width < 2.0 * dp ? 2.0 * dp : width;
    float *f = w->tmp;
    for (int i = 0; i < np; i++) {
        const double c = (i + 0.5) * dp;
        const double u = (far ? Lp - c : c) / wd;
        f[i] = u < 6.0 ? (float)(imp * exp(-u * u)) : 0.0f;
    }
    dct_forward(pp, f, f);

    if (full) {
        /* g == 1, whose DCT is nq at q = 0 and nothing else: only the plane-wave
         * modes are excited, and this costs the one transform it always did */
        const float cq = (float)nq;
        for (int m = 1; m < np; m++) {
            const size_t idx = along_y ? (size_t)m : (size_t)w->nx * m;
            if (w->omega[idx] > 0.0f) w->B[idx] += f[m] * cq / w->omega[idx];
        }
        return;
    }

    float *g = malloc((size_t)nq * sizeof(float));
    if (!g) return;
    const double c0 = pos * Lq, sig = 0.5 * span * Lq;
    for (int j = 0; j < nq; j++) {
        const double v = ((j + 0.5) * dq - c0) / sig;
        g[j] = (v > -6.0 && v < 6.0) ? (float)exp(-v * v) : 0.0f;
    }
    dct_forward(pq, g, g);
    float gmax = 0.0f;
    for (int n = 0; n < nq; n++) { const float a = fabsf(g[n]); if (a > gmax) gmax = a; }
    const float gmin = 1e-6f * gmax;
    for (int n = 0; n < nq; n++) {
        const float gn = g[n];
        if (fabsf(gn) < gmin) continue;
        for (int m = 0; m < np; m++) {
            const size_t idx = along_y ? (size_t)m + (size_t)w->nx * n : (size_t)n + (size_t)w->nx * m;
            if (w->omega[idx] > 0.0f) w->B[idx] += f[m] * gn / w->omega[idx];
        }
    }
    free(g);
}

void wave_breeze(wave *w, double k0, double amp, double dt)
{
    const size_t N = (size_t)w->nx * w->ny;
    if (w->bz_k0 != k0 && w->shape == WAVE_DISK) {
        int n = 0;
        double W = 0;
        for (int idx = 0; idx < w->nmodes; idx++) {
            const int plane = idx / (w->M * w->nr), m = (idx / w->nr) % w->M;
            if (plane == 1 && (m == 0 || m == w->nt / 2)) continue;
            const double k = w->kmag[idx];
            if (k <= 0 || k > 6.0 * k0) continue;
            double wt;
            if (k < k0) { const double d = (k - k0) / (0.35 * k0); wt = exp(-d * d); }
            else        { wt = (k0 / k) * (k0 / k); }
            wt *= 0.5;    /* the rectangle's cos^2 spread averages to 1/2 */
            if (wt < 1e-3) continue;
            w->bz_idx[n] = idx; w->bz_w[n] = (float)wt; n++;
            W += wt * wt;
        }
        w->bz_n = n; w->bz_k0 = k0;
        /* eta_rms = (2 / (nt sqrt(nr))) sqrt(sum A^2) on the disk (Parseval in theta,
         * orthonormal radial basis in the rho-weighted inner product) */
        w->bz_norm = W > 0 ? (float)(0.5 * w->nt * sqrt((double)w->nr) / sqrt(W)) : 0.0f;
    } else if (w->bz_k0 != k0) {
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
    const size_t N = (size_t)w->nmodes;
    double s = 0;
    for (size_t i = 0; i < N; i++) s += (double)w->A[i] * w->A[i] + (double)w->B[i] * w->B[i];
    return s;
}

double wave_rms_slope(const wave *w)
{
    const int nx = w->nx, ny = w->ny;
    const float *e = w->eta;
    double s = 0;
    if (w->shape == WAVE_DISK) {
        /* polar: d/dr between rings, (1/r) d/dtheta around the ring */
        for (int i = 0; i < ny - 1; i++) {
            const double r = (i + 0.5) * w->dr;
            for (int j = 0; j < nx; j++) {
                const int jp = (j + 1) % nx;
                const double er = (e[j + nx * (i + 1)] - e[j + nx * i]) / w->dr;
                const double et = (e[jp + nx * i] - e[j + nx * i]) / (r * w->dth);
                s += er * er + et * et;
            }
        }
        return sqrt(s / ((double)(ny - 1) * nx));
    }
    for (int j = 0; j < ny - 1; j++)
        for (int i = 0; i < nx - 1; i++) {
            const double ex = (e[i + 1 + nx * j] - e[i + nx * j]) / w->dx;
            const double ey = (e[i + nx * (j + 1)] - e[i + nx * j]) / w->dy;
            s += ex * ex + ey * ey;
        }
    return sqrt(s / ((double)(nx - 1) * (ny - 1)));
}
