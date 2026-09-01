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

/* hos.h — High-Order Spectral nonlinearity (West et al. 1987, Dommermuth & Yue
 * 1987) as a correction on top of the exact linear propagation.
 *
 * The full potential-flow surface equations in Zakharov's variables eta and
 * psi = phi(x, eta) are
 *   eta_t = W (1 + |grad eta|^2) - grad psi . grad eta
 *   psi_t = -g eta - 1/2 |grad psi|^2 + 1/2 W^2 (1 + |grad eta|^2) + capillary
 * with W = phi_z at the surface, obtained from psi by expanding the
 * Dirichlet-to-Neumann map in powers of eta to order M.  The linear parts of
 * both equations are exactly what the rotor in wave.c integrates; this module
 * evaluates the remainder pseudo-spectrally and applies it with a Heun step
 * in the middle of the frame's linear sub-steps (Strang splitting).
 *
 * Only the lowest nc x nc modes take part (the long, energetic waves, where
 * nonlinearity shows); products are formed on a 2nc x 2nc grid, which
 * dealiases the quadratic terms exactly.  The capillary term is kept linear.
 * Rectangular basins only: a rigid wall is a mirror symmetry of the full
 * equations too, so the even-extended field the cosine basis represents is
 * consistent under the nonlinear products.
 */
#ifndef POND_HOS_H
#define POND_HOS_H

#include "wave.h"

typedef struct hos hos;

/* nc: coarse modes per axis (power of two, <= grid); order: 2 or 3 */
hos   *hos_create(const wave *w, int nc, int order);
void   hos_destroy(hos *h);
/* Apply the nonlinear correction over dt to the wave's coarse modes.
 * Returns 1 if applied, 0 if skipped (steepness beyond the expansion's reach). */
int    hos_step(hos *h, wave *w, double dt);
double hos_steepness(const hos *h);   /* max |grad eta| seen in the last step */
int    hos_nc(const hos *h);
int    hos_order(const hos *h);

#endif
