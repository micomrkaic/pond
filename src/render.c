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

#include "render.h"
#include <math.h>
#include <string.h>

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float fract(float v) { return v - floorf(v); }

static inline uint32_t hash2(int a, int b)
{
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA77u;
    h ^= h >> 15; h *= 0xC2B2AE3Du; h ^= h >> 13;
    return h;
}

void render_defaults(render_params *rp)
{
    memset(rp, 0, sizeof *rp);
    rp->view = 0;
    rp->floor_style = 0;
    rp->gain = 1.0f;
    rp->ior = 1.333f;
    float sx = 0.22f, sy = -0.18f, sz = 0.96f;
    float n = 1.0f / sqrtf(sx * sx + sy * sy + sz * sz);
    rp->sun[0] = sx * n; rp->sun[1] = sy * n; rp->sun[2] = sz * n;
}

/* Floor albedo at physical position (px, py). tile is the tile pitch in metres. */
static inline void floor_color(int style, float px, float py, float tile, float grain, float *rgb)
{
    switch (style) {
    case 0: {   /* pale pool tiles with grout */
        float u = px / tile, v = py / tile;
        float fu = fract(u), fv = fract(v);
        const float gw = 0.045f;
        if (fu < gw || fv < gw) { rgb[0] = 0.40f; rgb[1] = 0.44f; rgb[2] = 0.47f; return; }
        float b = 0.94f + 0.06f * (float)(hash2((int)floorf(u), (int)floorf(v)) & 255) / 255.0f;
        rgb[0] = 0.72f * b; rgb[1] = 0.80f * b; rgb[2] = 0.80f * b;
        return;
    }
    case 1: {   /* checkerboard */
        int c = ((int)floorf(px / tile) + (int)floorf(py / tile)) & 1;
        if (c) { rgb[0] = 0.88f; rgb[1] = 0.87f; rgb[2] = 0.80f; }
        else   { rgb[0] = 0.20f; rgb[1] = 0.26f; rgb[2] = 0.34f; }
        return;
    }
    default: {  /* sand */
        float b = 0.92f + 0.08f * (float)(hash2((int)floorf(px / grain), (int)floorf(py / grain)) & 255) / 255.0f;
        rgb[0] = 0.80f * b; rgb[1] = 0.72f * b; rgb[2] = 0.54f * b;
        return;
    }
    }
}

static void render_height(const wave *w, const render_params *rp, uint32_t *pix)
{
    const int nx = w->nx, ny = w->ny;
    const float *e = w->eta;
    float mx = 1e-12f;
    for (int i = 0; i < nx * ny; i++) { float a = fabsf(e[i]); if (a > mx) mx = a; }
    const float s = rp->gain / mx;
    for (int i = 0; i < nx * ny; i++) {
        float v = clampf(e[i] * s, -1.0f, 1.0f);
        float r, g, b;
        if (v < 0) { v = -v; r = 0.04f + 0.10f * v; g = 0.05f + 0.35f * v; b = 0.10f + 0.85f * v; }
        else       { r = 0.04f + 0.94f * v; g = 0.05f + 0.85f * v; b = 0.10f + 0.65f * v; }
        pix[i] = 0xFF000000u | ((uint32_t)(r * 255.0f) << 16) | ((uint32_t)(g * 255.0f) << 8) | (uint32_t)(b * 255.0f);
    }
}

void render_frame(const wave *w, const render_params *rp, uint32_t *pix)
{
    if (rp->view == 1) { render_height(w, rp, pix); return; }

    const int nx = w->nx, ny = w->ny;
    const float *e = w->eta;
    const float dx = (float)w->dx, gain = rp->gain;
    const float inv2dx = gain / (2.0f * dx), invdx2 = gain / (dx * dx), inv4dx2 = gain / (4.0f * dx * dx);
    const float H = (float)w->depth;
    const float a = H * (1.0f - 1.0f / rp->ior);          /* paraxial refraction lever arm */
    const float tile = (float)w->Lx / 8.0f;
    const float grain = 1.5f * dx;

    /* absorption over the round trip to the floor: red goes first, then green */
    const float att[3] = { expf(-0.38f * 2.0f * H), expf(-0.060f * 2.0f * H), expf(-0.015f * 2.0f * H) };
    const float scatter[3] = { 0.00f, 0.24f, 0.40f };
    const float zenith[3] = { 0.36f, 0.58f, 0.88f }, horizon[3] = { 0.86f, 0.90f, 0.94f };
    const float suncol[3] = { 1.00f, 0.97f, 0.88f };
    const float R0 = 0.02f;
    const float exposure = 0.55f;
    const float caus_mix = expf(-H / 20.0f);      /* deep floors: caustics wash out */
    static float srgb[1025];
    if (srgb[1024] == 0.0f)
        for (int i = 0; i <= 1024; i++) srgb[i] = powf((float)i / 1024.0f, 1.0f / 2.2f);
    const float sx = rp->sun[0], sy = rp->sun[1], sz = rp->sun[2];
    /* a second, softer light from the other side (a lamp, or the bright part of the sky) */
    const float lx = -0.30f / 1.0535f, ly = 0.25f / 1.0535f, lz = 0.98f / 1.0535f;

    for (int j = 0; j < ny; j++) {
        const int jm = j > 0 ? j - 1 : 0, jp = j < ny - 1 ? j + 1 : ny - 1;
        const float *row = e + (size_t)j * nx, *rowm = e + (size_t)jm * nx, *rowp = e + (size_t)jp * nx;
        const float y = ((float)j + 0.5f) * dx;
        const float fy = (jp - jm) == 2 ? 1.0f : 2.0f;   /* one-sided at the wall */
        uint32_t *out = pix + (size_t)j * nx;
        for (int i = 0; i < nx; i++) {
            const int im = i > 0 ? i - 1 : 0, ip = i < nx - 1 ? i + 1 : nx - 1;
            const float fx = (ip - im) == 2 ? 1.0f : 2.0f;
            const float x = ((float)i + 0.5f) * dx;

            const float c = row[i];
            const float ex = (row[ip] - row[im]) * inv2dx * fx;
            const float ey = (rowp[i] - rowm[i]) * inv2dx * fy;
            const float exx = (row[ip] - 2.0f * c + row[im]) * invdx2;
            const float eyy = (rowp[i] - 2.0f * c + rowm[i]) * invdx2;
            const float exy = (rowp[ip] - rowp[im] - rowm[ip] + rowm[im]) * inv4dx2 * fx * fy;

            /* unit normal */
            const float inv = 1.0f / sqrtf(1.0f + ex * ex + ey * ey);
            const float nX = -ex * inv, nY = -ey * inv, nZ = inv;

            /* refracted ray hits the floor at (x,y) + a * grad(eta) */
            float fl[3];
            floor_color(rp->floor_style, x + a * ex, y + a * ey, tile, grain, fl);

            /* caustic: 1 / |det J| of the surface->floor map, J = I + a * Hess(eta) */
            float det = (1.0f + a * exx) * (1.0f + a * eyy) - a * a * exy * exy;
            det = fabsf(det);
            float caus = 1.0f / (det < 0.12f ? 0.12f : det);
            if (caus > 3.0f) caus = 3.0f;
            caus = 1.0f + (caus - 1.0f) * caus_mix;

            float under[3];
            for (int k = 0; k < 3; k++) under[k] = exposure * (fl[k] * caus * att[k] + scatter[k] * (1.0f - att[k]));

            /* Fresnel (Schlick) for a viewer straight above */
            float f1 = 1.0f - nZ;
            float f2 = f1 * f1;
            const float R = R0 + (1.0f - R0) * f2 * f2 * f1;

            /* reflected view ray and the sky it sees */
            const float rX = 2.0f * nZ * nX, rY = 2.0f * nZ * nY, rZ = 2.0f * nZ * nZ - 1.0f;
            const float t = clampf(2.0f * (1.0f - rZ), 0.0f, 1.0f);
            float sky[3];
            for (int k = 0; k < 3; k++) sky[k] = zenith[k] + (horizon[k] - zenith[k]) * t;

            /* sun glint (r . sun)^32 plus a broad fill light (r . lamp)^8 */
            float d = rX * sx + rY * sy + rZ * sz;
            if (d < 0.0f) d = 0.0f;
            d = d * d; d = d * d; d = d * d; d = d * d; d = d * d;   /* ^32 */
            float d2 = rX * lx + rY * ly + rZ * lz;
            if (d2 < 0.0f) d2 = 0.0f;
            d2 = d2 * d2; d2 = d2 * d2; d2 = d2 * d2;                 /* ^8 */
            const float glint = 0.55f * d + 0.12f * d2;

            uint32_t p = 0xFF000000u;
            for (int k = 0; k < 3; k++) {
                float v = (1.0f - R) * under[k] + R * 1.6f * sky[k] + glint * suncol[k];
                v = srgb[(int)(clampf(v, 0.0f, 1.0f) * 1024.0f)];
                p |= (uint32_t)(v * 255.0f + 0.5f) << (16 - 8 * k);
            }
            out[i] = p;
        }
    }
}
