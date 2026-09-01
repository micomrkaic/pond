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
    uniform mat4 u_vp;
    out vec3 v_pos;
    out vec3 v_nrm;
    float H(int i, int j) {
        i = clamp(i, 0, u_n.x - 1); j = clamp(j, 0, u_n.y - 1);
        return texelFetch(u_height, ivec2(i, j), 0).r * u_gain;
    }
    void main() {
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
    uniform vec3 u_mu;
    uniform sampler2D u_light;
    out vec4 o;

    /* colour seen along direction T from surface point P, inside the basin */
    vec3 inside(vec3 P, vec3 T) {
        /* distance to the wall planes and to the floor / surface planes */
        float tw = 1e30;
        if (T.x < 0.0) tw = min(tw, (0.0 - P.x) / T.x);
        if (T.x > 0.0) tw = min(tw, (u_L.x - P.x) / T.x);
        if (T.z < 0.0) tw = min(tw, (0.0 - P.z) / T.z);
        if (T.z > 0.0) tw = min(tw, (u_L.y - P.z) / T.z);
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
                if (Q.x >= 0.0 && Q.x <= u_L.x && Q.z >= 0.0 && Q.z <= u_L.y) {
                    float lm = texture(u_light, Q.xz / u_L).r * 4.0;
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
                bool xwall = (T.x < 0.0 && Q.x < 1e-4 * u_L.x) || (T.x > 0.0 && Q.x > u_L.x - 1e-4 * u_L.x);
                vec2 q = xwall ? vec2(Q.z, Q.y) : vec2(Q.x, Q.y);
                c = pattern(q, u_tile, u_style) * 0.55;
            }
        } else {
            path = tv;
            if (T.y >= 0.0 || u_floor == 1) c = sky(T);
            else {
                vec3 Q = P + tv * T;
                float lm = texture(u_light, Q.xz / u_L).r * 4.0;
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
    uniform sampler2D u_light;
    out vec4 o;
    void main() {
        vec3 N = normalize(v_nrm);
        vec3 c;
        if (v_kind < 0.5) {
            /* interior floor: lit by the caustic map, seen through water along the
             * path from here towards the camera until it leaves the basin box */
            float lm = texture(u_light, v_pos.xz / u_L).r * 4.0;
            c = pattern(v_pos.xz, u_tile, u_style) * (0.30 + 0.70 * lm);
            vec3 D = u_cam - v_pos;
            float dist = length(D); D /= dist;
            float t = dist;
            if (D.y > 0.0) t = min(t, (0.0 - v_pos.y) / D.y);
            if (D.x < 0.0) t = min(t, (0.0 - v_pos.x) / D.x);
            if (D.x > 0.0) t = min(t, (u_L.x - v_pos.x) / D.x);
            if (D.z < 0.0) t = min(t, (0.0 - v_pos.z) / D.z);
            if (D.z > 0.0) t = min(t, (u_L.y - v_pos.z) / D.z);
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

/* ------------------------------------------------------------------ state */
struct view3d {
    SDL_Window *win;
    SDL_GLContext ctx;
    int nx, ny, W, H;
    float Lx, Ly, depth, dx, dy;
    float yaw, pitch, dist, cx, cz;

    GLuint p_bg, p_surf, p_solid, p_sides, p_glass, p_ovl;
    GLuint vao_empty;
    GLuint vao_surf, vbo_surf, ebo_surf; int n_surf_idx;
    GLuint vao_solid, vbo_solid;                  /* slab 36 verts, then 4 walls x 36 */
    GLuint vao_sides, vbo_sides, ebo_sides; int n_sides_idx, n_bottom_idx;   /* bottom face follows the sides */
    GLuint tex_h, tex_lm, tex_ovl;

    float *lm_acc; float *lm_tmp; uint8_t *lm8;
    canvas ovl; int ovl_dirty, ovl_w, ovl_h;
    char hud[256]; const char *const *help; int nhelp, show_help;

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

/* vertex ranges in vbo_solid */
#define SOLID_SLAB   0     /* 36: basin slab, top face = floor (kind 0)        */
#define SOLID_WALLS  36    /* 144: four wall boxes (kind 1)                    */
#define SOLID_TABLE  180   /* 36: extended slab, top face sunlit (kind 3)      */
#define SOLID_INNER  216   /* 6: floor quad inside the basin (kind 0), for the extended slab */
#define SOLID_TOTAL  222

static void build_solid(view3d *v)
{
    const float L = v->Lx, Lz = v->Ly, h = v->depth;
    const float Lmax = L > Lz ? L : Lz;
    const float t = 0.03f * Lmax, fb = 0.06f * Lmax, E = 0.15f * Lmax;
    float *buf = malloc(sizeof(float) * 7 * SOLID_TOTAL);
    int n = 0;
    box_faces(buf, &n, -t, L + t, -h - t, -h, -t, Lz + t, 0.0f, 1.0f);   /* slab: top face is the floor */
    box_faces(buf, &n, -t, 0.0f,  -h, fb, -t, Lz + t, 1.0f, 1.0f);
    box_faces(buf, &n, L, L + t,  -h, fb, -t, Lz + t, 1.0f, 1.0f);
    box_faces(buf, &n, 0.0f, L,   -h, fb, -t, 0.0f,   1.0f, 1.0f);
    box_faces(buf, &n, 0.0f, L,   -h, fb, Lz, Lz + t, 1.0f, 1.0f);
    /* a table the basin sits on when the walls are invisible: floor plane continues */
    box_faces(buf, &n, -E, L + E, -h - t, -h, -E, Lz + E, 3.0f, 1.0f);
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
    GLuint *idx = malloc(sizeof(GLuint) * 6 * (size_t)(nx - 1) * (ny - 1));
    int n = 0;
    for (int j = 0; j < ny - 1; j++)
        for (int i = 0; i < nx - 1; i++) {
            GLuint a = (GLuint)(i + nx * j), b = a + 1, c = a + (GLuint)nx, d = c + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = b;
            idx[n++] = b; idx[n++] = c; idx[n++] = d;
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

static void build_sides(view3d *v)
{
    /* four strips, one column per edge cell (same x/z mapping as the surface mesh, so
     * the top edge coincides with it), plus a flat bottom face.
     * vertex = uv(2) frac(2) bottom(1) nrm(3) */
    const int nx = v->nx, ny = v->ny, VS = 8;
    int cols = 2 * nx + 2 * ny;
    float *verts = malloc(sizeof(float) * VS * (2 * (size_t)cols + 4));
    GLuint *idx = malloc(sizeof(GLuint) * (6 * (size_t)cols + 6));
    int nv = 0, ni = 0;
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
    v->n_sides_idx = ni;
    /* bottom face of the water body (drawn only when there is no container) */
    {
        const float corners[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
        GLuint base = (GLuint)nv;
        for (int c = 0; c < 4; c++) {
            float *p = verts + nv * VS;
            p[0] = 0; p[1] = 0; p[2] = corners[c][0]; p[3] = corners[c][1]; p[4] = 1.0f; p[5] = 0; p[6] = -1; p[7] = 0;
            nv++;
        }
        idx[ni++] = base; idx[ni++] = base + 1; idx[ni++] = base + 2;
        idx[ni++] = base; idx[ni++] = base + 2; idx[ni++] = base + 3;
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

view3d *view3d_create(SDL_Window *win, int nx, int ny)
{
    view3d *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->win = win; v->nx = nx; v->ny = ny;
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
    if (!v->p_bg || !v->p_surf || !v->p_solid || !v->p_sides || !v->p_glass || !v->p_ovl) { free(v); return NULL; }

    glGenVertexArrays(1, &v->vao_empty);
    glGenVertexArrays(1, &v->vao_surf); glGenBuffers(1, &v->vbo_surf); glGenBuffers(1, &v->ebo_surf);
    glGenVertexArrays(1, &v->vao_solid); glGenBuffers(1, &v->vbo_solid);
    glGenVertexArrays(1, &v->vao_sides); glGenBuffers(1, &v->vbo_sides); glGenBuffers(1, &v->ebo_sides);

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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, nx, ny, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

    glGenTextures(1, &v->tex_ovl);
    glBindTexture(GL_TEXTURE_2D, v->tex_ovl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    v->lm_acc = malloc(sizeof(float) * (size_t)nx * ny);
    v->lm_tmp = malloc(sizeof(float) * (size_t)nx * ny);
    v->lm8 = malloc((size_t)nx * ny);
    v->ovl_dirty = 1;
    v->yaw = 35.0f; v->pitch = 42.0f;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) fprintf(stderr, "GL error during init: 0x%x\n", err);
    return v;
}

void view3d_destroy(view3d *v)
{
    if (!v) return;
    free(v->lm_acc); free(v->lm_tmp); free(v->lm8); free(v->ovl.rgba); free(v->capture);
    if (v->ctx) SDL_GL_DeleteContext(v->ctx);
    free(v);
}

void view3d_set_pool(view3d *v, const wave *w)
{
    v->Lx = (float)w->Lx; v->Ly = (float)w->Ly; v->depth = (float)w->depth; v->dx = (float)w->dx; v->dy = (float)w->dy;
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
    if (px < 0.0f || px > v->Lx || pz < 0.0f || pz > v->Ly) return 0;
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
    const int nx = v->nx, ny = v->ny;
    const float dx = v->dx, dy = v->dy, gain = p->gain, depth = v->depth;
    const float *e = w->eta;
    float *acc = v->lm_acc;
    memset(acc, 0, sizeof(float) * (size_t)nx * ny);

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

    int px = 0, pz = 0;
    if (edge == 1) {
        px = (int)(fabsf(ox) / dx) + 3;
        pz = (int)(fabsf(oz) / dy) + 3;
        if (px > nx - 1) px = nx - 1;
        if (pz > ny - 1) pz = ny - 1;
    } else if (edge == 2) {
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                const float sx = ((float)i + 0.5f) * dx - ox, sz = ((float)j + 0.5f) * dy - oz;
                if (sx < 0.0f || sx > v->Lx || sz < 0.0f || sz > v->Ly) acc[i + nx * j] = 1.0f;
            }
    }

    for (int j = -pz; j < ny + pz; j++) {
        const int jm = mirror_idx(j - 1, ny), jc = mirror_idx(j, ny), jp = mirror_idx(j + 1, ny);
        for (int i = -px; i < nx + px; i++) {
            const int im = mirror_idx(i - 1, nx), ic = mirror_idx(i, nx), ip = mirror_idx(i + 1, nx);
            const float h  = e[ic + nx * jc] * gain;
            const float hx = (e[ip + nx * jc] - e[im + nx * jc]) * gain / (2.0f * dx);
            const float hz = (e[ic + nx * jp] - e[ic + nx * jm]) * gain / (2.0f * dy);
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
            const float qx = ((float)i + 0.5f) * dx + s * T[0];
            const float qz = ((float)j + 0.5f) * dy + s * T[2];
            /* bilinear splat into cell space */
            const float gx = qx / dx - 0.5f, gz = qz / dy - 0.5f;
            const int ix = (int)floorf(gx), iz = (int)floorf(gz);
            const float ax = gx - (float)ix, az = gz - (float)iz;
            const float wgt[4] = { (1 - ax) * (1 - az), ax * (1 - az), (1 - ax) * az, ax * az };
            const int cx[4] = { ix, ix + 1, ix, ix + 1 }, cz[4] = { iz, iz, iz + 1, iz + 1 };
            for (int q = 0; q < 4; q++)
                if (cx[q] >= 0 && cx[q] < nx && cz[q] >= 0 && cz[q] < ny)
                    acc[cx[q] + nx * cz[q]] += wgt[q];
        }
    }
    /* 3x3 binomial blur, then to 8 bit with 1.0 -> 64 */
    float *tmp = v->lm_tmp;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            const int im = i > 0 ? i - 1 : i, ip = i < nx - 1 ? i + 1 : i;
            tmp[i + nx * j] = 0.25f * (acc[im + nx * j] + 2.0f * acc[i + nx * j] + acc[ip + nx * j]);
        }
    for (int j = 0; j < ny; j++) {
        const int jm = j > 0 ? j - 1 : j, jp = j < ny - 1 ? j + 1 : j;
        for (int i = 0; i < nx; i++) {
            float val = 0.25f * (tmp[i + nx * jm] + 2.0f * tmp[i + nx * j] + tmp[i + nx * jp]);
            if (val > 4.0f) val = 4.0f;
            v->lm8[i + nx * j] = (uint8_t)(val * 63.75f + 0.5f);
        }
    }
    if (getenv("POND_DEBUG")) {
        int mn = 255, mx = 0; double mean = 0;
        for (int i = 0; i < nx * ny; i++) { if (v->lm8[i] < mn) mn = v->lm8[i]; if (v->lm8[i] > mx) mx = v->lm8[i]; mean += v->lm8[i]; }
        printf("lightmap min %d max %d mean %.1f\n", mn, mx, mean / (nx * ny));
    }
    glBindTexture(GL_TEXTURE_2D, v->tex_lm);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, nx, ny, GL_RED, GL_UNSIGNED_BYTE, v->lm8);
}

/* ------------------------------------------------------------------ overlay */
void view3d_set_overlay(view3d *v, const char *hud, const char *const *help, int nhelp, int show_help)
{
    if (hud) { strncpy(v->hud, hud, sizeof v->hud - 1); v->hud[sizeof v->hud - 1] = 0; }
    v->help = help; v->nhelp = nhelp; v->show_help = show_help;
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

        /* HUD: one or more lines separated by newlines */
        {
            char tmp[256];
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
        } else {
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
static void set_common(view3d *v, GLuint p, const view3d_params *prm, const float *vp)
{
    glUseProgram(p);
    GLint l;
    if ((l = U(p, "u_vp")) >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, vp);
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
    if (m <= 2) compute_caustics(v, w, p, m);           /* 0 shadow, 1 mirror-extend, 2 flat fill; no floor, no caustics */
    glBindTexture(GL_TEXTURE_2D, v->tex_lm);
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
    if (m == 0) { glDrawArrays(GL_TRIANGLES, SOLID_SLAB, 36); glDrawArrays(GL_TRIANGLES, SOLID_WALLS, 144); }
    if (m == 1) { glDrawArrays(GL_TRIANGLES, SOLID_TABLE, 36); glDrawArrays(GL_TRIANGLES, SOLID_INNER, 6); }
    if (m == 2) { glDrawArrays(GL_TRIANGLES, SOLID_SLAB, 36); }

    set_common(v, v->p_surf, p, vp);
    glBindVertexArray(v->vao_surf);
    glDrawElements(GL_TRIANGLES, v->n_surf_idx, GL_UNSIGNED_INT, (void *)0);

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
                if (m == 3) glDrawArrays(GL_TRIANGLES, SOLID_SLAB, 36);
                glDrawArrays(GL_TRIANGLES, SOLID_WALLS, 144);
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
