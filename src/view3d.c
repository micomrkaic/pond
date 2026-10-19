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

#include "view3d.h"
#include "gl.h"
#include "text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ GL loader */
#ifndef __EMSCRIPTEN__
#define GL_DEF(ret, name, args) ret (*name) args = NULL;
GL_FUNCTIONS(GL_DEF)
#undef GL_DEF

int gl_load(void)
{
    int missing = 0;
    /* memcpy rather than a cast: ISO C has no object-to-function pointer conversion */
#define GL_LOAD(ret, name, args) \
    { void *fp = SDL_GL_GetProcAddress(#name); memcpy(&name, &fp, sizeof fp); \
      if (!name) { fprintf(stderr, "GL: missing %s\n", #name); missing++; } }
    GL_FUNCTIONS(GL_LOAD)
#undef GL_LOAD
    return missing ? -1 : 0;
}
#endif

/* ------------------------------------------------------------------ shaders */
#define GLSL(...) #__VA_ARGS__ "\n"

#ifdef __EMSCRIPTEN__
static const char *glsl_header = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
#else
static const char *glsl_header = "#version 330 core\n";
#endif

/* shared: sky, floor pattern, gamma */
static const char *glsl_common = GLSL(
    uniform vec3 u_sun;       /* unit vector towards the light: the sun by day, the moon by night */
    uniform float u_day;      /* 1 full day .. 0 night */
    uniform float u_dusk;     /* 1 with the sun on the horizon: the warm sky */
    uniform float u_night;    /* 1 when the light is the moon */
    uniform float u_lk;       /* how much light there is to see by: 1 noon, ~0.12 moonlight */
    uniform float u_time;
    uniform float u_sunny;    /* 1 with the house around the basin: a deeper sky, a bluer pool */
    uniform float u_nosun;    /* 1: the sky without the sun's disc (the reflection pass; the water adds it back) */
    uniform float u_video;    /* 1: the video look -- clouds, ripples */
    uniform float u_linear;   /* 1: write linear light for the camera pipeline, not display gamma */
    uniform sampler2D u_shadow;
    uniform mat4 u_lvp;       /* world -> the sun's shadow map */
    uniform float u_shadow_on;
    uniform float u_sbias;    /* depth bias, in shadow-map depth units */
    const vec3 ZENITH  = vec3(0.30, 0.52, 0.86);
    const vec3 HORIZON = vec3(0.80, 0.86, 0.92);
    const vec3 GROUND  = vec3(0.07, 0.075, 0.09);
    const vec3 SUNCOL  = vec3(1.00, 0.96, 0.86);
    const vec3 MOONCOL = vec3(0.80, 0.86, 1.00);
    const vec3 SCATTER = vec3(0.00, 0.22, 0.38);
    vec3 lit(vec3 c) { return c * u_lk; }
    float hash31(vec3 p) {
        p = fract(p * vec3(443.897, 441.423, 437.195));
        p += dot(p, p.yzx + 19.19);
        return fract((p.x + p.y) * p.z);
    }
    /* the sun's disc and glow by day, the moon's by night */
    vec3 sun_term(vec3 d) {
        float s = max(dot(d, u_sun), 0.0);
        if (u_night < 0.5) return SUNCOL * mix(vec3(1.0), vec3(1.0, 0.45, 0.20), u_dusk) * (5.0 * pow(s, 800.0) + 0.12 * pow(s, 12.0)) * max(u_day, 0.15);
        return MOONCOL * (2.0 * pow(s, 3000.0) + 0.025 * pow(s, 20.0));
    }
    /* value noise and its fractal sum, for the clouds */
    float vnoise(vec2 p) {
        vec2 i = floor(p), f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float a = hash31(vec3(i, 0.37)), b = hash31(vec3(i + vec2(1.0, 0.0), 0.37));
        float c = hash31(vec3(i + vec2(0.0, 1.0), 0.37)), e = hash31(vec3(i + vec2(1.0, 1.0), 0.37));
        return mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
    }
    float fbm(vec2 p) {
        float s = 0.0, a = 0.5;
        for (int k = 0; k < 5; k++) { s += a * vnoise(p); p = p * 2.03 + vec2(1.7, 9.2); a *= 0.5; }
        return s;
    }
    /* fair-weather cumulus on a layer overhead, drifting; lit white on top, grey
     * beneath, warm at dusk, faint under the moon */
    vec3 clouds(vec3 d, vec3 c) {
        if (d.y <= 0.0) return c;
        vec2 p = d.xz / (d.y + 0.08) * 0.45 + vec2(u_time * 0.006, u_time * 0.0025);
        float n = fbm(p);
        float amt = smoothstep(0.44, 0.70, n) * smoothstep(0.0, 0.12, d.y);
        float thick = smoothstep(0.55, 0.95, n);
        float s = max(dot(d, u_sun), 0.0);
        vec3 day = mix(vec3(1.00, 0.99, 0.97), vec3(0.62, 0.66, 0.72), thick * 0.8) * (0.85 + 0.35 * pow(s, 6.0));
        day = mix(day, day * vec3(1.00, 0.62, 0.45), u_dusk);
        vec3 col = day * u_day + vec3(0.035, 0.040, 0.055) * (1.0 - u_day);
        return mix(c, col, amt * 0.92);
    }
    vec3 sky_s(vec3 d, float withsun) {
        float h = clamp(d.y, 0.0, 1.0);
        /* the day sky, warming at the horizon as the sun goes down */
        vec3 hor0 = mix(HORIZON, vec3(0.66, 0.82, 0.96), u_sunny);
        vec3 zen0 = mix(ZENITH, vec3(0.10, 0.36, 0.86), u_sunny);
        vec3 hor = mix(hor0, vec3(1.00, 0.52, 0.28), 0.85 * u_dusk);
        vec3 zen = mix(zen0, vec3(0.22, 0.26, 0.52), 0.75 * u_dusk);
        vec3 c;
        if (d.y >= 0.0) c = mix(hor, zen, sqrt(h));
        else            c = mix(mix(hor * 0.45, GROUND, pow(clamp(-d.y, 0.0, 1.0), 0.4)), hor * 0.92, u_sunny);   /* the garden: haze to the ground */
        c *= u_day;
        /* the night sky, with stars */
        vec3 nz = vec3(0.012, 0.018, 0.045), nh = vec3(0.035, 0.040, 0.070);
        vec3 n = d.y >= 0.0 ? mix(nh, nz, sqrt(h)) : mix(mix(nh * 0.5, GROUND * 0.3, pow(clamp(-d.y, 0.0, 1.0), 0.4)), nh * 0.9, u_sunny);
        if (d.y > 0.02) {
            vec3 cell = floor(d * 90.0);
            float r = hash31(cell);
            if (r > 0.985) {
                vec3 cen = (cell + 0.5 + 0.6 * (vec3(hash31(cell + 1.7), hash31(cell + 3.1), hash31(cell + 5.3)) - 0.5)) / 90.0;
                float dd = length(d - normalize(cen)) * 90.0;
                n += vec3(0.9, 0.9, 1.0) * (r - 0.985) / 0.015 * smoothstep(0.35, 0.0, dd) * (0.7 + 0.3 * sin(u_time * (3.0 + 5.0 * r) + r * 40.0));
            }
        }
        c += n * (1.0 - u_day);
        if (u_nosun < 0.5 && withsun > 0.5) c += sun_term(d);
        if (u_video > 0.5) c = clouds(d, c);
        return c;
    }
    vec3 sky(vec3 d) { return sky_s(d, 1.0); }
    float hash21(vec2 p) {
        p = fract(p * vec2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }
    vec3 pattern(vec2 q, float tile, int style) {
        if (style == 0) {
            vec2 u = q / tile;
            vec2 f = fract(u);
            if (f.x < 0.045 || f.y < 0.045) return mix(vec3(0.40, 0.44, 0.47), vec3(0.78, 0.84, 0.88), u_sunny);
            float b = 0.94 + 0.06 * hash21(floor(u));
            return mix(vec3(0.72, 0.80, 0.80), vec3(0.40, 0.66, 0.90), u_sunny) * b;
        } else if (style == 1) {
            float c = mod(floor(q.x / tile) + floor(q.y / tile), 2.0);
            return c > 0.5 ? vec3(0.88, 0.87, 0.80) : vec3(0.20, 0.26, 0.34);
        } else {
            float b = 0.92 + 0.08 * hash21(floor(q / (tile * 0.02)));
            return vec3(0.80, 0.72, 0.54) * b;
        }
    }
    vec3 gam(vec3 c) { return u_linear > 0.5 ? max(c, vec3(0.0)) : pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
);

/* shared, second part (ISO C caps a string literal at 4095 characters): the
 * setting's shadows and materials */
static const char *glsl_common2 = GLSL(
    /* 1 lit .. 0 in the shadow of the house, the palms, the cabana: a 3 x 3 percentage-closer
     * lookup, the point pushed off its surface a little along the normal */
    float shadow_at(vec3 p, vec3 n) {
        if (u_shadow_on < 0.5) return 1.0;
        vec4 q = u_lvp * vec4(p + 0.05 * n, 1.0);
        vec3 s = q.xyz / q.w * 0.5 + 0.5;
        if (s.x <= 0.0 || s.x >= 1.0 || s.y <= 0.0 || s.y >= 1.0 || s.z >= 1.0) return 1.0;
        vec2 tx = 1.0 / vec2(textureSize(u_shadow, 0));
        float lit = 0.0;
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++)
                lit += (s.z - u_sbias > texture(u_shadow, s.xy + vec2(float(i), float(j)) * tx).r) ? 0.0 : 1.0;
        return lit / 9.0;
    }
    /* a point under the water: its sunlight came in along the refracted ray, steeper
     * than the sun's, so the test is made where that ray crossed the surface */
    float shadow_under(vec3 q) {
        vec3 Lr = refract(-u_sun, vec3(0.0, 1.0, 0.0), 1.0 / 1.333);
        if (Lr.y > -0.05) return shadow_at(q, vec3(0.0, 1.0, 0.0));
        return shadow_at(q - Lr * (q.y / Lr.y), vec3(0.0, 1.0, 0.0));
    }

    /* the house and garden: albedo by kind, at a point with a normal.  Kinds from
     * 10 up; the basin's own faces are 0 .. 3 */
    vec3 scene_albedo(float k, vec3 p, vec3 n) {
        if (k < 10.5) {                                   /* terrace: travertine slabs, 1.2 m */
            vec2 q = (abs(n.y) > 0.5 ? p.xz : (abs(n.x) > 0.5 ? p.zy : p.xy)) / 1.2;
            vec2 f = fract(q);
            float j = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y));
            float b = 0.93 + 0.07 * hash21(floor(q));
            return vec3(0.93, 0.88, 0.79) * b * (j < 0.012 ? 0.80 : 1.0);
        }
        if (k < 11.5) return vec3(0.97, 0.95, 0.90);      /* coping */
        if (k < 12.5) {                                   /* lawn, mown in stripes, with a slow mottle */
            float s = 0.95 + 0.05 * smoothstep(0.35, 0.65, abs(fract(p.x / 2.4) * 2.0 - 1.0));
            float m = 0.92 + 0.08 * sin(p.x * 0.31 + 1.3 * sin(p.z * 0.23)) * sin(p.z * 0.27 + 0.7);
            return vec3(0.22, 0.43, 0.15) * s * m;
        }
        if (k < 13.5) return vec3(0.92, 0.36, 0.31);      /* the colour-washed walls: salmon pink */
        if (k < 14.5) return vec3(0.96, 0.95, 0.92);      /* white render, fascia, soffit */
        if (k < 15.5) return vec3(0.60, 0.58, 0.54) * (0.88 + 0.12 * hash21(floor(p.xz * 6.0)));   /* roof gravel */
        if (k < 16.5) return vec3(0.0);                   /* glass: shaded on its own */
        if (k < 17.5) {                                   /* stacked stone */
            vec2 q = (abs(n.x) > 0.5 ? p.zy : p.xy) * vec2(2.4, 6.5);
            q.x += 0.5 * mod(floor(q.y), 2.0);
            vec2 f = fract(q);
            float b = 0.78 + 0.22 * hash21(floor(q));
            return vec3(0.74, 0.66, 0.55) * b * ((f.x < 0.05 || f.y < 0.08) ? 0.6 : 1.0);
        }
        if (k < 18.5) return vec3(0.52, 0.46, 0.38) * (0.82 + 0.18 * step(0.5, fract(p.y * 3.3)));   /* palm trunk, ringed */
        if (k < 19.5) return vec3(0.22, 0.44, 0.14);      /* fronds */
        if (k < 20.5) return vec3(0.17, 0.36, 0.14) * (0.85 + 0.15 * hash21(floor(p.xz * 5.0)));   /* shrubs */
        if (k < 21.5) return vec3(0.11, 0.11, 0.12);      /* dark steel: posts, frames */
        if (k < 22.5) return vec3(0.96, 0.95, 0.90);      /* loungers */
        return vec3(0.42, 0.50, 0.40);                    /* far hills */
    }
);

/* --- background sky: fullscreen triangle via gl_VertexID --- */
static const char *vs_bg = GLSL(
    out vec2 v_ndc;
    void main() {
        vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
        v_ndc = p;
        gl_Position = vec4(p, 0.999999, 1.0);
    }
);
static const char *fs_bg = GLSL(
    in vec2 v_ndc;
    uniform vec3 u_fwd; uniform vec3 u_right; uniform vec3 u_up;
    uniform float u_tanhalf; uniform float u_aspect;
    uniform float u_mirror;   /* 1: the sky as the water mirrors it */
    out vec4 o;
    void main() {
        vec3 d = normalize(u_fwd + u_tanhalf * (v_ndc.x * u_aspect * u_right + v_ndc.y * u_up));
        if (u_mirror > 0.5) d.y = -d.y;
        o = vec4(gam(sky(d)), 1.0);
    }
);

/* --- water surface --- */
static const char *vs_surf = GLSL(
    in vec2 a_uv;
    uniform sampler2D u_height;
    uniform ivec2 u_n;
    uniform vec2 u_L;
    uniform vec2 u_d;      /* cell size dx, dy */
    uniform float u_gain;
    uniform int u_shape;
    uniform mat4 u_vp;
    out vec3 v_pos;
    out vec3 v_nrm;
    float H(int i, int j) {
        i = clamp(i, 0, u_n.x - 1); j = clamp(j, 0, u_n.y - 1);
        return texelFetch(u_height, ivec2(i, j), 0).r * u_gain;
    }
    void main() {
        if (u_shape == 1) {
            /* polar grid: column j = angle, row i = ring; ring 0 is drawn at the centre,
             * ring nr-1 at the wall (half a cell of stretch), derivatives at the true radius */
            int j = int(a_uv.x), i = int(a_uv.y);
            int nt = u_n.x, nr = u_n.y;
            float R = 0.5 * u_L.x;
            float dr = R / float(nr), dth = 6.28318530718 / float(nt);
            float th = dth * float(j);
            float ct = cos(th), st = sin(th);
            float h, gx, gz;
            if (i == 0) {
                /* all of ring 0 is drawn at the centre: one height and one gradient for the
                 * whole fan, from opposite samples on ring 1 */
                int jo = (j + nt / 2) % nt, jq = (j + nt / 4) % nt, jr = (j + 3 * nt / 4) % nt;
                h = 0.5 * (H(j, 0) + H(jo, 0));
                float r1 = 1.5 * dr;
                float g1 = (H(j, 1) - H(jo, 1)) / (2.0 * r1);
                float g2 = (H(jq, 1) - H(jr, 1)) / (2.0 * r1);
                gx = g1 * ct - g2 * st; gz = g1 * st + g2 * ct;
            } else {
                h = H(j, i);
                int ip = min(i + 1, nr - 1);
                float fr = (ip - i + 1 == 2) ? 1.0 : 2.0;
                float hr = (H(j, ip) - H(j, i - 1)) * fr / (2.0 * dr);
                int jm = (j + nt - 1) % nt, jp = (j + 1) % nt;
                float rs = (float(i) + 0.5) * dr;
                float ht = (H(jp, i) - H(jm, i)) / (2.0 * dth * rs);
                gx = hr * ct - ht * st; gz = hr * st + ht * ct;
            }
            float rm = R * float(i) / float(nr - 1);
            v_pos = vec3(R + rm * ct, h, R + rm * st);
            v_nrm = normalize(vec3(-gx, 1.0, -gz));
            gl_Position = u_vp * vec4(v_pos, 1.0);
            return;
        }
        int i = int(a_uv.x), j = int(a_uv.y);
        float fx = (i == 0 || i == u_n.x - 1) ? 2.0 : 1.0;
        float fz = (j == 0 || j == u_n.y - 1) ? 2.0 : 1.0;
        float h  = H(i, j);
        float hx = (H(i + 1, j) - H(i - 1, j)) * fx / (2.0 * u_d.x);
        float hz = (H(i, j + 1) - H(i, j - 1)) * fz / (2.0 * u_d.y);
        /* cell centres stretched by half a cell so the mesh meets the wall planes exactly */
        v_pos = vec3(float(i) * u_L.x / float(u_n.x - 1), h, float(j) * u_L.y / float(u_n.y - 1));
        v_nrm = normalize(vec3(-hx, 1.0, -hz));
        gl_Position = u_vp * vec4(v_pos, 1.0);
    }
);
/* --- water surface: declarations, the wind's detail and glitter, then the shading --- */
static const char *fs_surf_head = GLSL(
    in vec3 v_pos;
    in vec3 v_nrm;
    uniform vec3 u_cam;
    uniform vec2 u_L;
    uniform float u_depth;
    uniform float u_tile;
    uniform int u_style;
    uniform int u_walls;      /* 0 tiled walls, 1 walls absent (glass or none) */
    uniform int u_floor;      /* 0 tiled floor, 1 floor absent */
    uniform int u_extend;     /* 1: the floor plane continues outside the basin */
    uniform int u_shape;      /* 0 box, 1 cylinder */
    uniform vec3 u_mu;
    uniform sampler2D u_light;
    uniform float u_lscale;
    uniform float u_refl;     /* 1: the setting is mirrored in the water */
    uniform sampler2D u_refl_tex;
    uniform mat4 u_vpref;     /* world -> the mirrored view the reflection was drawn in */
    uniform float u_rd;       /* how far off the reflected things are, metres: sets how far a tilt displaces them */
    uniform float u_wind;     /* the breeze's speed, m/s; 0 calm */
    uniform int u_ngust;
    uniform vec4 u_gust[8];   /* x, z, radius, strength */
    uniform float u_gbase, u_gnorm;
    out vec4 o;

);
static const char *glsl_water = GLSL(
    /* the gusts here: 1 on average over the basin, more under a cat's paw */
    float gust_at(vec2 q) {
        if (u_ngust == 0) return 1.0;
        float m = u_gbase;
        for (int i = 0; i < 8; i++) {
            if (i >= u_ngust) break;
            vec2 dd = q - u_gust[i].xy;
            float qq = dot(dd, dd) / (u_gust[i].z * u_gust[i].z);
            m += u_gust[i].w * exp(-qq);
        }
        return m * u_gnorm;
    }

    /* the small stuff a real pool is never without: eight short wave trains, 9 to 60 cm,
     * moving at their own phase speeds (gravity and surface tension), each fading out
     * with distance before it can alias.  Slopes, added to the simulated normal. */
    vec3 ripples(vec3 N, vec3 P) {
        if (u_video < 0.5 || max(u_L.x, u_L.y) < 2.0) return N;
        float dist = length(u_cam - P);
        float amp = u_wind > 0.0 ? 0.25 + min(0.9 * gust_at(P.xz) * sqrt(u_wind / 3.0), 2.8) : 0.35;
        vec2 g = vec2(0.0);
        for (int k = 0; k < 8; k++) {
            float fk = float(k);
            float lam = 0.09 * pow(1.31, fk);
            float a = 2.39996 * fk + 0.6;
            vec2 dir = vec2(cos(a), sin(a));
            float kw = 6.28318 / lam;
            float om = sqrt(9.81 * kw + 0.072 / 1000.0 * kw * kw * kw);
            float fade = smoothstep(260.0 * lam, 60.0 * lam, dist);
            g += dir * (0.010 * amp * fade) * cos(kw * dot(dir, P.xz) - om * u_time + 13.7 * fk);
        }
        return normalize(N + vec3(-g.x, 0.0, -g.y));
    }

    /* Sun glitter.  What the grid and the ripples cannot hold is a slope distribution:
     * Gaussian, isotropic, of mean square s2 (Cox and Munk: about 0.003 + 0.005 U for the
     * whole sea; here only the sub-grid part, rising with the wind and under the gusts,
     * plus what the pixel itself averages away).  The glint is the sun's irradiance times
     * the share of facets turned to send it to the eye; it twinkles, as a few large
     * facets do, with a heavy-tailed weight of mean one, fresh at 24 Hz. */
    vec3 glitter(vec3 N, vec3 V, vec3 P) {
        vec3 L = u_sun;
        if (L.y <= 0.0) return vec3(0.0);
        vec3 Hh = normalize(L + V);
        float cb = max(dot(Hh, N), 1e-3);
        float t2 = (1.0 - cb * cb) / (cb * cb);
        float s2 = (u_wind > 0.0 ? (0.0008 + 0.0011 * u_wind) * gust_at(P.xz) : 0.0004);
        vec3 dnx = dFdx(N), dny = dFdy(N);
        s2 += clamp(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.0, 0.08);
        float p = exp(-t2 / s2) / (3.14159 * s2);
        float vh = max(dot(V, Hh), 0.0);
        float F = 0.02 + 0.98 * pow(1.0 - vh, 5.0);
        float g = F * p / (4.0 * cb * cb * cb * cb * max(dot(N, V), 0.08));
        /* twinkle: cells about three pixels across, a fresh draw 24 times a second */
        float dist = length(u_cam - P);
        float cs = 0.0025 * dist + 0.004;
        vec2 id = floor(P.xz / cs);
        float fr = floor(u_time * 24.0 + 8.0 * hash31(vec3(id, 1.7)));
        float h = hash31(vec3(id, fr));
        float tw = 0.15 + 0.85 * 12.0 * pow(h, 11.0);        /* mean one; most cells dim, a few blazing */
        vec3 E = u_night < 0.5 ? SUNCOL * mix(vec3(1.0), vec3(1.0, 0.50, 0.25), u_dusk) * 2.5 * max(u_day, 0.15)
                               : MOONCOL * 0.25;
        return min(E * g * tw, vec3(30.0)) * shadow_at(P, vec3(0.0, 1.0, 0.0));
    }

);
static const char *fs_surf = GLSL(
    bool in_basin(vec2 q) {
        if (u_shape == 1) return length(q - 0.5 * u_L) <= 0.5 * u_L.x;
        return q.x >= 0.0 && q.x <= u_L.x && q.y >= 0.0 && q.y <= u_L.y;
    }

    /* colour seen along direction T from surface point P, inside the basin */
    vec3 inside(vec3 P, vec3 T) {
        /* distance to the walls and to the floor / surface planes */
        float tw = 1e30;
        if (u_shape == 1) {
            vec2 C = 0.5 * u_L; float R = 0.5 * u_L.x;
            vec2 pp = P.xz - C, dd = T.xz;
            float a = dot(dd, dd), b = dot(pp, dd), c = dot(pp, pp) - R * R;
            float disc = b * b - a * c;
            if (a > 1e-12 && disc > 0.0) tw = (-b + sqrt(disc)) / a;
        } else {
            if (T.x < 0.0) tw = min(tw, (0.0 - P.x) / T.x);
            if (T.x > 0.0) tw = min(tw, (u_L.x - P.x) / T.x);
            if (T.z < 0.0) tw = min(tw, (0.0 - P.z) / T.z);
            if (T.z > 0.0) tw = min(tw, (u_L.y - P.z) / T.z);
        }
        float tv = 1e30;
        if (T.y < 0.0) tv = (-u_depth - P.y) / T.y;
        if (T.y > 0.0) tv = (0.0 - P.y) / T.y;
        vec3 c;
        float path;
        if (u_extend == 1) {
            /* walls are not there: the ray leaves the water at the wall plane and
             * lands on the floor plane, which continues outside the basin */
            path = min(tw, tv);
            if (T.y >= 0.0) { c = sky(T); }
            else {
                vec3 Q = P + tv * T;
                vec3 col = pattern(Q.xz, u_tile, u_style);
                if (in_basin(Q.xz)) {
                    float lm = texture(u_light, Q.xz / u_L).r * u_lscale;
                    c = col * (0.30 + 0.70 * lm * shadow_under(Q));
                } else {
                    c = col * (0.35 + 0.65 * max(u_sun.y, 0.0));
                }
            }
        } else if (tw < tv) {
            path = tw;
            if (u_walls == 1) c = u_sunny > 0.5 ? vec3(0.16, 0.13, 0.10) : sky(T);   /* sunk in the terrace: earth */
            else {
                vec3 Q = P + tw * T;
                vec2 q;
                if (u_shape == 1) q = vec2(atan(Q.z - 0.5 * u_L.y, Q.x - 0.5 * u_L.x) * 0.5 * u_L.x, Q.y);
                else {
                    bool xwall = (T.x < 0.0 && Q.x < 1e-4 * u_L.x) || (T.x > 0.0 && Q.x > u_L.x - 1e-4 * u_L.x);
                    q = xwall ? vec2(Q.z, Q.y) : vec2(Q.x, Q.y);
                }
                c = pattern(q, u_tile, u_style) * 0.55;
            }
        } else {
            path = tv;
            if (T.y >= 0.0 || u_floor == 1) c = sky(T);
            else {
                vec3 Q = P + tv * T;
                float lm = texture(u_light, Q.xz / u_L).r * u_lscale;
                c = pattern(Q.xz, u_tile, u_style) * (0.30 + 0.70 * lm * shadow_under(Q));
            }
        }
        vec3 att = exp(-u_mu * max(path, 0.0));
        return lit(c * att + SCATTER * (1.0 - att));
    }

    void main() {
        vec3 N = ripples(normalize(v_nrm), v_pos);
        vec3 V = normalize(u_cam - v_pos);
        vec3 c;
        float R0 = 0.02;
        if (dot(N, V) >= 0.0) {
            float cv = max(dot(N, V), 0.0);
            float F = R0 + (1.0 - R0) * pow(1.0 - cv, 5.0);
            vec3 R = reflect(-V, N);
            vec3 T = refract(-V, N, 1.0 / 1.333);
            vec3 rc;
            bool glint = u_video > 0.5;
            if (u_refl > 0.5) {
                /* the mirrored scene, looked up where the reflected ray, u_rd metres out,
                 * lands in the mirrored view: flat water finds the image straight below
                 * the house, and every tilt of the surface shifts it as far as it should */
                vec3 Rr = vec3(R.x, max(R.y, 0.01), R.z);
                vec4 cq = u_vpref * vec4(v_pos + Rr * u_rd, 1.0);
                vec2 uv = clamp(cq.xy / max(cq.w, 1e-4) * 0.5 + 0.5, vec2(0.001), vec2(0.999));
                rc = pow(texture(u_refl_tex, uv).rgb, vec3(2.2));
                if (!glint) rc += sun_term(R) * shadow_at(v_pos, vec3(0.0, 1.0, 0.0));
            } else rc = sky_s(R, glint ? 0.0 : 1.0);
            c = mix(inside(v_pos, T), rc, F);
            if (glint) c += glitter(N, V, v_pos);
        } else {
            vec3 Nn = -N;
            float cv = max(dot(Nn, V), 0.0);
            float F = R0 + (1.0 - R0) * pow(1.0 - cv, 5.0);
            vec3 T = refract(-V, Nn, 1.333);
            vec3 R = reflect(-V, Nn);
            if (dot(T, T) < 1e-6) c = inside(v_pos, R);
            else c = mix(sky(T), inside(v_pos, R), F);
        }
        o = vec4(gam(c), 1.0);
    }
);

/* --- caustic pass: the surface mesh rasterised onto the floor after refracting the sun.
 * Vertices sit at the true cell centres; a padded mesh reaches past the walls with the
 * even (mirror) extension of the height field, for the no-wall mode. --- */
static const char *vs_caus = GLSL(
    in vec2 a_uv;
    uniform sampler2D u_height;   /* rectangle: the height field; disk: its Cartesian resample */
    uniform ivec2 u_n;
    uniform vec2 u_L;
    uniform vec2 u_d;
    uniform float u_gain;
    uniform float u_depth;
    uniform vec3 u_incident;      /* unit sunlight direction, downwards */
    uniform ivec2 u_ioff;         /* disk: the resampled texture starts u_ioff cells before the square */
    out vec2 v_src;
    int mir(int i, int n) { return i < 0 ? -1 - i : (i >= n ? 2 * n - 1 - i : i); }
    float Hr(int i, int j) { return texelFetch(u_height, ivec2(mir(i + u_ioff.x, u_n.x), mir(j + u_ioff.y, u_n.y)), 0).r * u_gain; }
    void main() {
        int i = int(a_uv.x), j = int(a_uv.y);
        float h  = Hr(i, j);
        float hx = (Hr(i + 1, j) - Hr(i - 1, j)) / (2.0 * u_d.x);
        float hz = (Hr(i, j + 1) - Hr(i, j - 1)) / (2.0 * u_d.y);
        vec3 P = vec3((float(i) + 0.5) * u_d.x, h, (float(j) + 0.5) * u_d.y);
        vec3 N = normalize(vec3(-hx, 1.0, -hz));
        vec3 T = refract(u_incident, N, 1.0 / 1.333);
        vec2 q = vec2(-1.0e4);
        if (dot(T, T) > 1.0e-6 && T.y < -1.0e-4) q = P.xz + T.xz * (-(u_depth + P.y) / T.y);
        v_src = P.xz;
        gl_Position = vec4(2.0 * q / u_L - 1.0, 0.0, 1.0);
    }
);
static const char *fs_caus = GLSL(
    in vec2 v_src;
    uniform vec2 u_lcell;         /* light-map texel size in metres */
    uniform vec2 u_L;
    uniform float u_mask_r;       /* > 0: only sources within this radius of the centre count (disk) */
    out vec4 o;
    void main() {
        if (u_mask_r > 0.0 && length(v_src - 0.5 * u_L) > u_mask_r) discard;
        /* irradiance relative to flat water = |d(source)/d(floor)|, summed over sheets by blending */
        vec2 dx = dFdx(v_src), dy = dFdy(v_src);
        float det = abs(dx.x * dy.y - dx.y * dy.x) / (u_lcell.x * u_lcell.y);
        o = vec4(min(det, 4.0), 0.0, 0.0, 1.0);
    }
);
/* disk: resample the polar height field (with mirrored rings past the rim) onto the
 * Cartesian light-map grid, so the caustic pass can rasterise texel-sized cells */
static const char *vs_resample = GLSL(
    in vec2 a_uv;                 /* (angle j, ring i); i = -1 is the centre vertex */
    uniform sampler2D u_height;
    uniform ivec2 u_n;
    uniform vec2 u_L;
    uniform float u_gain;
    uniform float u_ext;          /* the target covers [-u_ext, L + u_ext]^2 */
    out float v_h;
    int mir(int i, int n) { return i < 0 ? -1 - i : (i >= n ? 2 * n - 1 - i : i); }
    float Hd(int j, int i) {
        j = (j + u_n.x) % u_n.x;
        return texelFetch(u_height, ivec2(j, mir(i, u_n.y)), 0).r * u_gain;
    }
    void main() {
        int j = int(a_uv.x), i = int(a_uv.y);
        int nt = u_n.x, nr = u_n.y;
        float R = 0.5 * u_L.x, dr = R / float(nr), dth = 6.28318530718 / float(nt);
        vec2 q;
        if (i < 0) { q = vec2(R, R); v_h = 0.5 * (Hd(0, 0) + Hd(nt / 2, 0)); }
        else {
            float rs = (float(i) + 0.5) * dr, th = dth * float(j);
            q = vec2(R + rs * cos(th), R + rs * sin(th));
            v_h = Hd(j, i);
        }
        gl_Position = vec4(2.0 * (q + u_ext) / (u_L + 2.0 * u_ext) - 1.0, 0.0, 1.0);
    }
);
static const char *fs_resample = GLSL(
    in float v_h;
    out vec4 o;
    void main() { o = vec4(v_h, 0.0, 0.0, 1.0); }
);
/* light through glass walls: the strip a wall would shadow is lit flat */
static const char *fs_fill = GLSL(
    in vec2 v_uv;
    uniform vec2 u_L;
    uniform vec2 u_offset;        /* floor point = surface point + offset, flat water */
    uniform int u_shape;
    out vec4 o;
    void main() {
        vec2 q = v_uv * u_L;      /* v_uv: 0..1 over the light map, y up here */
        vec2 sp = q - u_offset;
        bool inb;
        if (u_shape == 1) inb = length(sp - 0.5 * u_L) <= 0.5 * u_L.x;
        else inb = sp.x >= 0.0 && sp.x <= u_L.x && sp.y >= 0.0 && sp.y <= u_L.y;
        o = vec4(inb ? 0.0 : 1.0, 0.0, 0.0, 1.0);
    }
);
/* 3x3 binomial blur of the light map (same softening the CPU splat applies) */
static const char *fs_blur = GLSL(
    in vec2 v_uv;
    uniform sampler2D u_tex;
    out vec4 o;
    void main() {
        ivec2 c = ivec2(gl_FragCoord.xy);
        ivec2 n = textureSize(u_tex, 0);
        float s = 0.0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                ivec2 q = clamp(c + ivec2(dx, dy), ivec2(0), n - 1);
                float w = float((2 - abs(dx)) * (2 - abs(dy)));
                s += w * texelFetch(u_tex, q, 0).r;
            }
        o = vec4(s / 16.0, 0.0, 0.0, 1.0);
    }
);
/* --- flat marker geometry (the wavemaker outline): world space, one colour --- */
static const char *vs_mark = GLSL(
    in vec3 a_pos;
    uniform sampler2D u_height;
    uniform ivec2 u_n;
    uniform vec2 u_L;
    uniform float u_gain;
    uniform float u_lift;
    uniform int u_shape;
    uniform mat4 u_vp;
    float H(int i, int j) {
        i = clamp(i, 0, u_n.x - 1); j = clamp(j, 0, u_n.y - 1);
        return texelFetch(u_height, ivec2(i, j), 0).r * u_gain;
    }
    void main() {
        /* the marker sits on the water, a little above it, whatever the water does:
         * the same height field the surface is drawn from, at this vertex */
        float h;
        if (u_shape == 1) {
            float R = 0.5 * u_L.x;
            vec2 c = a_pos.xz - vec2(R, R);
            float r = length(c), th = atan(c.y, c.x);
            if (th < 0.0) th += 6.28318530718;
            int nt = u_n.x, nr = u_n.y;
            float dr = R / float(nr), dth = 6.28318530718 / float(nt);
            int i = int(clamp(r / dr - 0.5, 0.0, float(nr - 1)));
            int j = int(th / dth) % nt;
            h = H(j, i);
        } else {
            int i = int(a_pos.x / u_L.x * float(u_n.x - 1) + 0.5);
            int j = int(a_pos.z / u_L.y * float(u_n.y - 1) + 0.5);
            h = H(i, j);
        }
        gl_Position = u_vp * vec4(a_pos.x, a_pos.y + h + u_lift, a_pos.z, 1.0);
    }
);
static const char *fs_mark = GLSL(
    uniform vec3 u_col;
    uniform float u_alpha;
    out vec4 o;
    void main() { o = vec4(gam(lit(u_col)), u_alpha); }
);

static const char *vs_fill = GLSL(
    out vec2 v_uv;
    void main() {
        vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
        v_uv = p * 0.5 + 0.5;
        gl_Position = vec4(p, 0.0, 1.0);
    }
);

/* --- solid floor slab and walls --- */
static const char *vs_solid = GLSL(
    in vec3 a_pos;
    in vec3 a_nrm;
    in float a_kind;
    uniform mat4 u_vp;
    out vec3 v_pos;
    out vec3 v_nrm;
    out float v_kind;
    void main() {
        v_pos = a_pos; v_nrm = a_nrm; v_kind = a_kind;
        gl_Position = u_vp * vec4(a_pos, 1.0);
    }
);
static const char *fs_solid = GLSL(
    in vec3 v_pos;
    in vec3 v_nrm;
    in float v_kind;
    uniform vec3 u_cam;
    uniform vec2 u_L;
    uniform float u_depth;
    uniform float u_tile;
    uniform int u_style;
    uniform vec3 u_mu;
    uniform int u_shape;
    uniform sampler2D u_light;
    uniform float u_lscale;
    out vec4 o;
    void main() {
        vec3 N = normalize(v_nrm);
        vec3 c;
        if (v_kind < 0.5) {
            /* interior floor: lit by the caustic map, seen through water along the
             * path from here towards the camera until it leaves the basin box */
            float lm = texture(u_light, v_pos.xz / u_L).r * u_lscale;
            c = pattern(v_pos.xz, u_tile, u_style) * (0.30 + 0.70 * lm * shadow_at(v_pos, vec3(0.0, 1.0, 0.0)));
            vec3 D = u_cam - v_pos;
            float dist = length(D); D /= dist;
            float t = dist;
            if (D.y > 0.0) t = min(t, (0.0 - v_pos.y) / D.y);
            if (u_shape == 1) {
                vec2 C = 0.5 * u_L; float R = 0.5 * u_L.x;
                vec2 pp = v_pos.xz - C, dd = D.xz;
                float a = dot(dd, dd), b = dot(pp, dd), cc = dot(pp, pp) - R * R;
                float disc = b * b - a * cc;
                if (a > 1e-12 && disc > 0.0) t = min(t, (-b + sqrt(disc)) / a);
            } else {
                if (D.x < 0.0) t = min(t, (0.0 - v_pos.x) / D.x);
                if (D.x > 0.0) t = min(t, (u_L.x - v_pos.x) / D.x);
                if (D.z < 0.0) t = min(t, (0.0 - v_pos.z) / D.z);
                if (D.z > 0.0) t = min(t, (u_L.y - v_pos.z) / D.z);
            }
            vec3 att = exp(-u_mu * max(t, 0.0));
            c = c * att + SCATTER * (1.0 - att);
        } else if (v_kind > 9.5) {
            /* the coping round a basin sunk in the terrace */
            c = scene_albedo(v_kind, v_pos, N) * (0.35 + 0.65 * max(dot(N, u_sun), 0.0) * shadow_at(v_pos, N));
        } else {
            /* kind 1: wall or slab face; kind 3: floor outside the basin. Both sunlit. */
            vec2 q = abs(N.x) > 0.5 ? vec2(v_pos.z, v_pos.y) : (abs(N.z) > 0.5 ? vec2(v_pos.x, v_pos.y) : v_pos.xz);
            float diff = max(dot(N, u_sun), 0.0);
            c = pattern(q, u_tile, u_style) * (0.35 + 0.65 * diff);
        }
        o = vec4(gam(lit(c)), 1.0);
    }
);

/* --- the house and garden --- */
static const char *fs_scene = GLSL(
    in vec3 v_pos;
    in vec3 v_nrm;
    in float v_kind;
    uniform vec3 u_cam;
    uniform float u_fog;      /* metres: the haze's e-folding distance */
    uniform float u_clip;     /* 1 in the reflection pass: nothing below the water */
    out vec4 o;
    void main() {
        if (u_clip > 0.5 && v_pos.y < 0.0) discard;
        vec3 N = normalize(v_nrm);
        vec3 V = u_cam - v_pos;
        float dist = length(V); V /= dist;
        if (dot(N, V) < 0.0) N = -N;                       /* fronds and leaves: both sides */
        vec3 sunc = mix(vec3(1.0), vec3(1.0, 0.62, 0.42), u_dusk);
        vec3 c;
        if (v_kind > 15.5 && v_kind < 16.5) {
            /* glass: the sky in it at a glance, the room behind it straight on; the
             * rooms light up after dark.  Mullions every 1.6 m, a transom at 2.3 m */
            float cv = max(dot(N, V), 0.0);
            float F = 0.08 + 0.92 * pow(1.0 - cv, 5.0);
            float fx = fract(v_pos.x / 1.6), fy = v_pos.y - 0.15;
            if (fx < 0.025 || abs(fy - 2.3) < 0.03) { c = lit(scene_albedo(21.0, v_pos, N) * 0.6); }
            else {
                /* the rooms behind: a box 7.4 m deep from floor to ceiling, partitioned
                 * every 4.8 m, found by following the view ray in (interior mapping) */
                vec3 D = -V;
                float tb = 7.4 / max(-D.z, 1e-4), t = tb;
                int face = 0;                                   /* 0 back, 1 floor, 2 ceiling, 3 partition */
                if (D.y < 0.0) { float tf = (0.15 - v_pos.y) / D.y; if (tf < t) { t = tf; face = 1; } }
                if (D.y > 0.0) { float tc = (2.95 - v_pos.y) / D.y; if (tc < t) { t = tc; face = 2; } }
                float xr = v_pos.x / 4.8;
                if (abs(D.x) > 1e-4) {
                    float xs = (D.x > 0.0 ? floor(xr) + 1.0 : ceil(xr) - 1.0) * 4.8;
                    float ts = (xs - v_pos.x) / D.x;
                    if (ts < t) { t = ts; face = 3; }
                }
                vec3 Q = v_pos + t * D;
                float room = floor((v_pos.x + 0.5 * t * D.x) / 4.8);
                vec3 a = face == 0 ? vec3(0.86, 0.82, 0.74) : face == 1 ? vec3(0.50, 0.36, 0.24) : face == 2 ? vec3(0.82, 0.80, 0.74) : vec3(0.78, 0.66, 0.56);
                if (face == 0 && abs(fract(Q.x / 4.8) - 0.5) < 0.18 && Q.y > 0.9 && Q.y < 2.0) a = vec3(0.20, 0.30, 0.40);   /* a picture on the back wall */
                float shade = face == 1 ? 1.0 : face == 2 ? 0.55 : 0.8;
                vec3 day = a * shade * (0.30 - 0.025 * t);       /* daylight through the glass, falling off inwards */
                float lamp = (1.0 - u_day) * (0.45 + 0.55 * hash21(vec2(room, 7.0)));
                vec3 glow = a * vec3(1.00, 0.74, 0.46) * 1.3 * lamp * shade * (1.0 - 0.06 * t);
                c = mix(lit(max(day, vec3(0.0))) + glow, sky(reflect(-V, N)), F);
            }
        } else {
            vec3 alb = scene_albedo(v_kind, v_pos, N);
            float diff = max(dot(N, u_sun), 0.0);
            if (v_kind > 18.5 && v_kind < 20.5) diff = max(diff, 0.35 * max(-dot(N, u_sun), 0.0));   /* leaves let light through */
            diff *= shadow_at(v_pos, N);
            c = lit(alb * (0.30 + 0.10 * N.y + 0.72 * diff * sunc));
        }
        /* haze: towards the horizon's own colour */
        float f = 1.0 - exp(-max(dist - 60.0, 0.0) / u_fog);     /* clear air near the house */
        vec3 hz = sky(normalize(vec3(-V.x, 0.015, -V.z)));
        o = vec4(gam(mix(c, hz, f)), 1.0);
    }
);

/* --- the camera: exposure, bloom, film tone, lens, grain --- */
static const char *fs_adapt = GLSL(
    in vec2 v_uv;
    uniform sampler2D u_hdr;
    uniform sampler2D u_prev;
    uniform float u_lod;
    uniform float u_rate;
    out vec4 o;
    void main() {
        /* metering: a centre-weighted geometric mean over a 5 x 5 grid of a small mip */
        float s = 0.0, w = 0.0;
        for (int j = 0; j < 5; j++)
            for (int i = 0; i < 5; i++) {
                vec2 q = vec2(0.1 + 0.2 * float(i), 0.1 + 0.2 * float(j));
                float wt = exp(-6.0 * dot(q - 0.5, q - 0.5));
                vec3 c = textureLod(u_hdr, q, u_lod).rgb;
                s += wt * log(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
                w += wt;
            }
        float avg = exp(s / w);
        /* a camera keeps night darker than day: the key falls with the light */
        float key = 0.24 * mix(0.40, 1.0, smoothstep(0.03, 0.25, avg));
        float ex = clamp(key / avg, 0.30, 1.6);
        float prev = texelFetch(u_prev, ivec2(0), 0).r;
        o = vec4(prev > 0.0 ? mix(prev, ex, u_rate) : ex, 0.0, 0.0, 1.0);
    }
);
static const char *fs_bright = GLSL(
    in vec2 v_uv;
    uniform sampler2D u_hdr;
    uniform sampler2D u_ex;
    out vec4 o;
    void main() {
        float ex = texelFetch(u_ex, ivec2(0), 0).r;
        vec3 c = textureLod(u_hdr, v_uv, 2.0).rgb * ex;
        o = vec4(max(c - vec3(0.9), vec3(0.0)), 1.0);
    }
);
static const char *fs_bloom = GLSL(
    in vec2 v_uv;
    uniform sampler2D u_src;
    uniform vec2 u_dir;
    out vec4 o;
    void main() {
        const float W[7] = float[7](0.1596, 0.1473, 0.1159, 0.0776, 0.0443, 0.0215, 0.0089);
        vec3 c = texture(u_src, v_uv).rgb * W[0];
        for (int k = 1; k < 7; k++) {
            c += texture(u_src, v_uv + u_dir * float(k)).rgb * W[k];
            c += texture(u_src, v_uv - u_dir * float(k)).rgb * W[k];
        }
        o = vec4(c, 1.0);
    }
);
static const char *fs_post = GLSL(
    in vec2 v_uv;
    uniform sampler2D u_hdr;
    uniform sampler2D u_bloom;
    uniform sampler2D u_ex;
    uniform vec2 u_res;
    out vec4 o;
    vec3 aces(vec3 x) { return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
    void main() {
        float ex = texelFetch(u_ex, ivec2(0), 0).r;
        vec2 d = v_uv - 0.5;
        float r2 = dot(d * vec2(u_res.x / u_res.y, 1.0), d * vec2(u_res.x / u_res.y, 1.0));
        /* lens: lateral colour towards the corners, and a little softness */
        vec2 off = d * 0.0025 * r2;
        vec3 c;
        c.r = textureLod(u_hdr, v_uv - off, 0.0).r;
        c.g = textureLod(u_hdr, v_uv, 0.0).g;
        c.b = textureLod(u_hdr, v_uv + off, 0.0).b;
        c = mix(c, textureLod(u_hdr, v_uv, 1.0).rgb, 0.10);
        c = c * ex + texture(u_bloom, v_uv).rgb * 0.35;
        /* vignette */
        c *= 1.0 - 0.33 * pow(clamp(r2 / 0.6, 0.0, 1.0), 1.4);
        /* film tone, a touch of saturation and warmth */
        /* film tone on the luminance, so the hues keep their saturation; only what
         * is past white rolls off towards white, as film does */
        float li = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-5);
        float lo = aces(vec3(li)).r;
        c = c * (lo / li);
        c = mix(c, vec3(lo), smoothstep(0.75, 1.0, lo) * 0.6);
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(vec3(l), c, 1.05) * vec3(1.02, 1.0, 0.975);
        c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
        c = mix(c, c * c * (3.0 - 2.0 * c), 0.30);           /* the print's S-curve */
        /* sensor grain: per frame, stronger in the shadows and when the exposure is pushed */
        float n = hash21(gl_FragCoord.xy + fract(u_time * 7.13) * vec2(91.7, 47.3)) + hash21(gl_FragCoord.yx * 1.37 + fract(u_time * 3.7) * 61.0) - 1.0;
        c += n * 0.020 * clamp(sqrt(ex), 0.7, 2.0) * (1.0 - 0.6 * l);
        o = vec4(c, 1.0);
    }
);

static const char *fs_depth = GLSL(
    out vec4 o;
    void main() { o = vec4(1.0); }
);

/* --- water body side faces (visible through glass walls) --- */
static const char *vs_sides = GLSL(
    in vec2 a_uv;
    in vec2 a_frac;
    in float a_bottom;
    in vec3 a_nrm;
    uniform sampler2D u_height;
    uniform ivec2 u_n;
    uniform vec2 u_L;
    uniform float u_depth;
    uniform float u_gain;
    uniform mat4 u_vp;
    out vec3 v_pos;
    out vec3 v_nrm;
    void main() {
        int i = clamp(int(a_uv.x), 0, u_n.x - 1), j = clamp(int(a_uv.y), 0, u_n.y - 1);
        float h = texelFetch(u_height, ivec2(i, j), 0).r * u_gain;
        v_pos = vec3(a_frac.x * u_L.x, a_bottom > 0.5 ? -u_depth : h, a_frac.y * u_L.y);
        v_nrm = a_nrm;
        gl_Position = u_vp * vec4(v_pos, 1.0);
    }
);
static const char *fs_sides = GLSL(
    in vec3 v_pos;
    in vec3 v_nrm;
    uniform vec2 u_L;
    uniform float u_depth;
    uniform vec3 u_mu;
    uniform vec3 u_cam;
    uniform int u_shape;
    uniform sampler2D u_light;
    uniform float u_lscale;
    out vec4 o;
    void main() {
        /* a face of the water body, seen through glass: the water's own Fresnel
         * reflection over the light inside it.  The light is marched along the view
         * ray: at each sample the sunlight has been focused by the surface above
         * towards what the caustic map says the floor gets, by a share that grows
         * with depth -- uniform just under the surface, the full caustic pattern on
         * the floor -- so the shafts converge downwards under the crests and move
         * with them. */
        vec3 N = normalize(v_nrm);
        vec3 V = normalize(u_cam - v_pos);
        if (dot(N, V) < 0.0) N = -N;
        float cv = max(dot(N, V), 0.0);
        float F = 0.02 + 0.98 * pow(1.0 - cv, 5.0);
        /* how far the ray goes inside the body */
        vec3 T = -V;
        float t = 1e30;
        if (u_shape == 1) {
            vec2 C = 0.5 * u_L; float R = 0.5 * u_L.x;
            vec2 pp = v_pos.xz - C, dd = T.xz;
            float a = dot(dd, dd), b = dot(pp, dd), c0 = dot(pp, pp) - R * R;
            float disc = b * b - a * c0;
            if (a > 1e-12 && disc > 0.0) t = max((-b + sqrt(disc)) / a, 0.0);
        } else {
            if (T.x < 0.0) t = min(t, (0.0 - v_pos.x) / T.x);
            if (T.x > 0.0) t = min(t, (u_L.x - v_pos.x) / T.x);
            if (T.z < 0.0) t = min(t, (0.0 - v_pos.z) / T.z);
            if (T.z > 0.0) t = min(t, (u_L.y - v_pos.z) / T.z);
        }
        if (T.y < 0.0) t = min(t, (-u_depth - v_pos.y) / T.y);
        if (T.y > 0.0) t = min(t, (0.0 - v_pos.y) / T.y);
        t = clamp(t, 0.0, 3.0 * max(u_L.x, u_L.y));
        /* sunlight inside the water travels along the refracted direction */
        vec3 Lr = refract(-u_sun, vec3(0.0, 1.0, 0.0), 1.0 / 1.333);
        if (dot(Lr, Lr) < 1e-6 || Lr.y >= -0.05) Lr = vec3(0.0, -1.0, 0.0);
        const int NS = 24;
        float ds = t / float(NS);
        vec3 acc = vec3(0.0);
        float wsum = 0.0;
        float jitter = hash21(gl_FragCoord.xy) - 0.5;
        for (int i = 0; i < NS; i++) {
            float si = (float(i) + 0.5 + jitter) * ds;
            vec3 P = v_pos + si * T;
            float f = clamp(-P.y / u_depth, 0.0, 1.0);
            vec3 Q = P + ((-u_depth - P.y) / Lr.y) * Lr;
            /* the map, clamped: a fresh crater is a lens with a hot spot under it, and
             * a shaft fed by an unclamped spot would outshine the whole tank */
            float lm = min(texture(u_light, Q.xz / u_L).r * u_lscale, 2.5);
            float I = mix(1.0, pow(lm, 1.5), f);
            /* the light that reaches the eye from a sample: attenuated on the way in
             * from the surface and on the way out to the glass, and the near water
             * counts for more, as the scattering is forward */
            vec3 att = exp(-u_mu * (si + f * u_depth)) * exp(-si / (0.25 * max(u_L.x, u_L.y)));
            wsum += (att.r + att.g + att.b) / 3.0 * ds;
            acc += SCATTER * I * att * ds;
        }
        /* normalised so that an unfocused body comes out as before */
        vec3 body = acc / max(wsum, 1e-3) * 1.3 + vec3(0.05, 0.08, 0.10);
        float path = 0.6 * max(u_L.x, u_L.y);
        vec3 att = exp(-u_mu * path);
        float a = clamp(1.0 - (att.r + att.g + att.b) / 3.0, 0.06, 0.92);
        vec3 c = mix(lit(body), sky(reflect(-V, N)), F);
        o = vec4(gam(c), max(a, F));
    }
);

/* --- glass --- */
static const char *fs_glass = GLSL(
    in vec3 v_pos;
    in vec3 v_nrm;
    in float v_kind;
    uniform vec3 u_cam;
    out vec4 o;
    void main() {
        vec3 N = normalize(v_nrm);
        vec3 V = normalize(u_cam - v_pos);
        if (dot(N, V) < 0.0) N = -N;
        float cv = max(dot(N, V), 0.0);
        float F = 0.04 + 0.96 * pow(1.0 - cv, 5.0);
        vec3 R = reflect(-V, N);
        vec3 c = sky(R) * F + vec3(0.55, 0.70, 0.75) * 0.06;
        o = vec4(gam(c), clamp(0.10 + 0.9 * F, 0.0, 1.0));
    }
);

/* --- overlay --- */
static const char *vs_ovl = GLSL(
    out vec2 v_uv;
    void main() {
        vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
        v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
        gl_Position = vec4(p, 0.0, 1.0);
    }
);
static const char *fs_ovl = GLSL(
    in vec2 v_uv;
    uniform sampler2D u_tex;
    out vec4 o;
    void main() { o = texture(u_tex, v_uv); }
);

#define MARK_MAX 2048       /* vertices for the wavemaker outline: 6 per piece */
#define OBJ_MAX 4096        /* vertices for the boat and the floats */

/* ------------------------------------------------------------------ state */
struct view3d {
    float time;                      /* seconds of frames, for the stars */
    SDL_Window *win;
    SDL_GLContext ctx;
    int nx, ny, W, H;
    int shape;                     /* 0 rectangle, 1 disk */
    float Lx, Ly, depth, dx, dy;   /* disk: Lx = Ly = 2R, dx = dtheta, dy = dr */
    int lm_w, lm_h;                /* caustic light map size */
    /* vertex ranges in vbo_solid: slab, walls, table, inner floor */
    int slab_off, slab_n, walls_off, walls_n, table_off, table_n, inner_off, inner_n;
    float yaw, pitch, dist, cx, cz;

    GLuint p_bg, p_surf, p_solid, p_sides, p_glass, p_ovl, p_caus, p_fill, p_mark, p_scene;
    GLuint vao_scene, vbo_scene; int n_scene;
    int cast_off, cast_n;           /* the part of it that casts shadows: not the ground, not the hills */
    int scene_on, scene_dirty;      /* the house is drawn; its geometry needs rebuilding */
    GLuint p_depth;
    GLuint fbo_sh, tex_sh; int sh_size, sh_valid; float sh_sun[3]; float lvp[16], sbias;   /* the sun's shadow map */
    GLuint fbo_rf, tex_rf, rb_rf; int rf_w, rf_h;                  /* the reflection, half the window */
    /* the video look: the frame in linear light, multisampled and resolved into a mipmapped
     * float texture, then metered, bloomed, tone-mapped, given a lens and grain */
    int video_on, video_ok;          /* this frame; -1 untried, 0 no float target here, 1 fine */
    GLuint fbo_ms, rb_ms_c, rb_ms_d; int ms_samples;
    GLuint fbo_hdr, tex_hdr, rb_hdr_d; int hdr_w, hdr_h, hdr_levels;
    GLuint fbo_bl[2], tex_bl[2]; int bl_w, bl_h;
    GLuint fbo_ex[2], tex_ex[2]; int ex_i;
    GLuint p_adapt, p_bright, p_bloom, p_post;
    float last_time;
    GLuint vao_mark, vbo_mark; int n_mark;
    GLuint vao_obj, vbo_obj;
    GLuint vao_caus, vbo_caus, ebo_caus; int n_caus_int, n_caus_all;   /* caustic mesh (rect: padded grid; disk: light-map grid) */
    GLuint vao_res, vbo_res, ebo_res; int n_res_idx; int res_pad;        /* disk: polar resample mesh */
    GLuint p_res, tex_hc, fbo_hc; int hc_pad;   /* texels of margin around the square */
    GLuint fbo_lm, tex_lm2, fbo_lm2, p_blur; int gpu_caustics;
    GLuint vao_empty;
    GLuint vao_surf, vbo_surf, ebo_surf; int n_surf_idx;
    GLuint vao_solid, vbo_solid;                  /* slab 36 verts, then 4 walls x 36 */
    GLuint vao_sides, vbo_sides, ebo_sides; int n_sides_idx, n_bottom_idx;   /* bottom face follows the sides */
    GLuint tex_h, tex_lm, tex_ovl;

    float *lm_acc; float *lm_tmp; uint8_t *lm8;
    float *cs_tab;                 /* disk: cos, sin per angle */
    canvas ovl; int ovl_dirty, ovl_w, ovl_h;
    char hud[640]; const char *const *help; int nhelp, show_help, show_hud;

    float cam[3], fwd[3], right[3], up[3], tanhalf, aspect;
    int want_capture; uint8_t *capture; int cap_w, cap_h;
};

/* ------------------------------------------------------------------ small math */
static void v3norm(float *a) { float l = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); if (l > 0) { a[0]/=l; a[1]/=l; a[2]/=l; } }
static void v3cross(const float *a, const float *b, float *c)
{ c[0] = a[1]*b[2] - a[2]*b[1]; c[1] = a[2]*b[0] - a[0]*b[2]; c[2] = a[0]*b[1] - a[1]*b[0]; }

static void mat_perspective(float *m, float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect; m[5] = f;
    m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f;
    m[14] = 2.0f * zf * zn / (zn - zf);
}
static void mat_lookat(float *m, const float *eye, const float *fwd, const float *right, const float *up)
{
    /* column-major view matrix from an orthonormal basis */
    m[0] = right[0]; m[4] = right[1]; m[8]  = right[2]; m[12] = -(right[0]*eye[0] + right[1]*eye[1] + right[2]*eye[2]);
    m[1] = up[0];    m[5] = up[1];    m[9]  = up[2];    m[13] = -(up[0]*eye[0] + up[1]*eye[1] + up[2]*eye[2]);
    m[2] = -fwd[0];  m[6] = -fwd[1];  m[10] = -fwd[2];  m[14] =  (fwd[0]*eye[0] + fwd[1]*eye[1] + fwd[2]*eye[2]);
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
}
static void mat_ortho(float *m, float l, float r, float b, float t, float n, float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (r - l); m[5] = 2.0f / (t - b); m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(f + n) / (f - n); m[15] = 1.0f;
}
static void mat_mul(float *r, const float *a, const float *b)   /* r = a * b */
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            t[c*4 + rr] = a[0*4 + rr]*b[c*4 + 0] + a[1*4 + rr]*b[c*4 + 1] + a[2*4 + rr]*b[c*4 + 2] + a[3*4 + rr]*b[c*4 + 3];
    memcpy(r, t, sizeof t);
}

/* ------------------------------------------------------------------ GL helpers */
static GLuint compile(GLenum type, const char *const *parts, int nparts)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, nparts, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        fprintf(stderr, "shader compile error:\n%.*s\n", (int)n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program_parts(const char *vs, const char *const *fs, int nfs, int sides)
{
    const char *vp[4] = { glsl_header, glsl_common, glsl_common2, vs };
    const char *fp[8] = { glsl_header, glsl_common, glsl_common2 };
    for (int i = 0; i < nfs && i < 5; i++) fp[3 + i] = fs[i];
    GLuint v = compile(GL_VERTEX_SHADER, vp, 4), f = compile(GL_FRAGMENT_SHADER, fp, 3 + (nfs < 5 ? nfs : 5));
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    /* fixed attribute slots; unknown names are ignored by GL */
    glBindAttribLocation(p, 0, "a_uv");
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_frac");
    glBindAttribLocation(p, 2, "a_kind");
    glBindAttribLocation(p, 2, "a_bottom");
    glBindAttribLocation(p, sides ? 3 : 1, "a_nrm");
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof log, &n, log);
        fprintf(stderr, "program link error:\n%.*s\n", (int)n, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint program(const char *vs, const char *fs, int sides) { return program_parts(vs, &fs, 1, sides); }

static GLint U(GLuint p, const char *name) { return glGetUniformLocation(p, name); }

/* ------------------------------------------------------------------ geometry */
static void box_faces(float *out, int *n, float x0, float x1, float y0, float y1, float z0, float z1, float kind_top, float kind)
{
    /* 6 faces, 2 triangles each, CCW seen from outside; vertex = pos(3) nrm(3) kind(1) */
    const float P[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
    const int F[6][4] = { {4,5,6,7}, {1,0,3,2}, {0,4,7,3}, {5,1,2,6}, {3,7,6,2}, {0,1,5,4} }; /* +z, -z, -x, +x, +y, -y */
    const float N[6][3] = { {0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0} };
    const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int f = 0; f < 6; f++) {
        float k = (f == 4) ? kind_top : kind;
        for (int t = 0; t < 6; t++) {
            const float *p = P[F[f][tri[t]]];
            float *v = out + (*n) * 7;
            v[0] = p[0]; v[1] = p[1]; v[2] = p[2];
            v[3] = N[f][0]; v[4] = N[f][1]; v[5] = N[f][2];
            v[6] = k;
            (*n)++;
        }
    }
}

/* emit a quad (two triangles) with vertex order fixed so the winding matches the normal */
static void quad(float *out, int *n, const float *p0, const float *p1, const float *p2, const float *p3,
                 const float *nrm, float kind)
{
    float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] }, e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] }, cr[3];
    v3cross(e1, e2, cr);
    int flip = (cr[0]*nrm[0] + cr[1]*nrm[1] + cr[2]*nrm[2]) < 0.0f;
    const float *tri[6] = { p0, p1, p2, p0, p2, p3 };
    if (flip) { tri[1] = p2; tri[2] = p1; tri[4] = p3; tri[5] = p2; }
    for (int t = 0; t < 6; t++) {
        float *v = out + (*n) * 7;
        v[0] = tri[t][0]; v[1] = tri[t][1]; v[2] = tri[t][2];
        v[3] = nrm[0]; v[4] = nrm[1]; v[5] = nrm[2]; v[6] = kind;
        (*n)++;
    }
}

static void tri(float *out, int *n, const float *p0, const float *p1, const float *p2, const float *nrm, float kind)
{
    float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] }, e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] }, cr[3];
    v3cross(e1, e2, cr);
    int flip = (cr[0]*nrm[0] + cr[1]*nrm[1] + cr[2]*nrm[2]) < 0.0f;
    const float *t3[3] = { p0, flip ? p2 : p1, flip ? p1 : p2 };
    for (int t = 0; t < 3; t++) {
        float *v = out + (*n) * 7;
        v[0] = t3[t][0]; v[1] = t3[t][1]; v[2] = t3[t][2];
        v[3] = nrm[0]; v[4] = nrm[1]; v[5] = nrm[2]; v[6] = kind;
        (*n)++;
    }
}

#define DISK_SEGS 96

static void build_solid(view3d *v)
{
    const float L = v->Lx, Lz = v->Ly, h = v->depth;
    const float Lmax = L > Lz ? L : Lz;
    /* sunk in the terrace, the walls stop 15 cm above the water, and their top is the coping */
    const float t = 0.03f * Lmax, fb = v->scene_on ? 0.15f : 0.06f * Lmax, E = 0.15f * Lmax;
    const float kt = v->scene_on ? 11.0f : 1.0f;
    float *buf;
    int n = 0;
    if (v->shape == 0) {
        buf = malloc(sizeof(float) * 7 * 222);
        v->slab_off = n;
        box_faces(buf, &n, -t, L + t, -h - t, -h, -t, Lz + t, 0.0f, 1.0f);   /* slab: top face is the floor */
        v->slab_n = n - v->slab_off;
        v->walls_off = n;
        box_faces(buf, &n, -t, 0.0f,  -h, fb, -t, Lz + t, kt, 1.0f);
        box_faces(buf, &n, L, L + t,  -h, fb, -t, Lz + t, kt, 1.0f);
        box_faces(buf, &n, 0.0f, L,   -h, fb, -t, 0.0f,   kt, 1.0f);
        box_faces(buf, &n, 0.0f, L,   -h, fb, Lz, Lz + t, kt, 1.0f);
        v->walls_n = n - v->walls_off;
        /* a table the basin sits on when the walls are invisible: floor plane continues */
        v->table_off = n;
        box_faces(buf, &n, -E, L + E, -h - t, -h, -E, Lz + E, 3.0f, 1.0f);
        v->table_n = n - v->table_off;
        v->inner_off = n;
        {
            const float eps = 0.0005f * Lmax, y = -h + eps;
            const float q[6][2] = { {0, 0}, {L, 0}, {L, Lz}, {0, 0}, {L, Lz}, {0, Lz} };
            for (int k = 0; k < 6; k++) {
                float *p = buf + (size_t)n * 7;
                p[0] = q[k][0]; p[1] = y; p[2] = q[k][1];
                p[3] = 0; p[4] = 1; p[5] = 0; p[6] = 0.0f;
                n++;
            }
        }
        v->inner_n = n - v->inner_off;
    } else {
        /* cylinder: walls are a ring R..R+t from -h to fb; slab a disc of radius R+t */
        const float R = 0.5f * L, cx = R, cz = R;
        const int S = DISK_SEGS;
        buf = malloc(sizeof(float) * 7 * (size_t)(S * (18 + 12) + 36 + S * 3 + 16));
        const float up[3] = { 0, 1, 0 }, down[3] = { 0, -1, 0 };
        float pa[3], pb[3], pc[3], pd[3], ctr[3], nrm[3];
        v->slab_off = n;
        for (int sgm = 0; sgm < S; sgm++) {
            float a0 = 2.0f * (float)M_PI * sgm / S, a1 = 2.0f * (float)M_PI * (sgm + 1) / S, am = 0.5f * (a0 + a1);
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            /* slab top (floor, kind 0) and bottom (kind 1) fans, side (kind 1) */
            ctr[0] = cx; ctr[1] = -h; ctr[2] = cz;
            pa[0] = cx + (R + t) * c0; pa[1] = -h; pa[2] = cz + (R + t) * s0;
            pb[0] = cx + (R + t) * c1; pb[1] = -h; pb[2] = cz + (R + t) * s1;
            tri(buf, &n, ctr, pa, pb, up, 0.0f);
            ctr[1] = pa[1] = pb[1] = -h - t;
            tri(buf, &n, ctr, pa, pb, down, 1.0f);
            pc[0] = pa[0]; pc[1] = -h; pc[2] = pa[2]; pd[0] = pb[0]; pd[1] = -h; pd[2] = pb[2];
            nrm[0] = cosf(am); nrm[1] = 0; nrm[2] = sinf(am);
            quad(buf, &n, pa, pb, pd, pc, nrm, 1.0f);
        }
        v->slab_n = n - v->slab_off;
        v->walls_off = n;
        for (int sgm = 0; sgm < S; sgm++) {
            float a0 = 2.0f * (float)M_PI * sgm / S, a1 = 2.0f * (float)M_PI * (sgm + 1) / S, am = 0.5f * (a0 + a1);
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float ni[3] = { -cosf(am), 0, -sinf(am) }, no[3] = { cosf(am), 0, sinf(am) };
            /* inner face at R */
            pa[0] = cx + R * c0; pa[1] = -h; pa[2] = cz + R * s0;
            pb[0] = cx + R * c1; pb[1] = -h; pb[2] = cz + R * s1;
            pc[0] = pb[0]; pc[1] = fb; pc[2] = pb[2];
            pd[0] = pa[0]; pd[1] = fb; pd[2] = pa[2];
            quad(buf, &n, pa, pb, pc, pd, ni, 1.0f);
            /* outer face at R + t */
            float qa[3] = { cx + (R + t) * c0, -h, cz + (R + t) * s0 }, qb[3] = { cx + (R + t) * c1, -h, cz + (R + t) * s1 };
            float qc[3] = { qb[0], fb, qb[2] }, qd[3] = { qa[0], fb, qa[2] };
            quad(buf, &n, qa, qb, qc, qd, no, 1.0f);
            /* top ring */
            quad(buf, &n, pd, pc, qc, qd, up, kt);
        }
        v->walls_n = n - v->walls_off;
        v->table_off = n;
        box_faces(buf, &n, -E, L + E, -h - t, -h, -E, L + E, 3.0f, 1.0f);
        v->table_n = n - v->table_off;
        v->inner_off = n;
        {
            const float eps = 0.0005f * Lmax, y = -h + eps;
            ctr[0] = cx; ctr[1] = y; ctr[2] = cz;
            for (int sgm = 0; sgm < S; sgm++) {
                float a0 = 2.0f * (float)M_PI * sgm / S, a1 = 2.0f * (float)M_PI * (sgm + 1) / S;
                pa[0] = cx + R * cosf(a0); pa[1] = y; pa[2] = cz + R * sinf(a0);
                pb[0] = cx + R * cosf(a1); pb[1] = y; pb[2] = cz + R * sinf(a1);
                tri(buf, &n, ctr, pa, pb, up, 0.0f);
            }
        }
        v->inner_n = n - v->inner_off;
    }
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_solid);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 7 * n), buf, GL_STATIC_DRAW);
    free(buf);
}

/* ------------------------------------------------------------------ the house and garden
 * Everything in metres, placed off the basin's centre and half-sizes, so it sits the
 * same round any basin from 4 to 40 m.  The terrace is flush with the coping, 15 cm
 * above the water; the lawn a step below it.  The house is behind the basin (-z),
 * its glass wall facing the water under a deep cantilevered roof; a cabana on the
 * -x side; palms and shrubs round the edges; low hills far off for a horizon. */
typedef struct { float *b; int n, cap; } gbuf;
static void gb_need(gbuf *g, int more)
{
    if (g->n + more <= g->cap) return;
    while (g->n + more > g->cap) g->cap = g->cap ? 2 * g->cap : 4096;
    g->b = realloc(g->b, sizeof(float) * 7 * (size_t)g->cap);
}
static void gb_box(gbuf *g, float x0, float x1, float y0, float y1, float z0, float z1, float ktop, float k)
{ gb_need(g, 36); box_faces(g->b, &g->n, x0, x1, y0, y1, z0, z1, ktop, k); }
static void gb_quad(gbuf *g, const float *a, const float *b, const float *c, const float *d, const float *nrm, float k)
{ gb_need(g, 6); quad(g->b, &g->n, a, b, c, d, nrm, k); }
static void gb_quadn(gbuf *g, const float *a, const float *b, const float *c, const float *d, float k)
{
    /* normal from the corners */
    float e1[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] }, e2[3] = { d[0]-b[0], d[1]-b[1], d[2]-b[2] }, nn[3];
    v3cross(e1, e2, nn); v3norm(nn);
    gb_quad(g, a, b, c, d, nn, k);
}

int view3d_setting_fits(double Lx, double Ly)
{
    const double L = Lx > Ly ? Lx : Ly;
    return L >= 4.0 && L <= 40.0;
}

/* the basin's hole in the terrace, along direction (c, s) from its centre */
static float hole_reach(const view3d *v, float c, float s, float t)
{
    if (v->shape == 1) return 0.5f * v->Lx + t;
    const float ax = 0.5f * v->Lx + t, az = 0.5f * v->Ly + t;
    float r = 1e30f;
    if (fabsf(c) > 1e-7f) r = fminf(r, ax / fabsf(c));
    if (fabsf(s) > 1e-7f) r = fminf(r, az / fabsf(s));
    return r;
}
static float rect_reach(float c, float s, float x0, float x1, float z0, float z1)
{
    float r = 1e30f;
    if (c > 1e-7f) r = fminf(r, x1 / c); else if (c < -1e-7f) r = fminf(r, x0 / c);
    if (s > 1e-7f) r = fminf(r, z1 / s); else if (s < -1e-7f) r = fminf(r, z0 / s);
    return r;
}
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static void palm(gbuf *g, float bx, float by, float bz, float H, float lx, float lz, float seed)
{
    /* a trunk curving with the lean, thinning upwards, and a crown of drooping fronds */
    const int SEG = 14, SIDES = 8;
    float prev[SIDES + 1][3], cur[SIDES + 1][3];
    float top[3] = { 0 };
    for (int i = 0; i <= SEG; i++) {
        float s = (float)i / SEG;
        float cx = bx + lx * s * s, cy = by + H * s, cz = bz + lz * s * s;
        float r = 0.24f * (1.0f - 0.45f * s) * (i == 0 ? 1.25f : 1.0f);
        for (int k = 0; k <= SIDES; k++) {
            float a = 2.0f * (float)M_PI * k / SIDES;
            cur[k][0] = cx + r * cosf(a); cur[k][1] = cy; cur[k][2] = cz + r * sinf(a);
        }
        if (i > 0)
            for (int k = 0; k < SIDES; k++) {
                float a = 2.0f * (float)M_PI * (k + 0.5f) / SIDES, nrm[3] = { cosf(a), 0.0f, sinf(a) };
                gb_quad(g, prev[k], prev[k + 1], cur[k + 1], cur[k], nrm, 18.0f);
            }
        memcpy(prev, cur, sizeof cur);
        top[0] = cx; top[1] = cy; top[2] = cz;
    }
    const int NF = 13, FS = 7;
    for (int f = 0; f < NF; f++) {
        float az = 2.0f * (float)M_PI * (f + 0.37f * sinf(seed * 7.1f + f * 2.3f)) / NF + seed;
        float len = 3.6f + 0.8f * sinf(seed * 3.0f + f * 1.7f);
        float rise = 0.9f + 0.5f * cosf(seed + f * 2.9f);
        float dx = cosf(az), dz = sinf(az), px = -dz, pz = dx;
        float Lp[3], Rp[3], Lc[3], Rc[3];
        for (int i = 0; i <= FS; i++) {
            float u = (float)i / FS;
            float w = 0.55f * sinf((float)M_PI * fminf(1.0f, u * 1.15f + 0.05f));
            float cxp = top[0] + dx * len * u, cyp = top[1] + rise * u - 3.0f * u * u, czp = top[2] + dz * len * u;
            /* leaflets droop either side of the rib: a shallow V */
            Lc[0] = cxp + px * w; Lc[1] = cyp - 0.35f * w; Lc[2] = czp + pz * w;
            Rc[0] = cxp - px * w; Rc[1] = cyp - 0.35f * w; Rc[2] = czp - pz * w;
            float Cc[3] = { cxp, cyp, czp };
            if (i > 0) {
                float Cprev[3] = { top[0] + dx * len * (i - 1.0f) / FS, top[1] + rise * (i - 1.0f) / FS - 3.0f * ((i - 1.0f) / FS) * ((i - 1.0f) / FS), top[2] + dz * len * (i - 1.0f) / FS };
                gb_quadn(g, Cprev, Cc, Lc, Lp, 19.0f);
                gb_quadn(g, Cprev, Rp, Rc, Cc, 19.0f);
            }
            memcpy(Lp, Lc, sizeof Lc); memcpy(Rp, Rc, sizeof Rc);
        }
    }
}

static void blob(gbuf *g, float cx, float by, float cz, float r, float hy, float k)
{
    /* a shrub: a lumpy half-ellipsoid on the ground */
    const int NA = 18, NB = 7;
    float P[NB + 1][NA + 1][3], Nm[NB + 1][NA + 1][3];
    for (int j = 0; j <= NB; j++)
        for (int i = 0; i <= NA; i++) {
            float th = 0.5f * (float)M_PI * j / NB, ph = 2.0f * (float)M_PI * i / NA;
            float lump = 1.0f + 0.10f * sinf(3.0f * ph + cx) * cosf(2.0f * th + cz);
            P[j][i][0] = cx + r * lump * cosf(th) * cosf(ph);
            P[j][i][1] = by + hy * lump * sinf(th);
            P[j][i][2] = cz + r * lump * cosf(th) * sinf(ph);
            Nm[j][i][0] = cosf(th) * cosf(ph) / r; Nm[j][i][1] = sinf(th) / hy + (j == NB ? 1.0f : 0.0f); Nm[j][i][2] = cosf(th) * sinf(ph) / r;
            v3norm(Nm[j][i]);
        }
    /* smooth: each corner keeps its own normal */
    for (int j = 0; j < NB; j++)
        for (int i = 0; i < NA; i++) {
            const int cj[6] = { j, j, j + 1, j, j + 1, j + 1 }, ci[6] = { i, i + 1, i + 1, i, i + 1, i };
            gb_need(g, 6);
            for (int q = 0; q < 6; q++) {
                float *o = g->b + 7 * (size_t)g->n++;
                memcpy(o, P[cj[q]][ci[q]], 3 * sizeof(float));
                memcpy(o + 3, Nm[cj[q]][ci[q]], 3 * sizeof(float));
                o[6] = k;
            }
        }
}

static void lounger(gbuf *g, float x, float y0, float z, float dir)
{
    /* a sun lounger along z: a flat bed and a raised back at the far end (dir = +1 or -1) */
    const float w = 0.34f;
    float za = z, zb = z + dir * 1.35f;
    gb_box(g, x - w, x + w, y0 + 0.24f, y0 + 0.32f, fminf(za, zb), fmaxf(za, zb), 22.0f, 22.0f);
    gb_box(g, x - w, x - w + 0.05f, y0, y0 + 0.24f, fminf(za, zb), fminf(za, zb) + 0.05f, 21.0f, 21.0f);
    gb_box(g, x + w - 0.05f, x + w, y0, y0 + 0.24f, fminf(za, zb), fminf(za, zb) + 0.05f, 21.0f, 21.0f);
    gb_box(g, x - w, x - w + 0.05f, y0, y0 + 0.24f, fmaxf(za, zb) - 0.05f, fmaxf(za, zb), 21.0f, 21.0f);
    gb_box(g, x + w - 0.05f, x + w, y0, y0 + 0.24f, fmaxf(za, zb) - 0.05f, fmaxf(za, zb), 21.0f, 21.0f);
    float a[3] = { x - w, y0 + 0.32f, zb }, b[3] = { x + w, y0 + 0.32f, zb };
    float c[3] = { x + w, y0 + 0.88f, zb + dir * 0.55f }, d[3] = { x - w, y0 + 0.88f, zb + dir * 0.55f };
    gb_quadn(g, a, b, c, d, 22.0f);
}

static void build_scene(view3d *v)
{
    gbuf g = { 0 };
    const float Lmax = v->Lx > v->Ly ? v->Lx : v->Ly;
    const float t = 0.03f * Lmax;                         /* the walls' thickness, as in build_solid */
    const float pcx = 0.5f * v->Lx, pcz = 0.5f * v->Ly;
    const float ax = 0.5f * v->Lx, az = 0.5f * v->Ly;
    const float y0 = 0.15f, yl = y0 - 0.06f;              /* terrace top (= coping), lawn */
    /* the terrace, relative to the basin's centre */
    const float X0 = -ax - 7.0f, X1 = ax + 5.0f, Z0 = -az - 3.5f, Z1 = az + 4.0f;
    const float up[3] = { 0, 1, 0 };

    /* terrace: a ring from the basin's hole out to its rectangle, fanned about the centre */
    {
        float ang[256 + 8];
        int na = 0;
        for (int i = 0; i < 256; i++) ang[na++] = -(float)M_PI + 2.0f * (float)M_PI * i / 256;
        if (v->shape == 0) {
            const float hx = ax + t, hz = az + t;
            ang[na++] = atan2f(hz, hx); ang[na++] = atan2f(hz, -hx); ang[na++] = atan2f(-hz, -hx); ang[na++] = atan2f(-hz, hx);
        }
        ang[na++] = atan2f(Z1, X1); ang[na++] = atan2f(Z1, X0); ang[na++] = atan2f(Z0, X0); ang[na++] = atan2f(Z0, X1);
        qsort(ang, (size_t)na, sizeof ang[0], cmpf);
        for (int i = 0; i < na; i++) {
            float a0 = ang[i], a1 = (i + 1 < na) ? ang[i + 1] : ang[0] + 2.0f * (float)M_PI;
            if (a1 - a0 < 1e-6f) continue;
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float r0 = hole_reach(v, c0, s0, t), r1 = hole_reach(v, c1, s1, t);
            float R0 = rect_reach(c0, s0, X0, X1, Z0, Z1), R1 = rect_reach(c1, s1, X0, X1, Z0, Z1);
            float p0[3] = { pcx + r0 * c0, y0, pcz + r0 * s0 }, p1[3] = { pcx + r1 * c1, y0, pcz + r1 * s1 };
            float q1[3] = { pcx + R1 * c1, y0, pcz + R1 * s1 }, q0[3] = { pcx + R0 * c0, y0, pcz + R0 * s0 };
            gb_quad(&g, p0, p1, q1, q0, up, 10.0f);
        }
        /* its edge, down to the lawn */
        const float xa = pcx + X0, xb = pcx + X1, za = pcz + Z0, zb = pcz + Z1;
        float c[4][3] = { { xa, 0, za }, { xb, 0, za }, { xb, 0, zb }, { xa, 0, zb } };
        const float nn[4][3] = { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 } };
        for (int e = 0; e < 4; e++) {
            const float *A = c[e], *B = c[(e + 1) % 4];
            float a0[3] = { A[0], yl, A[2] }, b0[3] = { B[0], yl, B[2] }, b1[3] = { B[0], y0, B[2] }, a1[3] = { A[0], y0, A[2] };
            gb_quad(&g, a0, b0, b1, a1, nn[e], 10.0f);
        }
        /* the lawn, everywhere else: four strips round the terrace, out to the hills */
        const float G = 260.0f;
        const float gx0 = pcx - G, gx1 = pcx + G, gz0 = pcz - G, gz1 = pcz + G;
        float s[4][4] = { { gx0, gx1, gz0, za }, { gx0, gx1, zb, gz1 }, { gx0, xa, za, zb }, { xb, gx1, za, zb } };
        for (int e = 0; e < 4; e++) {
            float A[3] = { s[e][0], yl, s[e][2] }, B[3] = { s[e][1], yl, s[e][2] }, C[3] = { s[e][1], yl, s[e][3] }, D[3] = { s[e][0], yl, s[e][3] };
            gb_quad(&g, A, B, C, D, up, 12.0f);
        }
    }

    v->cast_off = g.n;
    /* the house: 18 m of it behind the basin, its front 3.5 m back from the water */
    {
        const float hx0 = pcx - 9.5f, hx1 = pcx + 8.5f, zf = pcz + Z0, zb = zf - 8.0f;
        const float yw = y0 + 2.8f;                        /* the roof's underside */
        gb_box(&g, hx0, hx1, yl, yw, zb, zb + 0.25f, 14.0f, 14.0f);               /* back wall */
        gb_box(&g, hx0, hx0 + 0.3f, yl, yw, zb, zf, 13.0f, 13.0f);                /* end walls, colour-washed */
        gb_box(&g, hx1 - 0.3f, hx1, yl, yw, zb, zf, 13.0f, 13.0f);
        gb_box(&g, hx0, hx0 + 5.5f, yl, yw, zf - 0.3f, zf, 13.0f, 13.0f);         /* the solid bay of the front */
        {
            float a[3] = { hx0 + 5.5f, y0, zf - 0.15f }, b[3] = { hx1 - 0.3f, y0, zf - 0.15f };
            float c[3] = { hx1 - 0.3f, yw, zf - 0.15f }, d[3] = { hx0 + 5.5f, yw, zf - 0.15f };
            const float nz[3] = { 0, 0, 1 };
            gb_quad(&g, a, b, c, d, nz, 16.0f);                                   /* the glass wall */
            gb_box(&g, hx0 + 5.5f, hx1 - 0.3f, yl, y0 + 0.02f, zf - 0.3f, zf, 21.0f, 21.0f);   /* its sill */
        }
        /* the roof: a thin slab, deep over the terrace, white edges and gravel on top */
        gb_box(&g, hx0 - 0.8f, hx1 + 1.6f, yw, yw + 0.32f, zb - 0.8f, zf + 2.3f, 15.0f, 14.0f);
        /* slim posts under the overhang's edge */
        for (int i = 0; i < 4; i++) {
            float px = hx0 + 6.8f + 4.4f * i;
            gb_box(&g, px - 0.06f, px + 0.06f, y0, yw, zf + 2.0f, zf + 2.12f, 21.0f, 21.0f);
        }
        /* the chimney, stacked stone, through the roof at the solid end */
        gb_box(&g, hx0 + 1.4f, hx0 + 2.8f, yl, yw + 1.7f, zb + 2.2f, zb + 3.6f, 17.0f, 17.0f);
        /* a planter of stone along the solid bay */
        gb_box(&g, hx0 + 0.6f, hx0 + 4.9f, yl, y0 + 0.45f, zf + 0.25f, zf + 0.85f, 17.0f, 17.0f);
        blob(&g, hx0 + 1.6f, y0 + 0.45f, zf + 0.55f, 0.55f, 0.55f, 20.0f);
        blob(&g, hx0 + 3.7f, y0 + 0.45f, zf + 0.55f, 0.65f, 0.7f, 20.0f);
    }

    /* the cabana on the -x side: a roof on two posts and a pink back wall */
    {
        const float x0 = pcx - ax - 6.3f, x1 = pcx - ax - 2.2f, z0 = pcz - 1.9f, z1 = pcz + 2.4f, yr = y0 + 2.45f;
        gb_box(&g, x0, x0 + 0.25f, yl, yr, z0, z1, 13.0f, 13.0f);
        gb_box(&g, x0 - 0.2f, x1 + 0.3f, yr, yr + 0.22f, z0 - 0.3f, z1 + 0.3f, 15.0f, 14.0f);
        gb_box(&g, x1 - 0.06f, x1 + 0.06f, y0, yr, z0 + 0.05f, z0 + 0.17f, 21.0f, 21.0f);
        gb_box(&g, x1 - 0.06f, x1 + 0.06f, y0, yr, z1 - 0.17f, z1 - 0.05f, 21.0f, 21.0f);
        lounger(&g, x0 + 1.4f, y0, z0 + 0.6f, 1.0f);
        lounger(&g, x0 + 2.6f, y0, z0 + 0.6f, 1.0f);
    }
    /* two loungers on the near side, facing the water */
    lounger(&g, pcx - 2.4f, y0, pcz + az + 1.4f, 1.0f);
    lounger(&g, pcx - 1.2f, y0, pcz + az + 1.4f, 1.0f);

    /* palms and shrubs round the edges */
    /* a pair at the house's far corner, one leaning out over the terrace; one behind
     * the roof; one by the cabana; one alone on the lawn */
    palm(&g, pcx + 10.6f,     yl, pcz + Z0 - 1.2f,  8.6f, -1.6f,  1.4f, 0.3f);
    palm(&g, pcx + 11.6f,     yl, pcz + Z0 + 0.2f,  6.8f,  0.8f,  0.9f, 1.9f);
    palm(&g, pcx - 3.0f,      yl, pcz + Z0 - 9.8f,  9.6f,  0.7f, -0.4f, 3.1f);
    palm(&g, pcx + X0 - 1.6f, yl, pcz - 3.6f,       7.6f, -0.9f, -0.5f, 4.4f);
    palm(&g, pcx + X1 + 6.5f, yl, pcz + Z1 + 3.0f,  8.2f,  0.6f,  0.8f, 5.6f);
    blob(&g, pcx + X1 + 1.3f, yl, pcz + 1.5f, 1.1f, 0.9f, 20.0f);
    blob(&g, pcx + X1 + 1.1f, yl, pcz - 2.5f, 0.9f, 0.8f, 20.0f);
    blob(&g, pcx + X0 - 1.2f, yl, pcz + 4.0f, 1.2f, 1.0f, 20.0f);
    blob(&g, pcx + X0 - 1.0f, yl, pcz - 4.5f, 1.0f, 0.9f, 20.0f);
    blob(&g, pcx + X1 + 1.2f, yl, pcz + Z0 - 1.0f, 1.0f, 0.9f, 20.0f);

    v->cast_n = g.n - v->cast_off;
    /* low hills on the horizon, 220 m off */
    {
        const int NH = 180;
        const float RH = 220.0f;
        for (int i = 0; i < NH; i++) {
            float a0 = 2.0f * (float)M_PI * i / NH, a1 = 2.0f * (float)M_PI * (i + 1) / NH;
            float h0 = 7.0f + 6.0f * sinf(3.0f * a0 + 0.7f) + 3.5f * sinf(7.0f * a0 + 1.9f) + 1.5f * sinf(17.0f * a0);
            float h1 = 7.0f + 6.0f * sinf(3.0f * a1 + 0.7f) + 3.5f * sinf(7.0f * a1 + 1.9f) + 1.5f * sinf(17.0f * a1);
            float A[3] = { pcx + RH * cosf(a0), yl - 1.0f, pcz + RH * sinf(a0) }, B[3] = { pcx + RH * cosf(a1), yl - 1.0f, pcz + RH * sinf(a1) };
            float C[3] = { B[0], yl + h1, B[2] }, D[3] = { A[0], yl + h0, A[2] };
            float am = 0.5f * (a0 + a1), nrm[3] = { -cosf(am), 0.35f, -sinf(am) };
            v3norm(nrm);
            gb_quad(&g, A, B, C, D, nrm, 23.0f);
        }
    }

    v->n_scene = g.n;
    v->sh_valid = 0;
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_scene);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 7 * (size_t)(g.n ? g.n : 1)), g.b, GL_STATIC_DRAW);
    free(g.b);
}

static void build_surface(view3d *v)
{
    const int nx = v->nx, ny = v->ny;
    float *verts = malloc(sizeof(float) * 2 * (size_t)nx * ny);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) { verts[2 * (i + nx * j)] = (float)i; verts[2 * (i + nx * j) + 1] = (float)j; }
    GLuint *idx = malloc(sizeof(GLuint) * 6 * (size_t)nx * ny);
    int n = 0;
    if (v->shape == 0) {
        for (int j = 0; j < ny - 1; j++)
            for (int i = 0; i < nx - 1; i++) {
                GLuint a = (GLuint)(i + nx * j), b = a + 1, c = a + (GLuint)nx, d = c + 1;
                idx[n++] = a; idx[n++] = c; idx[n++] = b;
                idx[n++] = b; idx[n++] = c; idx[n++] = d;
            }
    } else {
        /* rings j (rows) x angles i (columns), closed in the angle */
        for (int j = 0; j < ny - 1; j++)
            for (int i = 0; i < nx; i++) {
                int i1 = (i + 1) % nx;
                GLuint a = (GLuint)(i + nx * j), b = (GLuint)(i1 + nx * j), c = (GLuint)(i + nx * (j + 1)), d = (GLuint)(i1 + nx * (j + 1));
                idx[n++] = a; idx[n++] = c; idx[n++] = b;
                idx[n++] = b; idx[n++] = c; idx[n++] = d;
            }
    }
    v->n_surf_idx = n;
    glBindVertexArray(v->vao_surf);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_surf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 2 * nx * ny), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->ebo_surf);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GLuint) * n), idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
    free(verts); free(idx);
}

/* Mesh for the caustic pass: a W x H grid of cell centres, padded by (px, py)
 * cells past the edges (mirrored fetch in the shader).  Interior quads come
 * first in the index buffer so the no-pad case is a shorter draw. */
static void build_caustic_mesh(view3d *v, int W, int Hh, int px, int py)
{
    const int PW = W + 2 * px, PH = Hh + 2 * py;
    float *verts = malloc(sizeof(float) * 2 * (size_t)PW * PH);
    for (int j = 0; j < PH; j++)
        for (int i = 0; i < PW; i++) { verts[2 * (i + PW * j)] = (float)(i - px); verts[2 * (i + PW * j) + 1] = (float)(j - py); }
    GLuint *idx = malloc(sizeof(GLuint) * 6 * (size_t)PW * PH);
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < PH - 1; j++)
            for (int i = 0; i < PW - 1; i++) {
                int ii = i - px, jj = j - py;
                int interior = ii >= 0 && ii < W - 1 && jj >= 0 && jj < Hh - 1;
                if ((pass == 0) != interior) continue;
                GLuint a = (GLuint)(i + PW * j), b = a + 1, c = (GLuint)(i + PW * (j + 1)), d = c + 1;
                idx[n++] = a; idx[n++] = c; idx[n++] = b;
                idx[n++] = b; idx[n++] = c; idx[n++] = d;
            }
        if (pass == 0) v->n_caus_int = n;
    }
    v->n_caus_all = n;
    glBindVertexArray(v->vao_caus);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_caus);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 2 * PW * PH), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->ebo_caus);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GLuint) * n), idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
    free(verts); free(idx);
}

/* Disk: polar mesh with `pad` mirrored rings past the rim and a centre fan, drawn
 * into the Cartesian height texture the caustic pass reads. */
static void build_resample_mesh(view3d *v)
{
    const int nt = v->nx, nr = v->ny, pad = nr / 6, rings = nr + pad;
    v->res_pad = pad;
    float *verts = malloc(sizeof(float) * 2 * ((size_t)nt * rings + 1));
    int nv = 0;
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < nt; j++) { verts[2 * nv] = (float)j; verts[2 * nv + 1] = (float)i; nv++; }
    const GLuint ctr = (GLuint)nv;
    verts[2 * nv] = 0.0f; verts[2 * nv + 1] = -1.0f; nv++;
    GLuint *idx = malloc(sizeof(GLuint) * (6 * (size_t)nt * rings + 3 * (size_t)nt));
    int n = 0;
    for (int i = 0; i < rings - 1; i++)
        for (int j = 0; j < nt; j++) {
            int j1 = (j + 1) % nt;
            GLuint a = (GLuint)(j + nt * i), b = (GLuint)(j1 + nt * i), c = (GLuint)(j + nt * (i + 1)), d = (GLuint)(j1 + nt * (i + 1));
            idx[n++] = a; idx[n++] = c; idx[n++] = b;
            idx[n++] = b; idx[n++] = c; idx[n++] = d;
        }
    for (int j = 0; j < nt; j++) { idx[n++] = ctr; idx[n++] = (GLuint)j; idx[n++] = (GLuint)((j + 1) % nt); }
    v->n_res_idx = n;
    glBindVertexArray(v->vao_res);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_res);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 2 * nv), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->ebo_res);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GLuint) * n), idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
    free(verts); free(idx);
}

static void build_sides(view3d *v)
{
    /* vertex = uv(2) frac(2) bottom(1) nrm(3).  Rectangle: four strips, one column per
     * edge cell (same x/z mapping as the surface mesh, so the top edge coincides with it).
     * Disk: one closed strip around the rim.  Then a flat bottom face. */
    const int nx = v->nx, ny = v->ny, VS = 8;
    int cols = (v->shape == 0) ? 2 * nx + 2 * ny : nx + 1;
    float *verts = malloc(sizeof(float) * VS * (2 * (size_t)cols + (size_t)nx + 4));
    GLuint *idx = malloc(sizeof(GLuint) * (6 * (size_t)cols + 3 * (size_t)nx + 6));
    int nv = 0, ni = 0;
    if (v->shape == 0) {
        for (int wall = 0; wall < 4; wall++) {
            int along = (wall < 2) ? nx : ny;
            for (int c = 0; c < along; c++) {
                float frac = (float)c / (float)(along - 1);
                float u, w, fx, fz, nX = 0, nZ = 0;
                if (wall == 0)      { u = (float)c; w = 0.0f;            fx = frac; fz = 0.0f; nZ = -1; }
                else if (wall == 1) { u = (float)c; w = (float)(ny - 1); fx = frac; fz = 1.0f; nZ =  1; }
                else if (wall == 2) { u = 0.0f;     w = (float)c;        fx = 0.0f; fz = frac; nX = -1; }
                else                { u = (float)(nx - 1); w = (float)c; fx = 1.0f; fz = frac; nX =  1; }
                for (int b = 0; b < 2; b++) {
                    float *p = verts + nv * VS;
                    p[0] = u; p[1] = w; p[2] = fx; p[3] = fz; p[4] = (float)b; p[5] = nX; p[6] = 0; p[7] = nZ;
                    nv++;
                }
                if (c > 0) {
                    GLuint t0 = (GLuint)(nv - 4), b0 = t0 + 1, t1 = t0 + 2, b1 = t0 + 3;
                    idx[ni++] = t0; idx[ni++] = b0; idx[ni++] = t1;
                    idx[ni++] = t1; idx[ni++] = b0; idx[ni++] = b1;
                }
            }
        }
    } else {
        for (int c = 0; c <= nx; c++) {
            float th = 2.0f * (float)M_PI * (float)c / (float)nx, ct = cosf(th), st = sinf(th);
            for (int b = 0; b < 2; b++) {
                float *p = verts + nv * VS;
                p[0] = (float)(c % nx); p[1] = (float)(ny - 1); p[2] = 0.5f + 0.5f * ct; p[3] = 0.5f + 0.5f * st;
                p[4] = (float)b; p[5] = ct; p[6] = 0; p[7] = st;
                nv++;
            }
            if (c > 0) {
                GLuint t0 = (GLuint)(nv - 4), b0 = t0 + 1, t1 = t0 + 2, b1 = t0 + 3;
                idx[ni++] = t0; idx[ni++] = b0; idx[ni++] = t1;
                idx[ni++] = t1; idx[ni++] = b0; idx[ni++] = b1;
            }
        }
    }
    v->n_sides_idx = ni;
    /* bottom face of the water body (drawn only when there is no container) */
    if (v->shape == 0) {
        const float corners[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
        GLuint base = (GLuint)nv;
        for (int c = 0; c < 4; c++) {
            float *p = verts + nv * VS;
            p[0] = 0; p[1] = 0; p[2] = corners[c][0]; p[3] = corners[c][1]; p[4] = 1.0f; p[5] = 0; p[6] = -1; p[7] = 0;
            nv++;
        }
        idx[ni++] = base; idx[ni++] = base + 1; idx[ni++] = base + 2;
        idx[ni++] = base; idx[ni++] = base + 2; idx[ni++] = base + 3;
    } else {
        GLuint ctr = (GLuint)nv;
        float *p = verts + nv * VS;
        p[0] = 0; p[1] = 0; p[2] = 0.5f; p[3] = 0.5f; p[4] = 1.0f; p[5] = 0; p[6] = -1; p[7] = 0;
        nv++;
        GLuint base = (GLuint)nv;
        for (int c = 0; c <= nx; c++) {
            float th = 2.0f * (float)M_PI * (float)c / (float)nx;
            p = verts + nv * VS;
            p[0] = 0; p[1] = 0; p[2] = 0.5f + 0.5f * cosf(th); p[3] = 0.5f + 0.5f * sinf(th); p[4] = 1.0f; p[5] = 0; p[6] = -1; p[7] = 0;
            nv++;
        }
        for (int c = 0; c < nx; c++) { idx[ni++] = ctr; idx[ni++] = base + (GLuint)c; idx[ni++] = base + (GLuint)c + 1; }
    }
    v->n_bottom_idx = ni - v->n_sides_idx;

    glBindVertexArray(v->vao_sides);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_sides);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * VS * nv), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VS * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, VS * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, VS * sizeof(float), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, VS * sizeof(float), (void *)(5 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->ebo_sides);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GLuint) * ni), idx, GL_STATIC_DRAW);
    glBindVertexArray(0);
    free(verts); free(idx);
}

static void video_free(view3d *v);

/* ------------------------------------------------------------------ public */
void view3d_gl_attributes(int msaa)
{
#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    if (msaa) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    }
}

view3d *view3d_create(SDL_Window *win, const wave *w, int cpu_caustics)
{
    view3d *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    const int nx = w->nx, ny = w->ny;
    v->win = win; v->nx = nx; v->ny = ny;
    v->shape = (w->shape == WAVE_DISK) ? 1 : 0;
    v->lm_w = (v->shape == 0) ? nx : nx / 2;
    v->lm_h = (v->shape == 0) ? ny : nx / 2;
    v->ctx = SDL_GL_CreateContext(win);
    if (!v->ctx) {
        /* retry without multisampling */
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        v->ctx = SDL_GL_CreateContext(win);
    }
    if (!v->ctx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); free(v); return NULL; }
    SDL_GL_MakeCurrent(win, v->ctx);
    SDL_GL_SetSwapInterval(1);
    if (gl_load()) { fprintf(stderr, "could not load OpenGL 3.3 core entry points\n"); free(v); return NULL; }
    printf("GL: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

    v->p_bg    = program(vs_bg, fs_bg, 0);
    {
        const char *parts[3] = { fs_surf_head, glsl_water, fs_surf };
        v->p_surf = program_parts(vs_surf, parts, 3, 0);
    }
    v->p_solid = program(vs_solid, fs_solid, 0);
    v->p_sides = program(vs_sides, fs_sides, 1);
    v->p_glass = program(vs_solid, fs_glass, 0);
    v->p_ovl   = program(vs_ovl, fs_ovl, 0);
    v->p_caus  = program(vs_caus, fs_caus, 0);
    v->p_fill  = program(vs_fill, fs_fill, 0);
    v->p_blur  = program(vs_fill, fs_blur, 0);
    v->p_mark  = program(vs_mark, fs_mark, 0);
    v->p_res   = program(vs_resample, fs_resample, 0);
    v->p_scene = program(vs_solid, fs_scene, 0);
    v->p_depth = program(vs_solid, fs_depth, 0);
    v->p_adapt = program(vs_fill, fs_adapt, 0);
    v->p_bright = program(vs_fill, fs_bright, 0);
    v->p_bloom = program(vs_fill, fs_bloom, 0);
    v->p_post = program(vs_fill, fs_post, 0);
    if (!v->p_adapt || !v->p_bright || !v->p_bloom || !v->p_post || !v->p_depth || !v->p_scene || !v->p_res || !v->p_blur || !v->p_bg || !v->p_surf || !v->p_solid || !v->p_sides || !v->p_glass || !v->p_ovl || !v->p_caus || !v->p_fill || !v->p_mark) { free(v); return NULL; }

    glGenVertexArrays(1, &v->vao_empty);
    glGenVertexArrays(1, &v->vao_surf); glGenBuffers(1, &v->vbo_surf); glGenBuffers(1, &v->ebo_surf);
    glGenVertexArrays(1, &v->vao_solid); glGenBuffers(1, &v->vbo_solid);
    glGenVertexArrays(1, &v->vao_scene); glGenBuffers(1, &v->vbo_scene);
    glGenVertexArrays(1, &v->vao_sides); glGenBuffers(1, &v->vbo_sides); glGenBuffers(1, &v->ebo_sides);
    glGenVertexArrays(1, &v->vao_mark); glGenBuffers(1, &v->vbo_mark);
    glGenVertexArrays(1, &v->vao_obj); glGenBuffers(1, &v->vbo_obj);
    glBindVertexArray(v->vao_obj);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_obj);
    glBufferData(GL_ARRAY_BUFFER, OBJ_MAX * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glBindVertexArray(v->vao_mark);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_mark);
    glBufferData(GL_ARRAY_BUFFER, MARK_MAX * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glBindVertexArray(0);

    glGenVertexArrays(1, &v->vao_caus); glGenBuffers(1, &v->vbo_caus); glGenBuffers(1, &v->ebo_caus);
    glGenVertexArrays(1, &v->vao_res); glGenBuffers(1, &v->vbo_res); glGenBuffers(1, &v->ebo_res);

    glBindVertexArray(v->vao_solid);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_solid);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(6 * sizeof(float)));
    glBindVertexArray(v->vao_scene);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_scene);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(6 * sizeof(float)));
    glBindVertexArray(0);
    v->scene_dirty = 1;

    build_surface(v);
    build_sides(v);
    if (v->shape == 0) build_caustic_mesh(v, nx, ny, nx / 6, ny / 6);
    else {
        /* the mirrored rings reach nr/6 cells past the rim; give the Cartesian side the same margin */
        v->hc_pad = (int)ceilf((float)(ny / 6) / (float)ny * (float)v->lm_w * 0.5f) + 1;
        build_caustic_mesh(v, v->lm_w, v->lm_h, v->hc_pad, v->hc_pad);
        build_resample_mesh(v);
    }

    glGenTextures(1, &v->tex_h);
    glBindTexture(GL_TEXTURE_2D, v->tex_h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, nx, ny, 0, GL_RED, GL_FLOAT, NULL);

    glGenTextures(1, &v->tex_lm);
    glBindTexture(GL_TEXTURE_2D, v->tex_lm);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* try a half-float render target for the GPU caustic pass; fall back to R8 + CPU splat */
    v->gpu_caustics = 0;
    if (!cpu_caustics) {
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, v->lm_w, v->lm_h, 0, GL_RED, GL_HALF_FLOAT, NULL);
        glGenFramebuffers(1, &v->fbo_lm);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_lm);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->tex_lm, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (st == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR) v->gpu_caustics = 1;
        else { glDeleteFramebuffers(1, &v->fbo_lm); v->fbo_lm = 0; fprintf(stderr, "no half-float render target; caustics on the CPU\n"); }
    }
    if (v->gpu_caustics && v->shape == 1) {
        glGenTextures(1, &v->tex_hc);
        glBindTexture(GL_TEXTURE_2D, v->tex_hc);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, v->lm_w + 2 * v->hc_pad, v->lm_h + 2 * v->hc_pad, 0, GL_RED, GL_HALF_FLOAT, NULL);
        glGenFramebuffers(1, &v->fbo_hc);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_hc);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->tex_hc, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (v->gpu_caustics) {
        /* blurred copy, the one the lighting reads */
        glGenTextures(1, &v->tex_lm2);
        glBindTexture(GL_TEXTURE_2D, v->tex_lm2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, v->lm_w, v->lm_h, 0, GL_RED, GL_HALF_FLOAT, NULL);
        glGenFramebuffers(1, &v->fbo_lm2);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_lm2);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->tex_lm2, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (!v->gpu_caustics) {
        glBindTexture(GL_TEXTURE_2D, v->tex_lm);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, v->lm_w, v->lm_h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    }
    printf("caustics: %s\n", v->gpu_caustics ? "GPU" : "CPU");

    /* the sun's shadow map, for the setting: a depth texture, read by hand (3 x 3 PCF) */
    v->sh_size = 2048;
    glGenTextures(1, &v->tex_sh);
    glBindTexture(GL_TEXTURE_2D, v->tex_sh);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, v->sh_size, v->sh_size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glGenFramebuffers(1, &v->fbo_sh);
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_sh);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, v->tex_sh, 0);
    { const GLenum none = GL_NONE; glDrawBuffers(1, &none); glReadBuffer(GL_NONE); }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "no depth render target: the setting will have no shadows\n");
        glDeleteFramebuffers(1, &v->fbo_sh); v->fbo_sh = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* the reflection: colour texture and depth, sized on first use */
    glGenTextures(1, &v->tex_rf);
    glGenFramebuffers(1, &v->fbo_rf);
    glGenRenderbuffers(1, &v->rb_rf);

    glGenTextures(1, &v->tex_ovl);
    glBindTexture(GL_TEXTURE_2D, v->tex_ovl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    v->lm_acc = malloc(sizeof(float) * (size_t)v->lm_w * v->lm_h);
    v->lm_tmp = malloc(sizeof(float) * (size_t)v->lm_w * v->lm_h);
    v->lm8 = malloc((size_t)v->lm_w * v->lm_h);
    if (v->shape == 1) {
        v->cs_tab = malloc(sizeof(float) * 2 * (size_t)nx);
        for (int j = 0; j < nx; j++) { v->cs_tab[2 * j] = cosf(2.0f * (float)M_PI * j / nx); v->cs_tab[2 * j + 1] = sinf(2.0f * (float)M_PI * j / nx); }
    }
    v->ovl_dirty = 1;
    v->video_ok = -1;
    v->yaw = 35.0f; v->pitch = 42.0f;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) fprintf(stderr, "GL error during init: 0x%x\n", err);
    return v;
}

void view3d_destroy(view3d *v)
{
    if (!v) return;
    free(v->lm_acc); free(v->lm_tmp); free(v->lm8); free(v->cs_tab); free(v->ovl.rgba); free(v->capture);
    if (v->fbo_lm) glDeleteFramebuffers(1, &v->fbo_lm);
    if (v->fbo_lm2) glDeleteFramebuffers(1, &v->fbo_lm2);
    if (v->fbo_hc) glDeleteFramebuffers(1, &v->fbo_hc);
    if (v->fbo_sh) glDeleteFramebuffers(1, &v->fbo_sh);
    if (v->fbo_rf) glDeleteFramebuffers(1, &v->fbo_rf);
    if (v->rb_rf) glDeleteRenderbuffers(1, &v->rb_rf);
    video_free(v);
    if (v->ctx) SDL_GL_DeleteContext(v->ctx);
    free(v);
}

void view3d_set_pool(view3d *v, const wave *w)
{
    v->Lx = (float)w->Lx; v->Ly = (float)w->Ly; v->depth = (float)w->depth;
    if (w->shape == WAVE_DISK) { v->dx = (float)w->dth; v->dy = (float)w->dr; }
    else { v->dx = (float)w->dx; v->dy = (float)w->dy; }
    v->cx = 0.5f * v->Lx; v->cz = 0.5f * v->Ly;
    build_solid(v);
    v->scene_dirty = 1;
}

void view3d_reset_camera(view3d *v, const wave *w)
{
    view3d_set_pool(v, w);
    v->yaw = 35.0f; v->pitch = 42.0f;
    v->dist = 1.5f * (v->Lx > v->Ly ? v->Lx : v->Ly);
}

void view3d_set_camera(view3d *v, float yaw_deg, float pitch_deg, float dist_rel)
{
    v->yaw = yaw_deg; v->pitch = pitch_deg;
    v->dist = dist_rel * (v->Lx > v->Ly ? v->Lx : v->Ly);
}

void view3d_get_camera(const view3d *v, float *yaw_deg, float *pitch_deg, float *dist_rel)
{
    if (yaw_deg) *yaw_deg = v->yaw;
    if (pitch_deg) *pitch_deg = v->pitch;
    if (dist_rel) *dist_rel = v->dist / (v->Lx > v->Ly ? v->Lx : v->Ly);
}

void view3d_orbit(view3d *v, float dyaw, float dpitch)
{
    v->yaw += dyaw;
    v->pitch += dpitch;
    if (v->pitch > 89.0f) v->pitch = 89.0f;
    if (v->pitch < -89.0f) v->pitch = -89.0f;
}

void view3d_zoom(view3d *v, float f)
{
    float L = v->Lx > v->Ly ? v->Lx : v->Ly;
    v->dist *= f;
    if (v->dist < 0.15f * L) v->dist = 0.15f * L;
    if (v->dist > 12.0f * L) v->dist = 12.0f * L;
}

static void update_camera(view3d *v)
{
    SDL_GL_GetDrawableSize(v->win, &v->W, &v->H);
    if (v->H < 1) v->H = 1;
    v->aspect = (float)v->W / (float)v->H;
    v->tanhalf = tanf(0.5f * 45.0f * (float)M_PI / 180.0f);
    float yaw_d = v->yaw, pitch_d = v->pitch;
    if (v->video_on) {
        /* hand-held: a slow wander of a few tenths of a degree, and a faint tremor */
        const float t = v->time;
        yaw_d   += 0.22f * sinf(0.37f * t) + 0.11f * sinf(0.83f * t + 1.3f) + 0.035f * sinf(2.3f * t + 0.4f);
        pitch_d += 0.15f * sinf(0.29f * t + 2.0f) + 0.08f * sinf(0.71f * t + 0.7f) + 0.03f * sinf(2.1f * t + 2.2f);
    }
    float yaw = yaw_d * (float)M_PI / 180.0f, pitch = pitch_d * (float)M_PI / 180.0f;
    v->cam[0] = v->cx + v->dist * cosf(pitch) * sinf(yaw);
    v->cam[1] = v->dist * sinf(pitch);
    v->cam[2] = v->cz + v->dist * cosf(pitch) * cosf(yaw);
    v->fwd[0] = v->cx - v->cam[0]; v->fwd[1] = 0.0f - v->cam[1]; v->fwd[2] = v->cz - v->cam[2];
    v3norm(v->fwd);
    const float worldup[3] = { 0, 1, 0 };
    v3cross(v->fwd, worldup, v->right); v3norm(v->right);
    v3cross(v->right, v->fwd, v->up);   v3norm(v->up);
}

void view3d_listen(const view3d *v, double x, double z, double *pan, double *att)
{
    float d[3] = { (float)x - v->cam[0], 0.0f - v->cam[1], (float)z - v->cam[2] };
    float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (dist < 1e-6f) { *pan = 0; *att = 1; return; }
    float rx = d[0] * v->right[0] + d[2] * v->right[2];
    *pan = rx / dist;
    float L = v->Lx > v->Ly ? v->Lx : v->Ly;
    float a = 1.2f * L / dist;                 /* falls off past about a basin length */
    *att = a > 1.0f ? 1.0f : (a < 0.15f ? 0.15f : a);
}

int view3d_pick(const view3d *v, int mx, int my, double *x, double *y)
{
    int ww, wh;
    SDL_GetWindowSize(v->win, &ww, &wh);
    if (ww < 1 || wh < 1) return 0;
    float nx = 2.0f * (float)mx / (float)ww - 1.0f, ny = 1.0f - 2.0f * (float)my / (float)wh;
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = v->fwd[k] + v->tanhalf * (nx * v->aspect * v->right[k] + ny * v->up[k]);
    if (fabsf(d[1]) < 1e-6f) return 0;
    float t = -v->cam[1] / d[1];
    if (t <= 0.0f) return 0;
    float px = v->cam[0] + t * d[0], pz = v->cam[2] + t * d[2];
    if (v->shape == 1) {
        float R = 0.5f * v->Lx, ex = px - R, ez = pz - R;
        if (ex * ex + ez * ez > R * R) return 0;
    } else if (px < 0.0f || px > v->Lx || pz < 0.0f || pz > v->Ly) return 0;
    *x = px; *y = pz;
    return 1;
}

/* ------------------------------------------------------------------ caustics */
/* edge mode: 0 = the walls shadow the floor (opaque); 1 = no walls, the surface is
 * continued past the wall planes by its even (mirror) extension - which is exactly
 * what the cosine basis represents - so the caustic pattern runs to the edge;
 * 2 = glass walls, sunlight comes through them and lights the shadow strip flat. */
static inline int mirror_idx(int i, int n) { return i < 0 ? -1 - i : (i >= n ? 2 * n - 1 - i : i); }

static void compute_caustics(view3d *v, const wave *w, const view3d_params *p, int edge)
{
    const int nx = v->nx, ny = v->ny, lw = v->lm_w, lh = v->lm_h;
    const float dx = v->dx, dy = v->dy, gain = p->gain, depth = v->depth;
    const float *e = w->eta;
    float *acc = v->lm_acc;
    memset(acc, 0, sizeof(float) * (size_t)lw * lh);
    const float lcx = v->Lx / (float)lw, lcz = v->Ly / (float)lh;    /* light map cell size */

    /* incident light direction (downwards) */
    float I[3] = { -p->sun[0], -p->sun[1], -p->sun[2] };
    v3norm(I);
    const float eta = 1.0f / 1.333f;

    /* flat-surface refraction: where a floor point's light comes from */
    const float c0 = -I[1];
    const float k0 = 1.0f - eta * eta * (1.0f - c0 * c0);
    const float m0 = eta * c0 - sqrtf(k0 > 0 ? k0 : 0);
    const float Tx0 = eta * I[0], Ty0 = eta * I[1] + m0, Tz0 = eta * I[2];
    const float ox = -depth * Tx0 / Ty0, oz = -depth * Tz0 / Ty0;   /* floor = surface + (ox, oz) */

    if (edge == 2) {
        /* glass walls: the strip a wall would shadow is lit flat through the glass */
        for (int j = 0; j < lh; j++)
            for (int i = 0; i < lw; i++) {
                const float sx = ((float)i + 0.5f) * lcx - ox, sz = ((float)j + 0.5f) * lcz - oz;
                int inside;
                if (v->shape == 1) { float R = 0.5f * v->Lx, ex = sx - R, ez = sz - R; inside = ex * ex + ez * ez <= R * R; }
                else inside = sx >= 0.0f && sx <= v->Lx && sz >= 0.0f && sz <= v->Ly;
                if (!inside) acc[i + lw * j] = 1.0f;
            }
    }

    /* the pad of mirrored cells outside the wall used when there is no wall */
    int px = 0, pz = 0;
    if (edge == 1) {
        if (v->shape == 0) { px = (int)(fabsf(ox) / dx) + 3; pz = (int)(fabsf(oz) / dy) + 3; }
        else { px = 0; pz = (int)(sqrtf(ox * ox + oz * oz) / dy) + 3; }     /* rings only */
        if (px > nx - 1) px = nx - 1;
        if (pz > ny - 1) pz = ny - 1;
    }

    const int j0 = (v->shape == 0) ? -pz : 0;      /* disk: mirror only outward, past the rim */
    for (int j = j0; j < ny + pz; j++) {
        for (int i = -px; i < nx + px; i++) {
            float X, Z, hx, hz, h, area;
            if (v->shape == 0) {
                const int jm = mirror_idx(j - 1, ny), jc = mirror_idx(j, ny), jp = mirror_idx(j + 1, ny);
                const int im = mirror_idx(i - 1, nx), ic = mirror_idx(i, nx), ip = mirror_idx(i + 1, nx);
                h  = e[ic + nx * jc] * gain;
                hx = (e[ip + nx * jc] - e[im + nx * jc]) * gain / (2.0f * dx);
                hz = (e[ic + nx * jp] - e[ic + nx * jm]) * gain / (2.0f * dy);
                X = ((float)i + 0.5f) * dx; Z = ((float)j + 0.5f) * dy;
                area = dx * dy;
            } else {
                /* polar: j = ring, i = angle.  Rings mirror at the wall; through the centre
                 * the inner neighbour of ring 0 is ring 0 on the opposite side. */
                const int nt = nx, nr = ny;
                const int ic = i, ipl = (i + 1) % nt, iml = (i + nt - 1) % nt;
                const int jc = mirror_idx(j, nr), jp = mirror_idx(j + 1, nr);
                int jm = j - 1, imn = ic;
                if (jm < 0) { jm = 0; imn = (ic + nt / 2) % nt; }
                jm = mirror_idx(jm, nr);
                const float dr = v->dy, dth = v->dx;
                const float rs = ((float)j + 0.5f) * dr;
                h  = e[ic + nt * jc] * gain;
                const float hr = (e[ic + nt * jp] - e[imn + nt * jm]) * gain / (2.0f * dr);
                const float ht = (e[ipl + nt * jc] - e[iml + nt * jc]) * gain / (2.0f * dth * rs);
                const float ct = v->cs_tab[2 * ic], st = v->cs_tab[2 * ic + 1];
                hx = hr * ct - ht * st;
                hz = hr * st + ht * ct;
                const float R = 0.5f * v->Lx;
                X = R + rs * ct; Z = R + rs * st;
                area = rs * dr * dth;
            }
            float N[3] = { -hx, 1.0f, -hz };
            v3norm(N);
            /* refract */
            const float c = -(N[0]*I[0] + N[1]*I[1] + N[2]*I[2]);
            const float k = 1.0f - eta * eta * (1.0f - c * c);
            if (k < 0.0f) continue;
            const float m = eta * c - sqrtf(k);
            float T[3] = { eta * I[0] + m * N[0], eta * I[1] + m * N[1], eta * I[2] + m * N[2] };
            if (T[1] >= -1e-4f) continue;
            const float s = (-depth - h) / T[1];
            const float qx = X + s * T[0];
            const float qz = Z + s * T[2];
            /* bilinear splat into light-map cell space, weight = cell area / light-map cell area */
            const float wgt0 = area / (lcx * lcz);
            const float gx = qx / lcx - 0.5f, gz = qz / lcz - 0.5f;
            const int ix = (int)floorf(gx), iz = (int)floorf(gz);
            const float ax = gx - (float)ix, az = gz - (float)iz;
            const float wgt[4] = { (1 - ax) * (1 - az), ax * (1 - az), (1 - ax) * az, ax * az };
            const int cx[4] = { ix, ix + 1, ix, ix + 1 }, cz[4] = { iz, iz, iz + 1, iz + 1 };
            for (int q = 0; q < 4; q++)
                if (cx[q] >= 0 && cx[q] < lw && cz[q] >= 0 && cz[q] < lh)
                    acc[cx[q] + lw * cz[q]] += wgt0 * wgt[q];
        }
    }
    /* 3x3 binomial blur, then to 8 bit with 1.0 -> 64 */
    float *tmp = v->lm_tmp;
    for (int j = 0; j < lh; j++)
        for (int i = 0; i < lw; i++) {
            const int im = i > 0 ? i - 1 : i, ip = i < lw - 1 ? i + 1 : i;
            tmp[i + lw * j] = 0.25f * (acc[im + lw * j] + 2.0f * acc[i + lw * j] + acc[ip + lw * j]);
        }
    for (int j = 0; j < lh; j++) {
        const int jm = j > 0 ? j - 1 : j, jp = j < lh - 1 ? j + 1 : j;
        for (int i = 0; i < lw; i++) {
            float val = 0.25f * (tmp[i + lw * jm] + 2.0f * tmp[i + lw * j] + tmp[i + lw * jp]);
            if (val > 4.0f) val = 4.0f;
            if (val < 0.0f) val = 0.0f;
            v->lm8[i + lw * j] = (uint8_t)(val * 63.75f + 0.5f);
        }
    }
    if (getenv("POND_DEBUG")) {
        int mn = 255, mx = 0; double mean = 0;
        for (int i = 0; i < lw * lh; i++) { if (v->lm8[i] < mn) mn = v->lm8[i]; if (v->lm8[i] > mx) mx = v->lm8[i]; mean += v->lm8[i]; }
        printf("lightmap min %d max %d mean %.1f\n", mn, mx, mean / (lw * lh));
    }
    glBindTexture(GL_TEXTURE_2D, v->tex_lm);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lw, lh, GL_RED, GL_UNSIGNED_BYTE, v->lm8);
}

static void set_common(view3d *v, GLuint p, const view3d_params *prm, const float *vp);

/* GPU caustics: rasterise the refracted surface mesh into the light map (see vs_caus) */
static void gpu_caustics(view3d *v, const view3d_params *p, int edge)
{
    float I[3] = { -p->sun[0], -p->sun[1], -p->sun[2] };
    v3norm(I);
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_lm);
    glViewport(0, 0, v->lm_w, v->lm_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (edge == 2) {
        /* flat-surface refraction offset: floor = surface + (ox, oz) */
        const float eta = 1.0f / 1.333f, c0 = -I[1];
        const float k0 = 1.0f - eta * eta * (1.0f - c0 * c0);
        const float m0 = eta * c0 - sqrtf(k0 > 0 ? k0 : 0);
        const float Tx = eta * I[0], Ty = eta * I[1] + m0, Tz = eta * I[2];
        glUseProgram(v->p_fill);
        glUniform2f(U(v->p_fill, "u_L"), v->Lx, v->Ly);
        glUniform2f(U(v->p_fill, "u_offset"), -v->depth * Tx / Ty, -v->depth * Tz / Ty);
        glUniform1i(U(v->p_fill, "u_shape"), v->shape);
        glBindVertexArray(v->vao_empty);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    const float lcx = v->Lx / (float)v->lm_w, lcz = v->Ly / (float)v->lm_h;
    if (v->shape == 1) {
        /* pass A: polar height field -> Cartesian texture over the bounding square */
        const int W = v->lm_w + 2 * v->hc_pad, Hh = v->lm_h + 2 * v->hc_pad;
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_hc);
        glViewport(0, 0, W, Hh);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(v->p_res);
        glUniform1i(U(v->p_res, "u_height"), 0);
        glUniform2i(U(v->p_res, "u_n"), v->nx, v->ny);
        glUniform2f(U(v->p_res, "u_L"), v->Lx, v->Ly);
        glUniform1f(U(v->p_res, "u_gain"), p->gain);
        glUniform1f(U(v->p_res, "u_ext"), (float)v->hc_pad * lcx);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, v->tex_h);
        glBindVertexArray(v->vao_res);
        glDrawElements(GL_TRIANGLES, v->n_res_idx, GL_UNSIGNED_INT, (void *)0);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_lm);
        glViewport(0, 0, v->lm_w, v->lm_h);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    set_common(v, v->p_caus, p, NULL);
    glUniform3f(U(v->p_caus, "u_incident"), I[0], I[1], I[2]);
    glUniform2f(U(v->p_caus, "u_lcell"), lcx, lcz);
    glActiveTexture(GL_TEXTURE0);
    if (v->shape == 1) {
        /* pass B on the resampled field: cells are texels, the rectangle machinery applies;
         * sources outside the basin (or its mirrored pad) are cut off in the fragment shader */
        glBindTexture(GL_TEXTURE_2D, v->tex_hc);
        glUniform2i(U(v->p_caus, "u_n"), v->lm_w + 2 * v->hc_pad, v->lm_h + 2 * v->hc_pad);
        glUniform2i(U(v->p_caus, "u_ioff"), v->hc_pad, v->hc_pad);
        glUniform2f(U(v->p_caus, "u_d"), lcx, lcz);
        glUniform1f(U(v->p_caus, "u_gain"), 1.0f);
        const float R = 0.5f * v->Lx;
        glUniform1f(U(v->p_caus, "u_mask_r"), edge == 1 ? R + (float)v->res_pad * (R / (float)v->ny) : R);
    } else {
        glBindTexture(GL_TEXTURE_2D, v->tex_h);
        glUniform2i(U(v->p_caus, "u_ioff"), 0, 0);
        glUniform1f(U(v->p_caus, "u_mask_r"), 0.0f);
    }
    glBindVertexArray(v->vao_caus);
    glDrawElements(GL_TRIANGLES, edge == 1 ? v->n_caus_all : v->n_caus_int, GL_UNSIGNED_INT, (void *)0);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    if (getenv("POND_DEBUG")) {
        float *buf = malloc(sizeof(float) * (size_t)v->lm_w * v->lm_h);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, v->lm_w, v->lm_h, GL_RED, GL_FLOAT, buf);
        double mean = 0; float mx = 0; int nz = 0;
        for (int i = 0; i < v->lm_w * v->lm_h; i++) { mean += buf[i]; if (buf[i] > mx) mx = buf[i]; if (buf[i] > 0) nz++; }
        printf("gpu lightmap: mean %.3f max %.3f nonzero %.2f%% (edge %d, %d idx)\n", mean / (v->lm_w * v->lm_h), mx, 100.0 * nz / (v->lm_w * v->lm_h), edge, edge == 1 ? v->n_caus_all : v->n_caus_int);
        free(buf);
    }
    /* blur into the texture the lighting reads */
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_lm2);
    glUseProgram(v->p_blur);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->tex_lm);
    glUniform1i(U(v->p_blur, "u_tex"), 0);
    glBindVertexArray(v->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    if (getenv("POND_DEBUG")) {
        float *buf = malloc(sizeof(float) * (size_t)v->lm_w * v->lm_h);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, v->lm_w, v->lm_h, GL_RED, GL_FLOAT, buf);
        double mean = 0; float mx = 0;
        for (int i = 0; i < v->lm_w * v->lm_h; i++) { mean += buf[i]; if (buf[i] > mx) mx = buf[i]; }
        printf("blurred: mean %.3f max %.3f\n", mean / (v->lm_w * v->lm_h), mx);
        if (getenv("POND_LMDUMP")) {
            FILE *fp = fopen(getenv("POND_LMDUMP"), "wb");
            if (fp) {
                fprintf(fp, "P5\n%d %d\n255\n", v->lm_w, v->lm_h);
                for (int i = 0; i < v->lm_w * v->lm_h; i++) { float q = buf[i] * 64.0f; fputc(q > 255 ? 255 : (int)q, fp); }
                fclose(fp);
            }
        }
        free(buf);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ------------------------------------------------------------------ overlay */
void view3d_set_overlay(view3d *v, const char *hud, const char *const *help, int nhelp,
                        int show_help, int show_hud)
{
    if (hud) { strncpy(v->hud, hud, sizeof v->hud - 1); v->hud[sizeof v->hud - 1] = 0; }
    v->help = help; v->nhelp = nhelp; v->show_help = show_help; v->show_hud = show_hud;
    v->ovl_dirty = 1;
}

static void draw_overlay(view3d *v)
{
    if (v->W != v->ovl_w || v->H != v->ovl_h) {
        free(v->ovl.rgba);
        v->ovl.w = v->ovl_w = v->W; v->ovl.h = v->ovl_h = v->H;
        v->ovl.rgba = malloc((size_t)v->W * v->H * 4);
        v->ovl_dirty = 1;
        glBindTexture(GL_TEXTURE_2D, v->tex_ovl);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, v->W, v->H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    if (v->ovl_dirty) {
        canvas *c = &v->ovl;
        canvas_clear(c);
        /* glyph scale from the drawable height, then shrunk until the text fits the width */
        int base = (v->H + 250) / 500;
        if (base < 1) base = 1;
        if (base > 4) base = 4;

        /* HUD: one or more lines separated by newlines; d hides it, and the help
         * panel stands in for it while that is up */
        if (v->show_hud && !(v->show_help && v->help)) {
            char tmp[640];
            strncpy(tmp, v->hud, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
            int nl = 1, maxlen = 0;
            for (char *q = tmp; *q; q++) if (*q == '\n') nl++;
            char *line = tmp;
            for (int k = 0; k < nl; k++) {
                char *e = strchr(line, '\n'); if (e) *e = 0;
                int len = (int)strlen(line); if (len > maxlen) maxlen = len;
                line = e ? e + 1 : line + strlen(line);
            }
            int scale = base;
            while (scale > 1 && scale * (8 * maxlen + 16) > v->W) scale--;
            int lh = 10 * scale, pad = 8 * scale;
            canvas_fill(c, pad - 4 * scale, pad - 3 * scale, 8 * scale * maxlen + 8 * scale, nl * lh + 2 * scale, 0, 0, 0, 120);
            strncpy(tmp, v->hud, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
            line = tmp;
            for (int k = 0; k < nl; k++) {
                char *e = strchr(line, '\n'); if (e) *e = 0;
                canvas_text(c, pad, pad + k * lh, scale, 235, 235, 235, 255, line);
                line = e ? e + 1 : line + strlen(line);
            }
        }
        if (v->show_help && v->help) {
            int maxlen = 0;
            for (int k = 0; k < v->nhelp; k++) { int len = (int)strlen(v->help[k]); if (len > maxlen) maxlen = len; }
            int scale = base;
            while (scale > 1 && (scale * (8 * maxlen + 16) > v->W || scale * (10 * v->nhelp + 16) > v->H)) scale--;
            int lh = 10 * scale, pad = 8 * scale;
            int bw = 8 * scale * maxlen + 2 * pad, bh = v->nhelp * lh + 2 * pad;
            int bx = (v->W - bw) / 2, by = (v->H - bh) / 2;
            if (bx < 0) bx = 0;
            if (by < 0) by = 0;
            canvas_fill(c, bx, by, bw, bh, 8, 10, 14, 200);
            canvas_fill(c, bx, by, bw, scale, 120, 160, 200, 255);
            for (int k = 0; k < v->nhelp; k++)
                canvas_text(c, bx + pad, by + pad + k * lh, scale, 230, 232, 236, 255, v->help[k]);
        } else if (v->show_hud) {
            const char *hint = "h / F1: help";
            int scale = base, lh = 10 * scale, pad = 8 * scale;
            int hw = text_width(hint, scale);
            canvas_fill(c, v->W - hw - pad - 4 * scale, v->H - lh - pad - scale, hw + 8 * scale, lh + 2 * scale, 0, 0, 0, 100);
            canvas_text(c, v->W - hw - pad, v->H - lh - pad, scale, 200, 205, 210, 255, hint);
        }
        glBindTexture(GL_TEXTURE_2D, v->tex_ovl);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->W, v->H, GL_RGBA, GL_UNSIGNED_BYTE, c->rgba);
        v->ovl_dirty = 0;
    }
    glUseProgram(v->p_ovl);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->tex_ovl);
    glUniform1i(U(v->p_ovl, "u_tex"), 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(v->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

/* ------------------------------------------------------------------ frame */
/* ------------------------------------------------- the wavemaker's outline
 * A closed strip of thin quads lying just above the mean surface: the footprint
 * of the forcing, out to its 1/e contour in both directions.  Rebuilt each frame
 * from a few dozen vertices, which is nothing. */
static void mark_piece(float *b, int *n, float x0, float z0, float x1, float z1, float h, float y)
{
    float dx = x1 - x0, dz = z1 - z0;
    const float l = sqrtf(dx * dx + dz * dz);
    if (l < 1e-9f || *n + 6 > MARK_MAX) return;
    dx /= l; dz /= l;
    x0 -= dx * h; z0 -= dz * h;              /* overshoot the ends so the corners close */
    x1 += dx * h; z1 += dz * h;
    const float px = -dz * h, pz = dx * h;
    const float q[4][2] = { { x0 + px, z0 + pz }, { x1 + px, z1 + pz },
                            { x1 - px, z1 - pz }, { x0 - px, z0 - pz } };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        b[*n * 3 + 0] = q[tri[i]][0];
        b[*n * 3 + 1] = y;
        b[*n * 3 + 2] = q[tri[i]][1];
        (*n)++;
    }
}

/* a segment in short pieces, so the strip can follow the water it is lifted onto
 * (a long straight piece would cut through the crests between its ends) */
static void mark_seg(float *b, int *n, float x0, float z0, float x1, float z1, float h, float y)
{
    const float l = sqrtf((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
    int k = (int)(l / (6.0f * h)) + 1;
    if (k > 24) k = 24;
    for (int i = 0; i < k; i++) {
        const float a = (float)i / k, c = (float)(i + 1) / k;
        mark_piece(b, n, x0 + (x1 - x0) * a, z0 + (z1 - z0) * a, x0 + (x1 - x0) * c, z0 + (z1 - z0) * c, h, y);
    }
}

static void build_paddle_mark(view3d *v, const view3d_params *p)
{
    static float buf[MARK_MAX * 3];
    int n = 0;
    const float Lmax = v->Lx > v->Ly ? v->Lx : v->Ly;
    const float h = 0.004f * Lmax;                  /* half the line thickness */
    const float y = 0.0f;                           /* the shader lifts it onto the water */
    /* the forcing strip is a fraction of a percent of the basin deep, which would
     * draw as one line: give the outline a floor so it reads as a bar at the wall */
    float wd = p->paddle_width;
    if (wd < 0.02f * Lmax) wd = 0.02f * Lmax;

    if (v->shape == 1) {
        const float R = 0.5f * v->Lx, cx = R, cz = R;
        const float r0 = R - wd > 0.0f ? R - wd : 0.0f, r1 = R;
        const int full = p->paddle_span >= 1.0f;
        const float th0 = 6.28318530718f * p->paddle_pos, sig = 3.14159265f * p->paddle_span;
        const float a0 = full ? 0.0f : th0 - sig, a1 = full ? 6.28318530718f : th0 + sig;
        const int na = 48;
        for (int i = 0; i < na; i++) {
            const float t0 = a0 + (a1 - a0) * i / na, t1 = a0 + (a1 - a0) * (i + 1) / na;
            mark_seg(buf, &n, cx + r1 * cosf(t0), cz + r1 * sinf(t0), cx + r1 * cosf(t1), cz + r1 * sinf(t1), h, y);
            mark_seg(buf, &n, cx + r0 * cosf(t0), cz + r0 * sinf(t0), cx + r0 * cosf(t1), cz + r0 * sinf(t1), h, y);
        }
        if (!full) {
            mark_seg(buf, &n, cx + r0 * cosf(a0), cz + r0 * sinf(a0), cx + r1 * cosf(a0), cz + r1 * sinf(a0), h, y);
            mark_seg(buf, &n, cx + r0 * cosf(a1), cz + r0 * sinf(a1), cx + r1 * cosf(a1), cz + r1 * sinf(a1), h, y);
        }
    } else {
        const int wall = p->paddle_wall & 3, along_z = wall < 2;
        const float Lq = along_z ? v->Ly : v->Lx, Lp = along_z ? v->Lx : v->Ly;
        float v0 = 0.0f, v1 = Lq;
        if (p->paddle_span < 1.0f) {
            const float c = p->paddle_pos * Lq, sig = 0.5f * p->paddle_span * Lq;
            v0 = c - sig; v1 = c + sig;
            if (v0 < 0.0f) v0 = 0.0f;
            if (v1 > Lq) v1 = Lq;
        }
        const float u0 = (wall & 1) ? Lp - wd : 0.0f, u1 = (wall & 1) ? Lp : wd;
        /* (u, v) -> (x, z) */
        float c0[2], c1[2], c2[2], c3[2];
        if (along_z) { c0[0] = u0; c0[1] = v0; c1[0] = u1; c1[1] = v0; c2[0] = u1; c2[1] = v1; c3[0] = u0; c3[1] = v1; }
        else         { c0[1] = u0; c0[0] = v0; c1[1] = u1; c1[0] = v0; c2[1] = u1; c2[0] = v1; c3[1] = u0; c3[0] = v1; }
        mark_seg(buf, &n, c0[0], c0[1], c1[0], c1[1], h, y);
        mark_seg(buf, &n, c1[0], c1[1], c2[0], c2[1], h, y);
        mark_seg(buf, &n, c2[0], c2[1], c3[0], c3[1], h, y);
        mark_seg(buf, &n, c3[0], c3[1], c0[0], c0[1], h, y);
    }
    v->n_mark = n;
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_mark);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)n * 3 * sizeof(float), buf);
}

static void draw_paddle_mark(view3d *v, const view3d_params *p, const float *vp)
{
    if (!p->paddle) return;
    build_paddle_mark(v, p);
    if (v->n_mark <= 0) return;
    glUseProgram(v->p_mark);
    set_common(v, v->p_mark, p, vp);
    glUniform1f(U(v->p_mark, "u_lift"), 0.004f * (v->Lx > v->Ly ? v->Lx : v->Ly));
    glUniform3f(U(v->p_mark, "u_col"), 1.0f, 0.45f, 0.10f);
    glBindVertexArray(v->vao_mark);
    /* solid where it can be seen; and a ghost of it through whatever is in the way --
     * a wave crest, the near wall -- so it is never simply gone */
    glUniform1f(U(v->p_mark, "u_alpha"), 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, v->n_mark);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(U(v->p_mark, "u_alpha"), 0.3f);
    glDrawArrays(GL_TRIANGLES, 0, v->n_mark);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

/* ------------------------------------------------- things on the water
 * Small meshes in basin coordinates, each vertex lifted onto the surface at its
 * own (x, z) by the mark shader, so a hull heaves and tilts with the water
 * under it without any bookkeeping here. */
static int obj_tri(float *b, int n, const float *p0, const float *p1, const float *p2)
{
    if (n + 3 > OBJ_MAX) return n;
    memcpy(b + 3 * n, p0, 3 * sizeof(float)); memcpy(b + 3 * (n + 1), p1, 3 * sizeof(float)); memcpy(b + 3 * (n + 2), p2, 3 * sizeof(float));
    return n + 3;
}

/* a hull from stations: at each, a V of (port rail, keel, starboard rail) */
static int build_hull(float *b, int n, float cx, float cz, float hdg, float len, int deck_only)
{
    const float ex = cosf(hdg), ez = sinf(hdg);          /* along */
    const float px = -ez, pz = ex;                        /* across, to starboard */
    static const float st_u[6] = { 0.5f, 0.35f, 0.1f, -0.15f, -0.4f, -0.5f };
    static const float st_b[6] = { 0.0f, 0.10f, 0.17f, 0.18f, 0.15f, 0.12f };   /* half beam */
    static const float st_d[6] = { 0.01f, 0.05f, 0.07f, 0.07f, 0.06f, 0.05f };  /* keel depth */
    static const float st_f[6] = { 0.12f, 0.10f, 0.09f, 0.09f, 0.09f, 0.10f };  /* freeboard */
    float P[6][3][3];
    for (int i = 0; i < 6; i++) {
        const float u = st_u[i] * len, bb = st_b[i] * len;
        const float ox = cx + ex * u, oz = cz + ez * u;
        P[i][0][0] = ox - px * bb; P[i][0][1] = st_f[i] * len; P[i][0][2] = oz - pz * bb;
        P[i][1][0] = ox;           P[i][1][1] = -st_d[i] * len; P[i][1][2] = oz;
        P[i][2][0] = ox + px * bb; P[i][2][1] = st_f[i] * len; P[i][2][2] = oz + pz * bb;
    }
    for (int i = 0; i < 5; i++) {
        if (deck_only) {
            n = obj_tri(b, n, P[i][0], P[i + 1][0], P[i + 1][2]);
            n = obj_tri(b, n, P[i][0], P[i + 1][2], P[i][2]);
        } else {
            for (int side = 0; side < 2; side++) {
                const int r = side ? 2 : 0;
                n = obj_tri(b, n, P[i][r], P[i][1], P[i + 1][1]);
                n = obj_tri(b, n, P[i][r], P[i + 1][1], P[i + 1][r]);
            }
        }
    }
    if (!deck_only) {   /* the transom */
        n = obj_tri(b, n, P[5][0], P[5][1], P[5][2]);
    }
    return n;
}

/* a float: a cone standing in the water, its base a little below the surface */
static int build_float(float *b, int n, float cx, float cz, float size)
{
    const int ns = 10;
    const float r = 0.45f * size, top[3] = { cx, 1.1f * size, cz }, bot[3] = { cx, -0.25f * size, cz };
    for (int i = 0; i < ns; i++) {
        const float a0 = 6.2831853f * i / ns, a1 = 6.2831853f * (i + 1) / ns;
        const float q0[3] = { cx + r * cosf(a0), -0.25f * size, cz + r * sinf(a0) };
        const float q1[3] = { cx + r * cosf(a1), -0.25f * size, cz + r * sinf(a1) };
        n = obj_tri(b, n, q0, top, q1);
        n = obj_tri(b, n, q0, q1, bot);
    }
    return n;
}

/* a bubble: a small sphere at (cx, cz), its centre `depth` below the surface */
static int build_bubble(float *b, int n, float cx, float cz, float depth, float r)
{
    const int nu = 8, nv = 6;
    for (int j = 0; j < nv; j++) {
        const float t0 = 3.14159265f * j / nv, t1 = 3.14159265f * (j + 1) / nv;
        for (int i = 0; i < nu; i++) {
            const float a0 = 6.2831853f * i / nu, a1 = 6.2831853f * (i + 1) / nu;
            const float p00[3] = { cx + r * sinf(t0) * cosf(a0), -depth + r * cosf(t0), cz + r * sinf(t0) * sinf(a0) };
            const float p01[3] = { cx + r * sinf(t0) * cosf(a1), -depth + r * cosf(t0), cz + r * sinf(t0) * sinf(a1) };
            const float p10[3] = { cx + r * sinf(t1) * cosf(a0), -depth + r * cosf(t1), cz + r * sinf(t1) * sinf(a0) };
            const float p11[3] = { cx + r * sinf(t1) * cosf(a1), -depth + r * cosf(t1), cz + r * sinf(t1) * sinf(a1) };
            n = obj_tri(b, n, p00, p10, p11);
            n = obj_tri(b, n, p00, p11, p01);
        }
    }
    return n;
}

static void draw_objects(view3d *v, const view3d_params *p, const float *vp)
{
    if (!p->boat && p->nfloat <= 0 && p->nbub <= 0) return;
    static float buf[OBJ_MAX * 3];
    glUseProgram(v->p_mark);
    set_common(v, v->p_mark, p, vp);
    glUniform1f(U(v->p_mark, "u_lift"), 0.0f);
    glUniform1f(U(v->p_mark, "u_alpha"), 1.0f);
    glBindVertexArray(v->vao_obj);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo_obj);
    if (p->boat) {
        int n = build_hull(buf, 0, p->boat_x, p->boat_z, p->boat_hdg, p->boat_len, 0);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)n * 3 * sizeof(float), buf);
        glUniform3f(U(v->p_mark, "u_col"), 0.72f, 0.70f, 0.66f);
        glDrawArrays(GL_TRIANGLES, 0, n);
        n = build_hull(buf, 0, p->boat_x, p->boat_z, p->boat_hdg, p->boat_len, 1);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)n * 3 * sizeof(float), buf);
        glUniform3f(U(v->p_mark, "u_col"), 0.95f, 0.93f, 0.88f);
        glDrawArrays(GL_TRIANGLES, 0, n);
    }
    if (p->nfloat > 0) {
        int n = 0;
        for (int i = 0; i < p->nfloat && i < 12; i++) n = build_float(buf, n, p->float_x[i], p->float_z[i], p->float_size);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)n * 3 * sizeof(float), buf);
        glUniform3f(U(v->p_mark, "u_col"), 1.0f, 0.45f, 0.08f);
        glDrawArrays(GL_TRIANGLES, 0, n);
    }
    if (p->nbub > 0) {
        /* bubbles: under the surface, so seen through glass; translucent and bright */
        int n = 0;
        for (int i = 0; i < p->nbub && i < 64; i++) n = build_bubble(buf, n, p->bub_x[i], p->bub_z[i], p->bub_d[i], p->bub_r[i]);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)n * 3 * sizeof(float), buf);
        glUniform3f(U(v->p_mark, "u_col"), 0.85f, 0.95f, 1.0f);
        glUniform1f(U(v->p_mark, "u_alpha"), 0.7f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, n);
        glDisable(GL_BLEND);
    }
}

static void set_common(view3d *v, GLuint p, const view3d_params *prm, const float *vp)
{
    glUseProgram(p);
    GLint l;
    if (vp && (l = U(p, "u_vp")) >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, vp);
    if ((l = U(p, "u_lscale")) >= 0) glUniform1f(l, (v->gpu_caustics ? 1.0f : 4.0f) * (prm->night ? 0.5f : 1.0f));
    if ((l = U(p, "u_sun")) >= 0) glUniform3f(l, prm->sun[0], prm->sun[1], prm->sun[2]);
    if ((l = U(p, "u_day")) >= 0) glUniform1f(l, prm->day);
    if ((l = U(p, "u_dusk")) >= 0) glUniform1f(l, prm->dusk);
    if ((l = U(p, "u_night")) >= 0) glUniform1f(l, prm->night ? 1.0f : 0.0f);
    if ((l = U(p, "u_lk")) >= 0) glUniform1f(l, prm->lk);
    if ((l = U(p, "u_time")) >= 0) glUniform1f(l, v->time);
    if ((l = U(p, "u_sunny")) >= 0) glUniform1f(l, v->scene_on ? 1.0f : 0.0f);
    if ((l = U(p, "u_fog")) >= 0) glUniform1f(l, 260.0f);
    if ((l = U(p, "u_shadow")) >= 0) glUniform1i(l, 2);
    if ((l = U(p, "u_refl_tex")) >= 0) glUniform1i(l, 3);
    if ((l = U(p, "u_shadow_on")) >= 0) glUniform1f(l, (v->scene_on && v->sh_valid) ? 1.0f : 0.0f);
    if ((l = U(p, "u_lvp")) >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, v->lvp);
    if ((l = U(p, "u_sbias")) >= 0) glUniform1f(l, v->sbias);
    if ((l = U(p, "u_nosun")) >= 0) glUniform1f(l, 0.0f);
    if ((l = U(p, "u_mirror")) >= 0) glUniform1f(l, 0.0f);
    if ((l = U(p, "u_clip")) >= 0) glUniform1f(l, 0.0f);
    if ((l = U(p, "u_refl")) >= 0) glUniform1f(l, (v->scene_on && v->rf_w > 0) ? 1.0f : 0.0f);
    if ((l = U(p, "u_rd")) >= 0) glUniform1f(l, 12.0f);
    if ((l = U(p, "u_video")) >= 0) glUniform1f(l, v->video_on ? 1.0f : 0.0f);
    if ((l = U(p, "u_wind")) >= 0) glUniform1f(l, prm->wind_u);
    if ((l = U(p, "u_ngust")) >= 0) glUniform1i(l, prm->ngust < 8 ? prm->ngust : 8);
    if ((l = U(p, "u_gust")) >= 0) glUniform4fv(l, 8, &prm->gust[0][0]);
    if ((l = U(p, "u_gbase")) >= 0) glUniform1f(l, prm->gust_base);
    if ((l = U(p, "u_gnorm")) >= 0) glUniform1f(l, prm->gust_norm);
    if ((l = U(p, "u_linear")) >= 0) glUniform1f(l, v->video_on ? 1.0f : 0.0f);
    if ((l = U(p, "u_cam")) >= 0) glUniform3f(l, v->cam[0], v->cam[1], v->cam[2]);
    if ((l = U(p, "u_L")) >= 0) glUniform2f(l, v->Lx, v->Ly);
    if ((l = U(p, "u_depth")) >= 0) glUniform1f(l, v->depth);
    if ((l = U(p, "u_d")) >= 0) glUniform2f(l, v->dx, v->dy);
    if ((l = U(p, "u_gain")) >= 0) glUniform1f(l, prm->gain);
    if ((l = U(p, "u_n")) >= 0) glUniform2i(l, v->nx, v->ny);
    if ((l = U(p, "u_tile")) >= 0) glUniform1f(l, (v->Lx > v->Ly ? v->Lx : v->Ly) / 8.0f);
    if ((l = U(p, "u_style")) >= 0) glUniform1i(l, prm->floor_style);
    {
        const int m = prm->glass;
        if ((l = U(p, "u_walls")) >= 0) glUniform1i(l, m >= 1);
        if ((l = U(p, "u_floor")) >= 0) glUniform1i(l, m >= 3);
        if ((l = U(p, "u_extend")) >= 0) glUniform1i(l, m == 1);
    }
    if ((l = U(p, "u_mu")) >= 0) glUniform3f(l, 0.38f, 0.060f, 0.015f);
    if ((l = U(p, "u_shape")) >= 0) glUniform1i(l, v->shape);
    if ((l = U(p, "u_height")) >= 0) glUniform1i(l, 0);
    if ((l = U(p, "u_light")) >= 0) glUniform1i(l, 1);
}

/* The sun's shadow map: the casters seen from the sun, orthographically, fitted to
 * the box they occupy.  Redrawn only when the sun moves or the setting is rebuilt. */
static void shadow_pass(view3d *v, const view3d_params *p)
{
    if (!v->fbo_sh) return;
    const float *s = p->sun;
    if (v->sh_valid && fabsf(s[0] - v->sh_sun[0]) + fabsf(s[1] - v->sh_sun[1]) + fabsf(s[2] - v->sh_sun[2]) < 1e-5f) return;
    const float ax = 0.5f * v->Lx, az = 0.5f * v->Ly, pcx = ax, pcz = az;
    /* the casters' box: house, cabana, palms and their crowns */
    const float bx0 = pcx - ax - 14.0f, bx1 = pcx + ax + 17.0f, by0 = -1.0f, by1 = 14.0f, bz0 = pcz - az - 18.0f, bz1 = pcz + az + 12.0f;
    float fwd[3] = { -s[0], -s[1], -s[2] }, right[3], up[3];
    v3norm(fwd);
    float wup[3] = { 0, 1, 0 };
    if (fabsf(fwd[1]) > 0.99f) { wup[1] = 0; wup[2] = 1; }
    v3cross(fwd, wup, right); v3norm(right);
    v3cross(right, fwd, up); v3norm(up);
    const float eye[3] = { 0.5f * (bx0 + bx1), 0.5f * (by0 + by1), 0.5f * (bz0 + bz1) };
    float view[16], proj[16];
    mat_lookat(view, eye, fwd, right, up);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int c = 0; c < 8; c++) {
        const float P[3] = { (c & 1) ? bx1 : bx0, (c & 2) ? by1 : by0, (c & 4) ? bz1 : bz0 };
        for (int k = 0; k < 3; k++) {
            const float q = view[k] * P[0] + view[4 + k] * P[1] + view[8 + k] * P[2] + view[12 + k];
            if (q < lo[k]) lo[k] = q;
            if (q > hi[k]) hi[k] = q;
        }
    }
    /* the receivers lie below the casters' box, as far down as the pool floor: extend the far plane */
    const float n = -hi[2] - 1.0f, f = -lo[2] + v->depth + 30.0f;
    mat_ortho(proj, lo[0], hi[0], lo[1], hi[1], n, f);
    mat_mul(v->lvp, proj, view);
    v->sbias = 0.04f / (f - n) * 2.0f;          /* 4 cm, in [0,1] depth */
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_sh);
    glViewport(0, 0, v->sh_size, v->sh_size);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    glUseProgram(v->p_depth);
    glUniformMatrix4fv(U(v->p_depth, "u_vp"), 1, GL_FALSE, v->lvp);
    glBindVertexArray(v->vao_scene);
    glDrawArrays(GL_TRIANGLES, v->cast_off, v->cast_n);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    memcpy(v->sh_sun, s, sizeof v->sh_sun);
    v->sh_valid = 1;
}

/* The setting as the water mirrors it: the sky and the scene drawn through the camera
 * reflected in the plane of the still water, into a texture half the window's size. */
static void reflection_pass(view3d *v, const view3d_params *p, const float *vp, float *vpref)
{
    const int rw = v->W / 2 > 1 ? v->W / 2 : 1, rh = v->H / 2 > 1 ? v->H / 2 : 1;
    if (rw != v->rf_w || rh != v->rf_h) {
        glBindTexture(GL_TEXTURE_2D, v->tex_rf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindRenderbuffer(GL_RENDERBUFFER, v->rb_rf);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, rw, rh);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_rf);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->tex_rf, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, v->rb_rf);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "no reflection target: the water will mirror the sky only\n");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &v->fbo_rf); v->fbo_rf = 0;
            v->rf_w = v->rf_h = 0;
            return;
        }
        v->rf_w = rw; v->rf_h = rh;
    }
    /* y -> -y, then the camera's own view and projection */
    static const float S[16] = { 1,0,0,0, 0,-1,0,0, 0,0,1,0, 0,0,0,1 };
    mat_mul(vpref, vp, S);
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_rf);
    glViewport(0, 0, rw, rh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    /* the sky, mirrored, without the sun's disc (the water adds that back at full strength) */
    glDisable(GL_DEPTH_TEST);
    set_common(v, v->p_bg, p, vp);
    glUniform3f(U(v->p_bg, "u_fwd"), v->fwd[0], v->fwd[1], v->fwd[2]);
    glUniform3f(U(v->p_bg, "u_right"), v->right[0], v->right[1], v->right[2]);
    glUniform3f(U(v->p_bg, "u_up"), v->up[0], v->up[1], v->up[2]);
    glUniform1f(U(v->p_bg, "u_tanhalf"), v->tanhalf);
    glUniform1f(U(v->p_bg, "u_aspect"), v->aspect);
    glUniform1f(U(v->p_bg, "u_mirror"), 1.0f);
    glUniform1f(U(v->p_bg, "u_nosun"), 1.0f);
    glUniform1f(U(v->p_bg, "u_linear"), 0.0f);
    glBindVertexArray(v->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    /* the scene, from the mirrored eye, nothing below the water */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    const float cy = v->cam[1];
    v->cam[1] = -cy;
    set_common(v, v->p_scene, p, vpref);
    v->cam[1] = cy;
    glUniform1f(U(v->p_scene, "u_clip"), 1.0f);
    glUniform1f(U(v->p_scene, "u_nosun"), 1.0f);
    glUniform1f(U(v->p_scene, "u_linear"), 0.0f);
    glBindVertexArray(v->vao_scene);
    glDrawArrays(GL_TRIANGLES, 0, v->n_scene);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static GLuint float_tex(int w, int h, int mip)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    return t;
}
static int fbo_for(GLuint *fbo, GLuint tex)
{
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}
static void video_free(view3d *v)
{
    GLuint f[7] = { v->fbo_ms, v->fbo_hdr, v->fbo_bl[0], v->fbo_bl[1], v->fbo_ex[0], v->fbo_ex[1], 0 };
    GLuint t[5] = { v->tex_hdr, v->tex_bl[0], v->tex_bl[1], v->tex_ex[0], v->tex_ex[1] };
    GLuint r[3] = { v->rb_ms_c, v->rb_ms_d, v->rb_hdr_d };
    for (int i = 0; i < 6; i++) if (f[i]) glDeleteFramebuffers(1, &f[i]);
    for (int i = 0; i < 5; i++) if (t[i]) glDeleteTextures(1, &t[i]);
    for (int i = 0; i < 3; i++) if (r[i]) glDeleteRenderbuffers(1, &r[i]);
    v->fbo_ms = v->fbo_hdr = v->fbo_bl[0] = v->fbo_bl[1] = v->fbo_ex[0] = v->fbo_ex[1] = 0;
    v->tex_hdr = v->tex_bl[0] = v->tex_bl[1] = v->tex_ex[0] = v->tex_ex[1] = 0;
    v->rb_ms_c = v->rb_ms_d = v->rb_hdr_d = 0;
    v->hdr_w = v->hdr_h = 0;
}
/* the video look's targets, at the window's size; 0 if this GL cannot render to float */
static int video_setup(view3d *v)
{
    if (v->video_ok == 0) return 0;
    if (v->hdr_w == v->W && v->hdr_h == v->H && v->fbo_hdr) return 1;
    video_free(v);
    int ok = 1;
    const int W = v->W, H = v->H;
    v->tex_hdr = float_tex(W, H, 1);
    v->hdr_levels = 1;
    for (int m = W > H ? W : H; m > 1; m >>= 1) v->hdr_levels++;
    ok &= fbo_for(&v->fbo_hdr, v->tex_hdr);
    /* multisampled: a float colour and a depth buffer, resolved into the texture each frame */
    GLint maxs = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxs);
    v->ms_samples = maxs >= 4 ? 4 : 0;
    if (ok && v->ms_samples) {
        glGenRenderbuffers(1, &v->rb_ms_c);
        glBindRenderbuffer(GL_RENDERBUFFER, v->rb_ms_c);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, v->ms_samples, GL_RGBA16F, W, H);
        glGenRenderbuffers(1, &v->rb_ms_d);
        glBindRenderbuffer(GL_RENDERBUFFER, v->rb_ms_d);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, v->ms_samples, GL_DEPTH_COMPONENT24, W, H);
        glGenFramebuffers(1, &v->fbo_ms);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_ms);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, v->rb_ms_c);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, v->rb_ms_d);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &v->fbo_ms); v->fbo_ms = 0;
            v->ms_samples = 0;
        }
    }
    if (ok && !v->fbo_ms) {
        /* no multisampling: draw straight into the texture, with its own depth */
        glGenRenderbuffers(1, &v->rb_hdr_d);
        glBindRenderbuffer(GL_RENDERBUFFER, v->rb_hdr_d);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_hdr);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, v->rb_hdr_d);
        ok &= glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    v->bl_w = W / 4 > 1 ? W / 4 : 1; v->bl_h = H / 4 > 1 ? H / 4 : 1;
    for (int i = 0; i < 2 && ok; i++) { v->tex_bl[i] = float_tex(v->bl_w, v->bl_h, 0); ok &= fbo_for(&v->fbo_bl[i], v->tex_bl[i]); }
    for (int i = 0; i < 2 && ok; i++) {
        v->tex_ex[i] = float_tex(1, 1, 0); ok &= fbo_for(&v->fbo_ex[i], v->tex_ex[i]);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        fprintf(stderr, "no float render target: the video look is not available here\n");
        video_free(v);
        v->video_ok = 0;
        return 0;
    }
    v->hdr_w = W; v->hdr_h = H;
    if (v->video_ok < 0) printf("video look: %s\n", v->ms_samples ? "4x multisampled float" : "float, no multisampling");
    v->video_ok = 1;
    return 1;
}
static void fill_pass(view3d *v) { glBindVertexArray(v->vao_empty); glDrawArrays(GL_TRIANGLES, 0, 3); }
/* the frame as a camera hands it over: metered, bloomed, tone-mapped, through a lens, with grain */
static void video_post(view3d *v, float dt)
{
    const int W = v->W, H = v->H;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    if (v->fbo_ms) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, v->fbo_ms);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, v->fbo_hdr);
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->tex_hdr);
    glGenerateMipmap(GL_TEXTURE_2D);
    /* metering, eased: the exposure follows the light over about a second */
    const int nx = 1 - v->ex_i;
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_ex[nx]);
    glViewport(0, 0, 1, 1);
    glUseProgram(v->p_adapt);
    glUniform1i(U(v->p_adapt, "u_hdr"), 0);
    glUniform1i(U(v->p_adapt, "u_prev"), 1);
    glUniform1f(U(v->p_adapt, "u_lod"), (float)(v->hdr_levels > 6 ? v->hdr_levels - 6 : 0));
    glUniform1f(U(v->p_adapt, "u_rate"), 1.0f - expf(-dt / 0.9f));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, v->tex_ex[v->ex_i]);
    fill_pass(v);
    v->ex_i = nx;
    /* bloom: what is brighter than white, at a quarter size, blurred twice each way */
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_bl[0]);
    glViewport(0, 0, v->bl_w, v->bl_h);
    glUseProgram(v->p_bright);
    glUniform1i(U(v->p_bright, "u_hdr"), 0);
    glUniform1i(U(v->p_bright, "u_ex"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, v->tex_ex[v->ex_i]);
    fill_pass(v);
    glUseProgram(v->p_bloom);
    glUniform1i(U(v->p_bloom, "u_src"), 0);
    glActiveTexture(GL_TEXTURE0);
    for (int it = 0; it < 2; it++) {
        const float step = it == 0 ? 1.0f : 2.0f;
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_bl[1]);
        glBindTexture(GL_TEXTURE_2D, v->tex_bl[0]);
        glUniform2f(U(v->p_bloom, "u_dir"), step / (float)v->bl_w, 0.0f);
        fill_pass(v);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_bl[0]);
        glBindTexture(GL_TEXTURE_2D, v->tex_bl[1]);
        glUniform2f(U(v->p_bloom, "u_dir"), 0.0f, step / (float)v->bl_h);
        fill_pass(v);
    }
    /* the picture */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glUseProgram(v->p_post);
    glUniform1i(U(v->p_post, "u_hdr"), 0);
    glUniform1i(U(v->p_post, "u_bloom"), 1);
    glUniform1i(U(v->p_post, "u_ex"), 2);
    glUniform2f(U(v->p_post, "u_res"), (float)W, (float)H);
    glUniform1f(U(v->p_post, "u_time"), v->time);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, v->tex_hdr);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, v->tex_bl[0]);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->tex_ex[v->ex_i]);
    fill_pass(v);
    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
}

void view3d_render(view3d *v, const wave *w, const view3d_params *p)
{
    {
        /* the clock: the stars' twinkle, the clouds' drift, the grain, the hand-held drift */
        const float now = (float)SDL_GetTicks() / 1000.0f;
        v->last_time = v->time > 0.0f ? v->time : now;
        v->time = now;
    }
    if (p->look == 1) {
        SDL_GL_GetDrawableSize(v->win, &v->W, &v->H);
        if (v->H < 1) v->H = 1;
        v->video_on = video_setup(v);
    } else v->video_on = 0;
    update_camera(v);
    const float L = v->Lx > v->Ly ? v->Lx : v->Ly;
    {
        const int on = p->setting == 1 && view3d_setting_fits(v->Lx, v->Ly);
        if (on != v->scene_on || v->scene_dirty) {
            v->scene_on = on;
            build_solid(v);                   /* the walls' freeboard and coping follow the setting */
            if (on) build_scene(v);
            v->scene_dirty = 0;
        }
    }
    float proj[16], view[16], vp[16];
    mat_perspective(proj, 45.0f * (float)M_PI / 180.0f, v->aspect, 0.01f * L, v->scene_on ? fmaxf(40.0f * L, 600.0f) : 40.0f * L);
    mat_lookat(view, v->cam, v->fwd, v->right, v->up);
    mat_mul(vp, proj, view);

    /* the setting's light: the sun's shadow map, then the water's mirror image */
    float vpref[16];
    memcpy(vpref, vp, sizeof vpref);
    if (v->scene_on) {
        shadow_pass(v, p);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, v->tex_sh);
        glActiveTexture(GL_TEXTURE0);
        if (v->fbo_rf) reflection_pass(v, p, vp, vpref);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, v->tex_rf);
        glActiveTexture(GL_TEXTURE0);
    }

    /* per-frame data */
    const int m = p->glass;   /* 0 opaque, 1 floor only, 2 glass walls, 3 glass walls+bottom, 4 nothing */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->tex_h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->nx, v->ny, GL_RED, GL_FLOAT, w->eta);
    glActiveTexture(GL_TEXTURE1);
    if (m <= 2) {                                        /* 0 shadow, 1 mirror-extend, 2 flat fill; no floor, no caustics */
        if (v->gpu_caustics) gpu_caustics(v, p, m); else compute_caustics(v, w, p, m);
        glActiveTexture(GL_TEXTURE1);
    }
    glBindTexture(GL_TEXTURE_2D, v->gpu_caustics ? v->tex_lm2 : v->tex_lm);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->tex_h);

    if (v->video_on) glBindFramebuffer(GL_FRAMEBUFFER, v->fbo_ms ? v->fbo_ms : v->fbo_hdr);
    glViewport(0, 0, v->W, v->H);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* sky */
    glDisable(GL_DEPTH_TEST);
    set_common(v, v->p_bg, p, vp);
    glUniform3f(U(v->p_bg, "u_fwd"), v->fwd[0], v->fwd[1], v->fwd[2]);
    glUniform3f(U(v->p_bg, "u_right"), v->right[0], v->right[1], v->right[2]);
    glUniform3f(U(v->p_bg, "u_up"), v->up[0], v->up[1], v->up[2]);
    glUniform1f(U(v->p_bg, "u_tanhalf"), v->tanhalf);
    glUniform1f(U(v->p_bg, "u_aspect"), v->aspect);
    glBindVertexArray(v->vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* opaque */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    set_common(v, v->p_solid, p, vp);
    glBindVertexArray(v->vao_solid);
    if (m == 0) { glDrawArrays(GL_TRIANGLES, v->slab_off, v->slab_n); glDrawArrays(GL_TRIANGLES, v->walls_off, v->walls_n); }
    if (m == 1) { glDrawArrays(GL_TRIANGLES, v->table_off, v->table_n); glDrawArrays(GL_TRIANGLES, v->inner_off, v->inner_n); }
    if (m == 2) { glDrawArrays(GL_TRIANGLES, v->slab_off, v->slab_n); }
    if (v->scene_on) {
        set_common(v, v->p_scene, p, vp);
        glBindVertexArray(v->vao_scene);
        glDrawArrays(GL_TRIANGLES, 0, v->n_scene);
    }

    set_common(v, v->p_surf, p, vp);
    glUniformMatrix4fv(U(v->p_surf, "u_vpref"), 1, GL_FALSE, vpref);
    glBindVertexArray(v->vao_surf);
    glDrawElements(GL_TRIANGLES, v->n_surf_idx, GL_UNSIGNED_INT, (void *)0);

    draw_paddle_mark(v, p, vp);
    draw_objects(v, p, vp);

    /* transparent: the water body's faces, then the glass (if any) */
    if (m >= 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        set_common(v, v->p_sides, p, vp);
        glBindVertexArray(v->vao_sides);
        int nidx = v->n_sides_idx + (m == 4 ? v->n_bottom_idx : 0);
        glDrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_INT, (void *)0);

        if (m == 2 || m == 3) {
            set_common(v, v->p_glass, p, vp);
            glBindVertexArray(v->vao_solid);
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            for (int pass = 0; pass < 2; pass++) {
                glCullFace(pass == 0 ? GL_FRONT : GL_BACK);      /* far faces first */
                if (m == 3) glDrawArrays(GL_TRIANGLES, v->slab_off, v->slab_n);
                glDrawArrays(GL_TRIANGLES, v->walls_off, v->walls_n);
            }
            glDisable(GL_CULL_FACE);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    if (v->video_on) video_post(v, v->time - v->last_time);
    draw_overlay(v);
    glBindVertexArray(0);
    if (v->want_capture) {
        /* read the back buffer before it is swapped away */
        int W = v->W, H = v->H;
        free(v->capture);
        uint8_t *buf = malloc((size_t)W * H * 4);
        v->capture = malloc((size_t)W * H * 4);
        if (buf && v->capture) {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            for (int y = 0; y < H; y++)
                memcpy(v->capture + (size_t)y * W * 4, buf + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);
            v->cap_w = W; v->cap_h = H;
        }
        free(buf);
        v->want_capture = 0;
    }
    SDL_GL_SwapWindow(v->win);
}

void view3d_request_capture(view3d *v) { v->want_capture = 1; }

uint8_t *view3d_take_capture(view3d *v, int *w, int *h)
{
    uint8_t *c = v->capture;
    v->capture = NULL;
    *w = v->cap_w; *h = v->cap_h;
    return c;
}
