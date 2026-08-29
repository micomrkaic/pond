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

/* wave.h — linear free-surface water waves in a rectangular basin with
 * vertical walls, solved in the cosine-mode basis and advanced exactly in time.
 *
 * Physics (Airy / linear potential flow, finite depth, surface tension):
 *   omega^2(k) = (g k + sigma k^3 / rho) tanh(k h)
 *   amplitude decay rate gamma(k) = 2 nu k^2 + gamma0        (Lamb, sec. 349)
 *
 * Basin modes are cos(pi m x / Lx) cos(pi n y / Ly), i.e. exactly the DCT-II
 * basis with samples at cell centres.  Each mode (m,n) is an independent
 * damped oscillator; the state is (A, B) = (eta_hat, eta_hat_t / omega), and
 * propagation by dt is the rotation
 *   (A + iB) <- (A + iB) * exp(-gamma dt) * exp(-i omega dt)
 * which is exact for any dt: no CFL, no stability limit, no numerical
 * dispersion.  Real-space sources are accumulated in a buffer and injected
 * with one forward DCT; rendering needs one inverse DCT per frame.
 */
#ifndef POND_WAVE_H
#define POND_WAVE_H

#include "dct.h"
#include <stdint.h>

typedef struct {
    int nx, ny;
    double Lx, Ly, dx;         /* basin size [m], cell size [m] (square cells) */
    double depth;              /* [m] */
    double g, sigma, rho, nu;  /* 9.81, 0.072, 1000, 1e-6 for water */
    double gamma0;             /* extra uniform damping [1/s] (bottom/wall boundary layers, dirt) */
    double t;                  /* simulated time [s] */

    float *A, *B;              /* mode coefficients, index m + nx*n */
    float *omega, *gamma, *kmag;
    float *Rr, *Ri;            /* rotor for dt_rotor */
    double dt_rotor;           /* < 0: rotor invalid */

    float *eta;                /* real-space surface, valid after wave_realize() */
    float *src_d, *src_v;      /* real-space displacement / velocity source buffers */
    int dirty_d, dirty_v;

    /* cached rotor powers R^p, p = 1..WAVE_MAXPOW, so a frame of p substeps is one pass */
    #define WAVE_MAXPOW 8
    float *Rpr[WAVE_MAXPOW + 1], *Rpi[WAVE_MAXPOW + 1];
    int rotor_pow_valid;

    /* cached breeze band: mode indices and weights */
    int *bz_idx; float *bz_w; int bz_n; double bz_k0; float bz_norm;

    dct_plan px, py;
    float *tmp;
    uint64_t rng;
} wave;

wave *wave_create(int nx, int ny, double L, double depth);
void  wave_destroy(wave *w);

void  wave_set_pool(wave *w, double L, double depth);   /* rebuilds dispersion tables; state kept */
void  wave_set_damping(wave *w, double gamma0);
void  wave_clear(wave *w);

/* Inject pending sources, then advance nsub substeps of dt (nsub may be 0). */
void  wave_step(wave *w, double dt, int nsub);
/* eta <- inverse transform of A */
void  wave_realize(wave *w);

/* Sources (x, y in metres from the top-left corner; y grows downward on screen).
 * Drop: volume-conserving crater  amp * (1 - q) e^{-q},  q = r^2 / (2 s^2).
 * amp < 0 gives a crater with a raised rim. */
void  wave_add_drop(wave *w, double x, double y, double s, double amp);
/* Plane wavemaker along the x = 0 wall: velocity forcing accel * exp(-(x/width)^2),
 * applied as an impulse over dt.  Works in mode space directly (only the (m,0)
 * modes are involved), so it costs one 1-D transform. */
void  wave_add_paddle(wave *w, double width, double accel, double dt);
/* Stochastic wind forcing.  Modes are kicked with amplitude weights
 *   w(k) = (k/k0)^-2 above the peak k0 (a k^-4 elevation spectrum),
 *          Gaussian roll-off below it, times a cos^2 directional spread
 *          favouring waves that travel along x (wind along the x axis).
 * amp is the surface-elevation rms added per sqrt(second); steady state under
 * uniform damping gamma0 is roughly amp / sqrt(2 gamma0). */
void  wave_breeze(wave *w, double k0, double amp, double dt);
/* Set a single mode directly (tests). */
void  wave_set_mode(wave *w, int m, int n, float a, float b);

double wave_omega(const wave *w, double k);
double wave_norm(const wave *w);    /* sum A^2 + B^2 over modes */
/* RMS surface slope of the realized field, a good dimensionless "how rough" number. */
double wave_rms_slope(const wave *w);

#endif
