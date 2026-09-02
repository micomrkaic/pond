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

#include "param.h"
#include "script.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

const preset presets[NPRESETS] = {
    { "tray",  0.30,  0.02, 0 },
    { "pond",  3.00,  0.60, 2 },
    { "pool", 12.00,  2.00, 0 },
    { "sea",  80.00, 30.00, 2 },
};

/* ------------------------------------------------ setters with side effects */

wave *app_make_wave(int shape, int grid, double Lx, double Ly, double depth)
{
    if (shape == WAVE_DISK) {
        printf("building the disk basis (%d angles x %d rings)...\n", grid, grid / 2);
        fflush(stdout);
        return wave_create_disk(grid, grid / 2, Lx, depth);
    }
    return wave_create(grid, grid, Lx, Ly, depth);
}

void app_reset_camera(app *a)
{
    a->cam_yaw = 35.0f; a->cam_pitch = 42.0f; a->cam_dist = 1.5f;
    if (a->v3) { view3d_reset_camera(a->v3, a->w); view3d_get_camera(a->v3, &a->cam_yaw, &a->cam_pitch, &a->cam_dist); }
}

int app_set_shape(app *a, int shape)
{
    if (shape == a->shape) return 0;
    double Lx = a->w->Lx, Ly = a->w->Ly, depth = a->w->depth, gamma0 = a->w->gamma0;
    if (shape == WAVE_DISK) Lx = Ly = sqrt(Lx * Ly);
    wave *nw = app_make_wave(shape, a->nx, Lx, Ly, depth);
    if (!nw) { fprintf(stderr, "could not create the %s basin\n", shape == WAVE_DISK ? "disk" : "rectangular"); return -1; }
    wave_set_damping(nw, gamma0);
    wave_destroy(a->w);
    a->w = nw;
    a->shape = shape;
    if (a->hs) { hos_destroy(a->hs); a->hs = NULL; }
    if (a->v3) {
        view3d_destroy(a->v3);
        a->v3 = view3d_create(a->win, a->w, a->p3.cpu_caustics);
        if (!a->v3) { fprintf(stderr, "3-D view unavailable\n"); a->running = 0; return -1; }
    }
    app_reset_camera(a);
    wave_add_drop(a->w, 0.5 * a->w->Lx, 0.5 * a->w->Ly, 0.03 * a->w->Lx, -0.15 * 0.03 * a->w->Lx);
    app_clamp_paddle(a);
    a->hud_dirty = 1;
    return 0;
}

void app_pool_changed(app *a)
{
    if (a->v3) view3d_set_pool(a->v3, a->w);
    app_clamp_paddle(a);
    a->hud_dirty = 1;
}

void app_set_preset(app *a, int p)
{
    if (p < 0 || p >= NPRESETS) return;
    a->preset = p;
    wave_set_pool(a->w, presets[p].L, presets[p].L, presets[p].depth);
    a->rp.floor_style = a->p3.floor_style = presets[p].floor;
    app_reset_camera(a);
    app_clamp_paddle(a);
    a->hud_dirty = 1;
}

void app_set_fullscreen(app *a, int on)
{
    a->fullscreen = !!on;
    a->hud_dirty = 1;
    if (!a->win) return;
#ifdef __EMSCRIPTEN__
    /* The browser grants full screen only from inside a user gesture, and SDL
     * events are polled from the animation frame, so defer the request to the
     * next one Emscripten sees. */
    if (on) emscripten_request_fullscreen("#canvas", 1);
    else    emscripten_exit_fullscreen();
#else
    if (SDL_SetWindowFullscreen(a->win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
        fprintf(stderr, "full screen: %s\n", SDL_GetError());
#endif
}

/* a drop into the water, seen and heard */
void app_splash(app *a, double x, double y, double s, double amp)
{
    wave_add_drop(a->w, x, y, s, amp);
    if (!a->au) return;
    double pan = 0, att = 1;
    if (a->v3) view3d_listen(a->v3, x, y, &pan, &att);
    /* drops are drawn at a size relative to the basin so they can be seen; for the ear,
     * rain should plink like rain whatever the basin, so the acoustic size is the drop as it
     * would be in the 30 cm tray: a raindrop's few millimetres, a click a small stone */
    audio_splash(a->au, s * 0.3 / sqrt(a->w->Lx * a->w->Ly), pan, att);
}

/* The wavemaker is driven at a frequency; the wavelength it radiates is whatever
 * the dispersion relation answers with.  The number kept is that wavelength as a
 * count across the basin, so resizing the basin or changing preset keeps the same
 * picture rather than the same hertz. */
double app_paddle_k(const app *a)
{
    return 2.0 * M_PI * a->paddle_div / sqrt(a->w->Lx * a->w->Ly);
}

double app_paddle_hz(const app *a)
{
    return wave_omega(a->w, app_paddle_k(a)) / (2.0 * M_PI);
}

/* The band of wavenumbers this basin can actually answer with.  Below its lowest
 * mode there is nothing to resonate with and the paddle looks broken; above eight
 * cells per wavelength there is nothing left to draw. */
static void paddle_k_range(const app *a, double *klo, double *khi)
{
    const wave *w = a->w;
    if (w->shape == WAVE_DISK) { *klo = M_PI / w->R; *khi = M_PI / (4.0 * w->dr); }
    else {
        const double Lmax = w->Lx > w->Ly ? w->Lx : w->Ly;
        const double cell = w->dx < w->dy ? w->dx : w->dy;
        *klo = M_PI / Lmax;                /* the (1,0) or (0,1) mode */
        *khi = M_PI / (4.0 * cell);
    }
    if (*khi < *klo) *khi = *klo;
}

void app_clamp_paddle(app *a)
{
    double klo, khi;
    paddle_k_range(a, &klo, &khi);
    const double L = sqrt(a->w->Lx * a->w->Ly);
    double k = 2.0 * M_PI * a->paddle_div / L;
    if (k < klo) k = klo;
    if (k > khi) k = khi;
    a->paddle_div = k * L / (2.0 * M_PI);
}

void app_set_paddle_hz(app *a, double f)
{
    const double L = sqrt(a->w->Lx * a->w->Ly);
    a->paddle_div = wave_k_of_omega(a->w, 2.0 * M_PI * f) * L / (2.0 * M_PI);
    app_clamp_paddle(a);
}

/* ------------------------------------------------------- get / set per name */

#define G(fn, expr)  static double fn(const app *a) { return (double)(expr); }
#define S(fn, stmt)  static void fn(app *a, double v) { stmt; }

/* basin */
G(g_preset, a->preset)              S(s_preset, app_set_preset(a, (int)v))
G(g_shape, a->shape == WAVE_DISK)   S(s_shape, app_set_shape(a, v > 0.5 ? WAVE_DISK : WAVE_RECT))
G(g_width, a->w->Lx)
G(g_length, a->w->Ly)
G(g_depth, a->w->depth)             S(s_depth, { wave_set_pool(a->w, a->w->Lx, a->w->Ly, v); app_pool_changed(a); })
static void set_dims(app *a, double Lx, double Ly)
{
    if (a->shape == WAVE_DISK) Lx = Ly;                       /* the diameter */
    if (Lx / Ly > 4.0 || Ly / Lx > 4.0) return;               /* keep the aspect within 4:1 */
    wave_set_pool(a->w, Lx, Ly, a->w->depth);
    app_pool_changed(a);
}
S(s_width, set_dims(a, v, a->shape == WAVE_DISK ? v : a->w->Ly))
S(s_length, set_dims(a, a->shape == WAVE_DISK ? v : a->w->Lx, v))
G(g_hos, a->hos_on)                 S(s_hos, a->hos_on = v > 0.5)
G(g_damping, a->w->gamma0)          S(s_damping, wave_set_damping(a->w, v))
G(g_warp, a->warp)                  S(s_warp, a->warp = v)

/* sources */
G(g_rain, a->rain)                  S(s_rain, a->rain = v > 0.5)
G(g_rain_rate, a->rain_rate)        S(s_rain_rate, a->rain_rate = v)
G(g_breeze, a->breeze)              S(s_breeze, a->breeze = v > 0.5)
G(g_paddle, a->paddle)              S(s_paddle, a->paddle = v > 0.5)
G(g_pfreq, app_paddle_hz(a))        S(s_pfreq, app_set_paddle_hz(a, v))
G(g_pwall, a->paddle_wall)          S(s_pwall, a->paddle_wall = (int)v & 3)
G(g_ppos, a->paddle_pos)
static void s_ppos(app *a, double v)
{
    if (a->shape == WAVE_DISK) { v = fmod(v, 1.0); if (v < 0) v += 1.0; }   /* the rim has no ends */
    else { if (v < 0) v = 0; if (v > 1) v = 1; }
    a->paddle_pos = v;
}
G(g_pspan, a->paddle_span)          S(s_pspan, a->paddle_span = v)
G(g_pmark, a->paddle_mark)          S(s_pmark, a->paddle_mark = v > 0.5)
G(g_pstroke, a->paddle_gain)        S(s_pstroke, a->paddle_gain = v)
G(g_bgain, a->breeze_gain)          S(s_bgain, a->breeze_gain = v)
G(g_fgain, a->finger_gain)          S(s_fgain, a->finger_gain = v)

/* display */
G(g_glass, a->p3.glass)             S(s_glass, a->p3.glass = (int)v)
G(g_floor, a->p3.floor_style)       S(s_floor, a->rp.floor_style = a->p3.floor_style = (int)v)
G(g_gain, a->p3.gain)               S(s_gain, a->rp.gain = a->p3.gain = (float)v)
G(g_hud, a->show_hud)               S(s_hud, a->show_hud = v > 0.5)
G(g_help, a->show_help)             S(s_help, a->show_help = v > 0.5)
G(g_full, a->fullscreen)            S(s_full, app_set_fullscreen(a, v > 0.5))
G(g_paused, a->paused)              S(s_paused, a->paused = v > 0.5)
G(g_view, a->rp.view)               S(s_view, a->rp.view = v > 0.5)
/* the camera lives in the view when there is one, else in the app's shadow copy */
static void cam_get(const app *a, float *y, float *p, float *d)
{
    if (a->v3) view3d_get_camera(a->v3, y, p, d);
    else { *y = a->cam_yaw; *p = a->cam_pitch; *d = a->cam_dist; }
}
static void cam_put(app *a, float y, float p, float d)
{
    a->cam_yaw = y; a->cam_pitch = p; a->cam_dist = d;
    if (a->v3) view3d_set_camera(a->v3, y, p, d);
}
static double g_yaw(const app *a)   { float y, p, d; cam_get(a, &y, &p, &d); return y; }
static double g_pitch(const app *a) { float y, p, d; cam_get(a, &y, &p, &d); return p; }
static double g_dist(const app *a)  { float y, p, d; cam_get(a, &y, &p, &d); return d; }
static void s_yaw(app *a, double v)   { float y, p, d; cam_get(a, &y, &p, &d); cam_put(a, (float)v, p, d); }
static void s_pitch(app *a, double v) { float y, p, d; cam_get(a, &y, &p, &d); cam_put(a, y, (float)v, d); }
static void s_dist(app *a, double v)  { float y, p, d; cam_get(a, &y, &p, &d); cam_put(a, y, p, (float)v); }

/* sound: shadowed in the app so a run without a device still keeps the numbers */
G(g_volume, a->volume)              S(s_volume, { a->volume = v; if (a->au) audio_set_volume(a->au, v); })
G(g_mute, a->mute)                  S(s_mute, { a->mute = v > 0.5; if (a->au) audio_set_mute(a->au, a->mute); })
#define KNOB(nm, K) \
    G(g_##nm, a->knob[K]) S(s_##nm, { a->knob[K] = v; if (a->au) audio_set_knob(a->au, K, v); })
KNOB(drops, SND_DROPS) KNOB(bed, SND_BED) KNOB(brown, SND_BROWN) KNOB(sbreeze, SND_BREEZE) KNOB(harsh, SND_HARSH)

/* --------------------------------------------------------------- the table */

static const char *const preset_names[] = { "tray", "pond", "pool", "sea", NULL };
static const char *const shape_names[]  = { "rect", "disk", NULL };
static const char *const wall_names[]   = { "x=0", "x=Lx", "y=0", "y=Ly", NULL };
static const char *const glass_names[]  = { "opaque", "floor", "glass", "glass+bottom", "none", NULL };
static const char *const floor_names[]  = { "tiles", "checker", "sand", NULL };

#define B(name, grp, help, g, s)                  { name, PK_BOOL, grp, help, 0, 1, 0, 1, 0, 0, NULL, g, s }
#define E(name, grp, help, names, g, s)           { name, PK_ENUM, grp, help, 0, 0, 1, 1, 0, 0, names, g, s }
#define RM(name, grp, help, lo, hi, step, snap, g, s) { name, PK_REAL, grp, help, lo, hi, 0, step, 1, snap, NULL, g, s }
#define RA(name, grp, help, lo, hi, step, wrap, g, s) { name, PK_REAL, grp, help, lo, hi, wrap, step, 0, 0, NULL, g, s }

static const param params[] = {
    /* basin */
    E("preset", "basin",  "tray 30 cm, pond 3 m, pool 12 m, sea 80 m; sets size, depth, floor, camera", preset_names, g_preset, s_preset),
    E("shape", "basin",   "rect or disk", shape_names, g_shape, s_shape),
    RM("width", "basin",  "basin width, metres (disk: the diameter)", 0.05, 1000, 1.05, 0, g_width, s_width),
    RM("length", "basin", "basin length, metres", 0.05, 1000, 1.05, 0, g_length, s_length),
    RM("depth", "basin",  "water depth, metres", 0.002, 5000, 1.1, 0, g_depth, s_depth),
    B("nonlinear", "basin", "the HOS correction (rectangle only)", g_hos, s_hos),
    RM("damping", "basin", "extra uniform damping, 1/s", 0.0001, 10, 2.0, 0, g_damping, s_damping),
    RM("warp", "basin",   "simulated seconds per real second", 1.0 / 64, 64, 1.5, 0, g_warp, s_warp),
    /* sources */
    B("rain", "sources",    "rain on", g_rain, s_rain),
    RM("rain-rate", "sources", "drops per simulated second", 0.05, 200, 1.5, 0, g_rain_rate, s_rain_rate),
    B("breeze", "sources",  "wind sea on", g_breeze, s_breeze),
    RM("breeze-gain", "sources", "multiplier on the wind forcing", 0.01, 100, 1.5, 0, g_bgain, s_bgain),
    RM("finger-gain", "sources", "multiplier on the drag-a-finger forcing", 0.01, 100, 1.5, 0, g_fgain, s_fgain),
    B("paddle", "wavemaker",  "wavemaker on", g_paddle, s_paddle),
    RM("paddle-freq", "wavemaker", "wavemaker frequency, Hz (held inside the band the basin can answer)", 0.001, 10000, 1.25, 0, g_pfreq, s_pfreq),
    E("paddle-wall", "wavemaker", "which wall it is on (disk: the rim)", wall_names, g_pwall, s_pwall),
    RA("paddle-pos", "wavemaker", "where along that wall, 0..1 (disk: turns round the rim)", 0, 1, 0.02, 2, g_ppos, s_ppos),
    RM("paddle-span", "wavemaker", "how much of the wall it covers, 0..1", 0.02, 1, 1.25, 0, g_pspan, s_pspan),
    B("paddle-mark", "wavemaker", "outline the wavemaker on the water (key: D)", g_pmark, s_pmark),
    RM("paddle-stroke", "wavemaker", "multiplier on the wavemaker's speed", 0.01, 100, 1.5, 0, g_pstroke, s_pstroke),
    /* display */
    E("glass", "display",   "the container: opaque, floor only, glass, glass+bottom, none", glass_names, g_glass, s_glass),
    E("floor", "display",   "floor pattern", floor_names, g_floor, s_floor),
    RM("gain", "display",   "display gain on the height field", 0.01, 1000, 1.5, 0, g_gain, s_gain),
    B("hud", "display",     "the settings box, and the wavemaker outline", g_hud, s_hud),
    B("help", "display",    "the help panel", g_help, s_help),
    B("fullscreen", "display", "full screen", g_full, s_full),
    B("paused", "display",  "time stopped", g_paused, s_paused),
    B("view", "display",    "2-D mode: height map instead of the rendered view", g_view, s_view),
    RA("yaw", "camera",    "camera yaw, degrees", 0, 360, 5, 1, g_yaw, s_yaw),
    RA("pitch", "camera",  "camera pitch, degrees above the water", -89, 89, 3, 0, g_pitch, s_pitch),
    RM("dist", "camera",   "camera distance, in basin lengths", 0.15, 12, 1.25, 0, g_dist, s_dist),
    /* sound */
    RM("volume", "sound", "master volume", 0, 1, 1.25, 0.05, g_volume, s_volume),
    B("mute", "sound",    "silent (the device stays open)", g_mute, s_mute),
    RM("sound.drops", "sound",  "drop plinks, 1 = as designed", 0, 4, 1.25, 0.05, g_drops, s_drops),
    RM("sound.bed", "sound",    "the rain bed", 0, 4, 1.25, 0.05, g_bed, s_bed),
    RM("sound.brown", "sound",  "brown noise", 0, 4, 1.25, 0.05, g_brown, s_brown),
    RM("sound.breeze", "sound", "the breeze", 0, 4, 1.25, 0.05, g_sbreeze, s_sbreeze),
    RM("sound.harsh", "sound",  "gusts and rustle in the breeze", 0, 4, 1.25, 0.05, g_harsh, s_harsh),
};
#define NPARAMS ((int)(sizeof params / sizeof params[0]))

int param_count(void) { return NPARAMS; }
const param *param_at(int i) { return (i >= 0 && i < NPARAMS) ? &params[i] : NULL; }

static int keyeq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x == '_') x = '-';
        if (y == '_') y = '-';
        if (x != y) return 0;
    }
    return *a == *b;
}

const param *param_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < NPARAMS; i++) if (keyeq(params[i].name, name)) return &params[i];
    return NULL;
}

static int enum_count(const param *p) { int n = 0; while (p->names[n]) n++; return n; }

static double clamp_to(const param *p, double v)
{
    double lo = p->lo, hi = p->hi;
    if (p->kind == PK_ENUM) hi = enum_count(p) - 1;
    if (p->wrap == 2) return v;
    if (p->wrap == 1) {
        const double span = hi - lo + (p->kind == PK_ENUM ? 1 : 0);   /* n names wrap at n, not n-1 */
        if (span > 0) { v = fmod(v - lo, span); if (v < 0) v += span; v += lo; }
        return v;
    }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

double param_get(const app *a, const char *name)
{
    const param *p = param_find(name);
    return p ? p->get(a) : 0.0;
}

int param_set(app *a, const char *name, double v)
{
    const param *p = param_find(name);
    if (!p) return -1;
    if (v != v) return -2;                          /* NaN */
    if (p->kind == PK_BOOL) v = v > 0.5 ? 1 : 0;
    else if (p->kind == PK_ENUM || p->kind == PK_INT) v = floor(v + 0.5);
    p->set(a, clamp_to(p, v));
    a->hud_dirty = 1;
    if (a->sc && !a->in_script) script_user_set(a->sc, p->name);   /* the person wins */
    return 0;
}

int param_set_str(app *a, const char *name, const char *val)
{
    const param *p = param_find(name);
    if (!p || !val) return -1;
    while (isspace((unsigned char)*val)) val++;
    if (p->kind == PK_BOOL) {
        if (keyeq(val, "on") || keyeq(val, "yes") || keyeq(val, "true") || keyeq(val, "1")) return param_set(a, name, 1);
        if (keyeq(val, "off") || keyeq(val, "no") || keyeq(val, "false") || keyeq(val, "0")) return param_set(a, name, 0);
        if (keyeq(val, "toggle")) return param_nudge(a, name, 0);
        return -2;
    }
    if (p->kind == PK_ENUM) {
        for (int i = 0; p->names[i]; i++) if (keyeq(val, p->names[i])) return param_set(a, name, i);
        /* a few spellings people will type */
        if (keyeq(val, "rectangle") || keyeq(val, "square")) return param_set(a, name, 0);
        if (keyeq(val, "x0")) return param_set(a, name, 0);
        if (keyeq(val, "xL")) return param_set(a, name, 1);
        if (keyeq(val, "y0")) return param_set(a, name, 2);
        if (keyeq(val, "yL")) return param_set(a, name, 3);
        if (keyeq(val, "auto")) return 0;              /* "the preset's": leave it */
        char *end;
        long n = strtol(val, &end, 10);
        if (end == val) return -2;
        if (keyeq(name, "preset")) n -= 1;             /* people count presets 1..4, as the keys do */
        return param_set(a, name, (double)n);
    }
    char *end;
    double v = strtod(val, &end);
    if (end == val) return -2;
    if (*end == '%') v /= 100.0;
    return param_set(a, name, v);
}

int param_nudge(app *a, const char *name, int dir)
{
    const param *p = param_find(name);
    if (!p) return -1;
    double v = p->get(a);
    if (p->kind == PK_BOOL) return param_set(a, name, v > 0.5 ? 0 : 1);
    if (p->kind == PK_ENUM) {
        const int n = enum_count(p);
        int i = (int)v + (dir < 0 ? -1 : 1);
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        return param_set(a, name, i);
    }
    if (dir == 0) return 0;
    if (p->mult) {
        if (p->snap > 0) {                       /* a level that may be off */
            if (dir > 0 && v < p->snap) v = p->snap;
            else if (dir < 0 && v < p->snap * 1.2) v = 0;
            else v = dir > 0 ? v * p->step : v / p->step;
        } else v = dir > 0 ? v * p->step : v / p->step;
    } else v += dir > 0 ? p->step : -p->step;
    return param_set(a, name, v);
}

int param_get_str(const app *a, const char *name, char *buf, size_t n)
{
    const param *p = param_find(name);
    if (!p) return -1;
    const double v = p->get(a);
    switch (p->kind) {
    case PK_BOOL: snprintf(buf, n, "%s", v > 0.5 ? "on" : "off"); break;
    case PK_ENUM: {
        int i = (int)v, c = enum_count(p);
        if (i < 0 || i >= c) i = 0;
        if (keyeq(name, "preset")) snprintf(buf, n, "%d", i + 1);
        else snprintf(buf, n, "%s", p->names[i]);
        break;
    }
    case PK_INT: snprintf(buf, n, "%d", (int)v); break;
    default: snprintf(buf, n, "%.6g", v); break;
    }
    return 0;
}

int param_is_derived(const app *a, const char *name)
{
    const preset *p = &presets[a->preset];
    if (!strcmp(name, "width"))  return fabs(a->w->Lx - p->L) < 1e-9;
    if (!strcmp(name, "length")) return fabs(a->w->Ly - p->L) < 1e-9;
    if (!strcmp(name, "depth"))  return fabs(a->w->depth - p->depth) < 1e-9;
    if (!strcmp(name, "floor"))  return a->p3.floor_style == p->floor;
    if (!strcmp(name, "paddle-freq")) return fabs(a->paddle_div - 8.0) < 1e-9;
    return 0;
}

void param_list(const app *a, FILE *f)
{
    for (int i = 0; i < NPARAMS; i++) {
        const param *p = &params[i];
        char v[64] = "-", range[96];
        if (a) param_get_str(a, p->name, v, sizeof v);
        if (p->kind == PK_BOOL) snprintf(range, sizeof range, "on/off");
        else if (p->kind == PK_ENUM) {
            int k = 0;
            range[0] = 0;
            for (int j = 0; p->names[j]; j++) k += snprintf(range + k, sizeof range - k, "%s%s", j ? "/" : "", p->names[j]);
        } else snprintf(range, sizeof range, "%g .. %g%s", p->lo, p->hi, p->mult ? "" : "");
        fprintf(f, "%-14s %-10s %-34s  %s\n", p->name, v, range, p->help);
    }
}
