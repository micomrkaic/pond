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
    uniform vec3 u_sun;
    const vec3 ZENITH  = vec3(0.30, 0.52, 0.86);
    const vec3 HORIZON = vec3(0.80, 0.86, 0.92);
    const vec3 GROUND  = vec3(0.07, 0.075, 0.09);
    const vec3 SUNCOL  = vec3(1.00, 0.96, 0.86);
    const vec3 SCATTER = vec3(0.00, 0.22, 0.38);
    vec3 sky(vec3 d) {
        vec3 c;
        if (d.y >= 0.0) c = mix(HORIZON, ZENITH, sqrt(clamp(d.y, 0.0, 1.0)));
        else            c = mix(HORIZON * 0.45, GROUND, pow(clamp(-d.y, 0.0, 1.0), 0.4));
        float s = max(dot(d, u_sun), 0.0);
        c += SUNCOL * (5.0 * pow(s, 800.0) + 0.12 * pow(s, 12.0));
        return c;
    }
    float hash21(vec2 p) {
        p = fract(p * vec2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }
    vec3 pattern(vec2 q, float tile, int style) {
        if (style == 0) {
            vec2 u = q / tile;
            vec2 f = fract(u);
            if (f.x < 0.045 || f.y < 0.045) return vec3(0.40, 0.44, 0.47);
            float b = 0.94 + 0.06 * hash21(floor(u));
            return vec3(0.72, 0.80, 0.80) * b;
        } else if (style == 1) {
            float c = mod(floor(q.x / tile) + floor(q.y / tile), 2.0);
            return c > 0.5 ? vec3(0.88, 0.87, 0.80) : vec3(0.20, 0.26, 0.34);
        } else {
            float b = 0.92 + 0.08 * hash21(floor(q / (tile * 0.02)));
            return vec3(0.80, 0.72, 0.54) * b;
        }
    }
    vec3 gam(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
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
    out vec4 o;
    void main() {
        vec3 d = normalize(u_fwd + u_tanhalf * (v_ndc.x * u_aspect * u_right + v_ndc.y * u_up));
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
static const char *fs_surf = GLSL(
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
    out vec4 o;

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
                    c = col * (0.30 + 0.70 * lm);
                } else {
                    c = col * (0.35 + 0.65 * max(u_sun.y, 0.0));
                }
            }
        } else if (tw < tv) {
            path = tw;
            if (u_walls == 1) c = sky(T);
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
                c = pattern(Q.xz, u_tile, u_style) * (0.30 + 0.70 * lm);
            }
        }
        vec3 att = exp(-u_mu * max(path, 0.0));
        return c * att + SCATTER * (1.0 - att);
    }

    void main() {
        vec3 N = normalize(v_nrm);
        vec3 V = normalize(u_cam - v_pos);
        vec3 c;
        float R0 = 0.02;
        if (dot(N, V) >= 0.0) {
            float cv = max(dot(N, V), 0.0);
            float F = R0 + (1.0 - R0) * pow(1.0 - cv, 5.0);
            vec3 R = reflect(-V, N);
            vec3 T = refract(-V, N, 1.0 / 1.333);
            c = mix(inside(v_pos, T), sky(R), F);
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
    uniform mat4 u_vp;
    void main() { gl_Position = u_vp * vec4(a_pos, 1.0); }
);
static const char *fs_mark = GLSL(
    uniform vec3 u_col;
    out vec4 o;
    void main() { o = vec4(gam(u_col), 1.0); }
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
            c = pattern(v_pos.xz, u_tile, u_style) * (0.30 + 0.70 * lm);
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
        } else {
            /* kind 1: wall or slab face; kind 3: floor outside the basin. Both sunlit. */
            vec2 q = abs(N.x) > 0.5 ? vec2(v_pos.z, v_pos.y) : (abs(N.z) > 0.5 ? vec2(v_pos.x, v_pos.y) : v_pos.xz);
            float diff = max(dot(N, u_sun), 0.0);
            c = pattern(q, u_tile, u_style) * (0.35 + 0.65 * diff);
        }
        o = vec4(gam(c), 1.0);
    }
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
    uniform vec3 u_mu;
    uniform vec3 u_cam;
    out vec4 o;
    void main() {
        /* a vertical face of the water body: translucent, with the water's own Fresnel reflection */
        vec3 N = normalize(v_nrm);
        vec3 V = normalize(u_cam - v_pos);
        if (dot(N, V) < 0.0) N = -N;
        float cv = max(dot(N, V), 0.0);
        float F = 0.02 + 0.98 * pow(1.0 - cv, 5.0);
        float path = 0.6 * max(u_L.x, u_L.y);
        vec3 att = exp(-u_mu * path);
        float a = clamp(1.0 - (att.r + att.g + att.b) / 3.0, 0.06, 0.92);
        vec3 body = SCATTER * 1.3 + vec3(0.05, 0.08, 0.10);
        vec3 c = mix(body, sky(reflect(-V, N)), F);
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

#define MARK_MAX 768        /* vertices for the wavemaker outline: 6 per segment */

/* ------------------------------------------------------------------ state */
struct view3d {
    SDL_Window *win;
    SDL_GLContext ctx;
    int nx, ny, W, H;
    int shape;                     /* 0 rectangle, 1 disk */
    float Lx, Ly, depth, dx, dy;   /* disk: Lx = Ly = 2R, dx = dtheta, dy = dr */
    int lm_w, lm_h;                /* caustic light map size */
    /* vertex ranges in vbo_solid: slab, walls, table, inner floor */
    int slab_off, slab_n, walls_off, walls_n, table_off, table_n, inner_off, inner_n;
    float yaw, pitch, dist, cx, cz;

    GLuint p_bg, p_surf, p_solid, p_sides, p_glass, p_ovl, p_caus, p_fill, p_mark;
    GLuint vao_mark, vbo_mark; int n_mark;
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

static GLuint program(const char *vs, const char *fs, int sides)
{
    const char *vp[3] = { glsl_header, glsl_common, vs };
    const char *fp[3] = { glsl_header, glsl_common, fs };
    GLuint v = compile(GL_VERTEX_SHADER, vp, 3), f = compile(GL_FRAGMENT_SHADER, fp, 3);
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
    const float t = 0.03f * Lmax, fb = 0.06f * Lmax, E = 0.15f * Lmax;
    float *buf;
    int n = 0;
    if (v->shape == 0) {
        buf = malloc(sizeof(float) * 7 * 222);
        v->slab_off = n;
        box_faces(buf, &n, -t, L + t, -h - t, -h, -t, Lz + t, 0.0f, 1.0f);   /* slab: top face is the floor */
        v->slab_n = n - v->slab_off;
        v->walls_off = n;
        box_faces(buf, &n, -t, 0.0f,  -h, fb, -t, Lz + t, 1.0f, 1.0f);
        box_faces(buf, &n, L, L + t,  -h, fb, -t, Lz + t, 1.0f, 1.0f);
        box_faces(buf, &n, 0.0f, L,   -h, fb, -t, 0.0f,   1.0f, 1.0f);
        box_faces(buf, &n, 0.0f, L,   -h, fb, Lz, Lz + t, 1.0f, 1.0f);
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
            quad(buf, &n, pd, pc, qc, qd, up, 1.0f);
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
    v->p_surf  = program(vs_surf, fs_surf, 0);
    v->p_solid = program(vs_solid, fs_solid, 0);
    v->p_sides = program(vs_sides, fs_sides, 1);
    v->p_glass = program(vs_solid, fs_glass, 0);
    v->p_ovl   = program(vs_ovl, fs_ovl, 0);
    v->p_caus  = program(vs_caus, fs_caus, 0);
    v->p_fill  = program(vs_fill, fs_fill, 0);
    v->p_blur  = program(vs_fill, fs_blur, 0);
    v->p_mark  = program(vs_mark, fs_mark, 0);
    v->p_res   = program(vs_resample, fs_resample, 0);
    if (!v->p_res || !v->p_blur || !v->p_bg || !v->p_surf || !v->p_solid || !v->p_sides || !v->p_glass || !v->p_ovl || !v->p_caus || !v->p_fill || !v->p_mark) { free(v); return NULL; }

    glGenVertexArrays(1, &v->vao_empty);
    glGenVertexArrays(1, &v->vao_surf); glGenBuffers(1, &v->vbo_surf); glGenBuffers(1, &v->ebo_surf);
    glGenVertexArrays(1, &v->vao_solid); glGenBuffers(1, &v->vbo_solid);
    glGenVertexArrays(1, &v->vao_sides); glGenBuffers(1, &v->vbo_sides); glGenBuffers(1, &v->ebo_sides);
    glGenVertexArrays(1, &v->vao_mark); glGenBuffers(1, &v->vbo_mark);
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
    glBindVertexArray(0);

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
    float yaw = v->yaw * (float)M_PI / 180.0f, pitch = v->pitch * (float)M_PI / 180.0f;
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

        /* HUD: one or more lines separated by newlines; d hides it */
        if (v->show_hud) {
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
static void mark_seg(float *b, int *n, float x0, float z0, float x1, float z1, float h, float y)
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

static void build_paddle_mark(view3d *v, const view3d_params *p)
{
    static float buf[MARK_MAX * 3];
    int n = 0;
    const float Lmax = v->Lx > v->Ly ? v->Lx : v->Ly;
    const float h = 0.004f * Lmax;                  /* half the line thickness */
    const float y = 0.006f * Lmax;                  /* just clear of the mean surface */
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
    glUniformMatrix4fv(U(v->p_mark, "u_vp"), 1, GL_FALSE, vp);
    glUniform3f(U(v->p_mark, "u_col"), 1.0f, 0.45f, 0.10f);
    glBindVertexArray(v->vao_mark);
    glDrawArrays(GL_TRIANGLES, 0, v->n_mark);
}

static void set_common(view3d *v, GLuint p, const view3d_params *prm, const float *vp)
{
    glUseProgram(p);
    GLint l;
    if (vp && (l = U(p, "u_vp")) >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, vp);
    if ((l = U(p, "u_lscale")) >= 0) glUniform1f(l, v->gpu_caustics ? 1.0f : 4.0f);
    if ((l = U(p, "u_sun")) >= 0) glUniform3f(l, prm->sun[0], prm->sun[1], prm->sun[2]);
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

void view3d_render(view3d *v, const wave *w, const view3d_params *p)
{
    update_camera(v);
    const float L = v->Lx > v->Ly ? v->Lx : v->Ly;
    float proj[16], view[16], vp[16];
    mat_perspective(proj, 45.0f * (float)M_PI / 180.0f, v->aspect, 0.01f * L, 40.0f * L);
    mat_lookat(view, v->cam, v->fwd, v->right, v->up);
    mat_mul(vp, proj, view);

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

    set_common(v, v->p_surf, p, vp);
    glBindVertexArray(v->vao_surf);
    glDrawElements(GL_TRIANGLES, v->n_surf_idx, GL_UNSIGNED_INT, (void *)0);

    draw_paddle_mark(v, p, vp);

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
