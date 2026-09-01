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

#include "hos.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_STEEPNESS 0.45f    /* beyond this the expansion is not trusted; the step is skipped */

struct hos {
    int nc, np, order;
    int nx, ny;                 /* the wave's grid */
    double Lx_, Ly_;
    float scale_in, scale_out;  /* coefficient scale between the wave's grid and the padded grid */
    dct_plan px, py;            /* padded grid transforms (np x np) */
    float *tmp;
    /* per coarse mode (nc x nc, index m + nc*n) */
    float *kx, *ky, *k, *L, *om;
    float *filt;                /* smooth cutoff at the top of the coarse band */
    /* mode-space work (nc x nc) */
    float *eh, *ph, *eh0, *ph0, *k1e, *k1p, *k2e, *k2p, *phi2, *phi3, *cm;
    /* real-space work (np x np) */
    float *eta, *ex, *ey, *qx, *qy, *R1, *R2, *R3, *S1, *S2, *T1, *W, *Ne, *Np_, *pad;
    float steep;
};

static float *fa(size_t n) { return calloc(n, sizeof(float)); }

hos *hos_create(const wave *w, int nc, int order)
{
    if (w->shape != WAVE_RECT || nc < 8 || (nc & (nc - 1)) || nc > w->nx || nc > w->ny) return NULL;
    hos *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->nc = nc; h->np = 2 * nc; h->order = order < 2 ? 2 : (order > 3 ? 3 : order);
    h->nx = w->nx; h->ny = w->ny;
    const size_t NC = (size_t)nc * nc, NP = (size_t)h->np * h->np;
    h->scale_in  = (float)((double)NP / ((double)w->nx * w->ny));
    h->scale_out = 1.0f / h->scale_in;
    if (dct_plan_init(&h->px, h->np) || dct_plan_init(&h->py, h->np)) { hos_destroy(h); return NULL; }
    h->tmp = fa((size_t)h->np);
    h->kx = fa(NC); h->ky = fa(NC); h->k = fa(NC); h->L = fa(NC); h->om = fa(NC); h->filt = fa(NC);
    h->eh = fa(NC); h->ph = fa(NC); h->eh0 = fa(NC); h->ph0 = fa(NC);
    h->k1e = fa(NC); h->k1p = fa(NC); h->k2e = fa(NC); h->k2p = fa(NC);
    h->phi2 = fa(NC); h->phi3 = fa(NC); h->cm = fa(NC);
    h->eta = fa(NP); h->ex = fa(NP); h->ey = fa(NP); h->qx = fa(NP); h->qy = fa(NP);
    h->R1 = fa(NP); h->R2 = fa(NP); h->R3 = fa(NP); h->S1 = fa(NP); h->S2 = fa(NP); h->T1 = fa(NP);
    h->W = fa(NP); h->Ne = fa(NP); h->Np_ = fa(NP); h->pad = fa(NP);
    float **all[] = { &h->tmp, &h->kx, &h->ky, &h->k, &h->L, &h->om, &h->filt, &h->eh, &h->ph, &h->eh0, &h->ph0,
                      &h->k1e, &h->k1p, &h->k2e, &h->k2p, &h->phi2, &h->phi3, &h->cm, &h->eta, &h->ex, &h->ey,
                      &h->qx, &h->qy, &h->R1, &h->R2, &h->R3, &h->S1, &h->S2, &h->T1, &h->W, &h->Ne, &h->Np_, &h->pad };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (!*all[i]) { hos_destroy(h); return NULL; }
    /* mode tables follow the basin; refreshed on every step (cheap) */
    return h;
}

void hos_destroy(hos *h)
{
    if (!h) return;
    float *all[] = { h->tmp, h->kx, h->ky, h->k, h->L, h->om, h->filt, h->eh, h->ph, h->eh0, h->ph0,
                     h->k1e, h->k1p, h->k2e, h->k2p, h->phi2, h->phi3, h->cm, h->eta, h->ex, h->ey,
                     h->qx, h->qy, h->R1, h->R2, h->R3, h->S1, h->S2, h->T1, h->W, h->Ne, h->Np_, h->pad };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) free(all[i]);
    if (h->px.n) dct_plan_free(&h->px);
    if (h->py.n) dct_plan_free(&h->py);
    free(h);
}

static void tables(hos *h, const wave *w)
{
    const int nc = h->nc;
    for (int n = 0; n < nc; n++)
        for (int m = 0; m < nc; m++) {
            const size_t i = (size_t)m + (size_t)nc * n;
            const double kx = M_PI * m / w->Lx, ky = M_PI * n / w->Ly, k = sqrt(kx * kx + ky * ky);
            h->kx[i] = (float)kx; h->ky[i] = (float)ky; h->k[i] = (float)k;
            h->L[i] = (float)(k * tanh(k * w->depth));
            h->om[i] = (float)wave_omega(w, k);
            /* smooth cutoff over the top quarter of the band */
            const double r = (m > n ? m : n) / (double)nc;
            h->filt[i] = (float)(r < 0.75 ? 1.0 : exp(-16.0 * (r - 0.75) * (r - 0.75) / (0.25 * 0.25)));
        }
}

/* ---- transforms between the coarse coefficient block and the padded real grid ---- */

/* zero-pad the coarse block into the np x np array (wave-grid coefficient convention -> padded) */
static void spread(const hos *h, const float *c, float *pad)
{
    const int nc = h->nc, np = h->np;
    memset(pad, 0, sizeof(float) * (size_t)np * np);
    for (int n = 0; n < nc; n++)
        for (int m = 0; m < nc; m++) pad[m + np * n] = c[m + nc * n] * h->scale_in;
}

/* coefficients -> real field.  deriv: 0 value, 1 d/dx, 2 d/dy.
 * A derivative turns the cosine series into a sine series; on this grid
 * sin(pi m (2i+1)/2N) = (-1)^i cos(pi (N-m)(2i+1)/2N), so it is a DCT-III of
 * the reversed, k-weighted coefficients with an alternating sign. */
static void to_real(hos *h, const float *c, float *out, int deriv)
{
    const int np = h->np;
    float *pad = h->pad;
    spread(h, c, pad);
    if (deriv == 1) {
        for (int n = 0; n < np; n++) {
            float *row = pad + (size_t)np * n, *t = h->tmp;
            t[0] = 0.0f;
            for (int mp = 1; mp < np; mp++) { int m = np - mp; t[mp] = -(float)(M_PI * m / h->Lx_) * row[m]; }
            memcpy(row, t, sizeof(float) * (size_t)np);
        }
    } else if (deriv == 2) {
        for (int m = 0; m < np; m++) {
            float *t = h->tmp;
            t[0] = 0.0f;
            for (int npp = 1; npp < np; npp++) { int n = np - npp; t[npp] = -(float)(M_PI * n / h->Ly_) * pad[m + np * n]; }
            for (int n = 0; n < np; n++) pad[m + np * n] = t[n];
        }
    }
    dct2_inverse(&h->px, &h->py, pad, h->tmp);
    if (deriv == 1) {
        for (int n = 0; n < np; n++)
            for (int i = 1; i < np; i += 2) pad[i + np * n] = -pad[i + np * n];
    } else if (deriv == 2) {
        for (int n = 1; n < np; n += 2)
            for (int i = 0; i < np; i++) pad[i + np * n] = -pad[i + np * n];
    }
    memcpy(out, pad, sizeof(float) * (size_t)np * np);
}

/* real field -> coarse coefficients (padded -> wave-grid convention), high modes dropped */
static void to_modes(hos *h, const float *f, float *c)
{
    const int nc = h->nc, np = h->np;
    memcpy(h->pad, f, sizeof(float) * (size_t)np * np);
    dct2_forward(&h->px, &h->py, h->pad, h->tmp);
    for (int n = 0; n < nc; n++)
        for (int m = 0; m < nc; m++) c[m + nc * n] = h->pad[m + np * n] * h->scale_out;
}

/* vertical derivative of order j applied to surface-potential coefficients:
 * a mode cosh k(z+h)/cosh kh has d^j/dz^j at z = 0 equal to k^j tanh kh (j odd), k^j (j even) */
static void dz(const hos *h, const float *c, float *out, int j)
{
    const size_t NC = (size_t)h->nc * h->nc;
    for (size_t i = 0; i < NC; i++) {
        float k = h->k[i], f = 1.0f;
        for (int q = 0; q < j; q++) f *= k;
        if (j & 1) f *= (k > 0.0f ? h->L[i] / k : 0.0f);
        out[i] = c[i] * f;
    }
}

/* Nonlinear right-hand sides (the linear parts are handled by the rotor). */
static void rhs(hos *h, const float *eh, const float *ph, float *ke, float *kp)
{
    const int np = h->np;
    const size_t NP = (size_t)np * np, NC = (size_t)h->nc * h->nc;
    float *cm = h->cm;

    to_real(h, eh, h->eta, 0);
    to_real(h, eh, h->ex, 1);
    to_real(h, eh, h->ey, 2);
    to_real(h, ph, h->qx, 1);
    to_real(h, ph, h->qy, 2);
    dz(h, ph, cm, 1); to_real(h, cm, h->R1, 0);
    dz(h, ph, cm, 2); to_real(h, cm, h->R2, 0);
    if (h->order >= 3) { dz(h, ph, cm, 3); to_real(h, cm, h->R3, 0); }

    float steep = 0;
    for (size_t i = 0; i < NP; i++) { float s2 = h->ex[i] * h->ex[i] + h->ey[i] * h->ey[i]; if (s2 > steep) steep = s2; }
    h->steep = sqrtf(steep);

    /* phi^(2) = -eta * dz phi^(1) */
    for (size_t i = 0; i < NP; i++) h->W[i] = -h->eta[i] * h->R1[i];
    to_modes(h, h->W, h->phi2);
    dz(h, h->phi2, cm, 1); to_real(h, cm, h->S1, 0);
    if (h->order >= 3) {
        dz(h, h->phi2, cm, 2); to_real(h, cm, h->S2, 0);
        /* phi^(3) = -eta dz phi^(2) - eta^2/2 dz^2 phi^(1) */
        for (size_t i = 0; i < NP; i++) h->W[i] = -h->eta[i] * h->S1[i] - 0.5f * h->eta[i] * h->eta[i] * h->R2[i];
        to_modes(h, h->W, h->phi3);
        dz(h, h->phi3, cm, 1); to_real(h, cm, h->T1, 0);
    }
    /* W = sum over orders of the vertical velocity at the surface */
    for (size_t i = 0; i < NP; i++) {
        const float e = h->eta[i];
        float Wv = h->R1[i] + h->S1[i] + e * h->R2[i];
        if (h->order >= 3) Wv += h->T1[i] + e * h->S2[i] + 0.5f * e * e * h->R3[i];
        h->W[i] = Wv;
    }
    for (size_t i = 0; i < NP; i++) {
        const float g2 = 1.0f + h->ex[i] * h->ex[i] + h->ey[i] * h->ey[i];
        const float Wv = h->W[i];
        h->Ne[i]  = Wv * g2 - (h->qx[i] * h->ex[i] + h->qy[i] * h->ey[i]) - h->R1[i];
        h->Np_[i] = -0.5f * (h->qx[i] * h->qx[i] + h->qy[i] * h->qy[i]) + 0.5f * Wv * Wv * g2;
    }
    to_modes(h, h->Ne, ke);
    to_modes(h, h->Np_, kp);
    for (size_t i = 0; i < NC; i++) { ke[i] *= h->filt[i]; kp[i] *= h->filt[i]; }
    ke[0] = kp[0] = 0.0f;
}

int hos_step(hos *h, wave *w, double dt)
{
    if (w->shape != WAVE_RECT || w->nx != h->nx || w->ny != h->ny) return 0;
    h->Lx_ = w->Lx; h->Ly_ = w->Ly;
    tables(h, w);
    const int nc = h->nc, nx = w->nx;
    const size_t NC = (size_t)nc * nc;

    /* coarse block of the wave state -> (eta_hat, psi_hat) */
    for (int n = 0; n < nc; n++)
        for (int m = 0; m < nc; m++) {
            const size_t i = (size_t)m + (size_t)nc * n, wi = (size_t)m + (size_t)nx * n;
            h->eh[i] = w->A[wi];
            h->ph[i] = (h->L[i] > 0.0f) ? w->B[wi] * h->om[i] / h->L[i] : 0.0f;
        }
    memcpy(h->eh0, h->eh, sizeof(float) * NC);
    memcpy(h->ph0, h->ph, sizeof(float) * NC);

    /* Heun */
    rhs(h, h->eh0, h->ph0, h->k1e, h->k1p);
    if (h->steep > MAX_STEEPNESS || !(h->steep == h->steep)) return 0;
    const float fdt = (float)dt;
    for (size_t i = 0; i < NC; i++) { h->eh[i] = h->eh0[i] + fdt * h->k1e[i]; h->ph[i] = h->ph0[i] + fdt * h->k1p[i]; }
    rhs(h, h->eh, h->ph, h->k2e, h->k2p);
    if (h->steep > MAX_STEEPNESS || !(h->steep == h->steep)) return 0;
    for (size_t i = 0; i < NC; i++) {
        h->eh[i] = h->eh0[i] + 0.5f * fdt * (h->k1e[i] + h->k2e[i]);
        h->ph[i] = h->ph0[i] + 0.5f * fdt * (h->k1p[i] + h->k2p[i]);
    }
    /* back into the wave state */
    for (int n = 0; n < nc; n++)
        for (int m = 0; m < nc; m++) {
            const size_t i = (size_t)m + (size_t)nc * n, wi = (size_t)m + (size_t)nx * n;
            if (h->L[i] <= 0.0f || h->om[i] <= 0.0f) continue;
            w->A[wi] = h->eh[i];
            w->B[wi] = h->ph[i] * h->L[i] / h->om[i];
        }
    return 1;
}

double hos_steepness(const hos *h) { return h->steep; }
int hos_nc(const hos *h) { return h->nc; }
int hos_order(const hos *h) { return h->order; }
