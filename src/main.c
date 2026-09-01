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

/* Entry point: SDL2 window, input, timing; Linux / macOS via OpenGL 3.3,
 * browser via Emscripten (WebGL2), same source. */
#include "wave.h"
#include "render.h"
#include "view3d.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SUBSTEPS_PER_SEC 240.0   /* rotor table granularity at time warp 1 */

typedef struct { const char *name; double L, depth; int floor; } preset;
static const preset presets[] = {
    { "tray",  0.30,  0.02, 0 },
    { "pond",  3.00,  0.60, 2 },
    { "pool", 12.00,  2.00, 0 },
    { "sea",  80.00, 30.00, 2 },
};
#define NPRESETS ((int)(sizeof presets / sizeof presets[0]))

static const char *const help_lines[] = {
    "pond - dispersive waves in a rectangular basin",
    "",
    "click / shift-click on water   drop / big drop",
    "drag on water                  finger",
    "drag elsewhere, ctrl+drag,     orbit",
    "  right/middle drag, arrows, two fingers",
    "wheel, PgUp/PgDn               zoom",
    "o                              reset camera",
    "t         container: opaque, floor only,",
    "          glass, glass+bottom, none",
    "",
    "1-4       tray 30 cm, pond 3 m, pool 12 m, sea 80 m",
    "[ ]  { }  width, length (5%; hold to sweep)",
    ", .  \\    depth;  square it up (same area)",
    "r i/I     rain on/off, rate",
    "b         breeze (wind sea)",
    "p k/K     wavemaker on/off, wavelength",
    "- =       time warp        x/X   damping",
    "g/G f     display gain, floor pattern",
    "c space   clear, pause     s     screenshot",
    "h / F1    help             q/esc quit",
    "",
    "w^2 = (gk + sk^3/rho) tanh(kh)   gamma = 2nuk^2 + g0",
};
#define NHELP ((int)(sizeof help_lines / sizeof help_lines[0]))

typedef struct {
    SDL_Window *win;
    /* 2-D path */
    SDL_Renderer *ren;
    SDL_Texture *tex;
    render_params rp;
    uint32_t *pix;
    SDL_Rect dst;
    /* 3-D path */
    view3d *v3;
    view3d_params p3;
    int mode3d, show_help;

    wave *w;
    int nx, ny;
    int running, paused, hud_dirty;
    int rain, breeze, paddle;
    double rain_rate;          /* drops per simulated second */
    double warp;               /* simulated seconds per real second */
    double acc;                /* simulated time not yet stepped */
    double paddle_div;         /* paddle wavelength = L / paddle_div */
    double paddle_gain, breeze_gain, finger_gain;
    int preset;

    int dragging, orbiting, mx, my;
    int touch_active; float tx, ty;   /* two-finger gesture (touch screens, browsers) */
    Uint64 prev;
    int shots, shot_pending;
    const char *snap_path;     /* --snap3d: save the last frame here */
    int frames_left;           /* --frames N: quit after N frames (0 = never) */
} app;

static void print_help(void)
{
    for (int k = 0; k < NHELP; k++) puts(help_lines[k]);
    puts("\n  --grid N      mode grid (power of two, default 512 native / 256 web)\n"
         "  --window WxH  window size in pixels (default 1280x800; a single number means 16:10)\n"
         "  --preset N    start with preset 1..4\n"
         "  --basin WxL   basin width x length in metres (overrides the preset)\n"
         "  --depth H     depth in metres\n"
         "  --2d          top-down CPU renderer instead of the 3-D view\n"
         "  --nomsaa      do not ask for a multisampled framebuffer\n"
         "  --cam Y,P,D   camera yaw, pitch (deg) and distance (in basin lengths)\n"
         "  --glass N     0 opaque, 1 floor only, 2 glass walls, 3 glass walls + bottom, 4 no walls\n"
         "  --frames N    quit after N displayed frames\n"
         "  --snap3d F    with --frames: save the last 3-D frame to F (bmp)\n"
         "  --bench N     run N headless frames of the CPU path, print timings, exit\n"
         "  --snap F      with --bench: save the last frame to F (bmp)\n"
         "  --scene S     sources to start with: any of rain,paddle,breeze\n");
}

static void pool_changed(app *a)
{
    if (a->v3) view3d_set_pool(a->v3, a->w);
    a->hud_dirty = 1;
}

static void set_preset(app *a, int p)
{
    if (p < 0 || p >= NPRESETS) return;
    a->preset = p;
    wave_set_pool(a->w, presets[p].L, presets[p].L, presets[p].depth);
    a->rp.floor_style = a->p3.floor_style = presets[p].floor;
    if (a->v3) view3d_reset_camera(a->v3, a->w);
    a->hud_dirty = 1;
}

static void update_hud(app *a)
{
    static const char *glass_names[] = { "opaque", "floor only", "glass walls", "glass walls + bottom", "no walls" };
    char buf[256], line2[128];
    snprintf(line2, sizeof line2, "warp %.2gx  gain %.2g  damp %.3g/s %s%s%s%s",
             a->warp, (double)a->p3.gain, a->w->gamma0,
             a->rain ? " rain" : "", a->breeze ? " breeze" : "", a->paddle ? " paddle" : "", a->paused ? "  PAUSED" : "");
    snprintf(buf, sizeof buf, "%s  %.3g x %.3g m  h=%.3g m  %s\n%s",
             presets[a->preset].name, a->w->Lx, a->w->Ly, a->w->depth, glass_names[a->p3.glass], line2);
    if (a->v3) view3d_set_overlay(a->v3, buf, help_lines, NHELP, a->show_help);
    char title[300];
    snprintf(title, sizeof title, "pond  |  %s  %.3g x %.3g m  h=%.3g m  |  %s",
             presets[a->preset].name, a->w->Lx, a->w->Ly, a->w->depth, line2);
    SDL_SetWindowTitle(a->win, title);
    a->hud_dirty = 0;
}

/* window pixel -> basin metres; returns 0 if not on the water */
static int to_basin(const app *a, int mx, int my, double *x, double *y)
{
    if (a->mode3d) return view3d_pick(a->v3, mx, my, x, y);
    int ww, wh, ow, oh;
    SDL_GetWindowSize(a->win, &ww, &wh);
    SDL_GetRendererOutputSize(a->ren, &ow, &oh);
    double sx = ww > 0 ? (double)ow / ww : 1.0, sy = wh > 0 ? (double)oh / wh : 1.0;
    double px = mx * sx - a->dst.x, py = my * sy - a->dst.y;
    if (px < 0 || py < 0 || px >= a->dst.w || py >= a->dst.h) return 0;
    *x = px / a->dst.w * a->w->Lx;
    *y = py / a->dst.h * a->w->Ly;
    return 1;
}

static void drop_at(app *a, int mx, int my, double rel_size)
{
    double x, y;
    if (!to_basin(a, mx, my, &x, &y)) return;
    double s = rel_size * sqrt(a->w->Lx * a->w->Ly);
    wave_add_drop(a->w, x, y, s, -0.15 * s);
}

static void save_bmp_rgba(const char *name, uint8_t *rgba, int w, int h)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(rgba, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    if (s) { if (SDL_SaveBMP(s, name) == 0) printf("saved %s\n", name); SDL_FreeSurface(s); }
}

static void save_screenshot_2d(app *a)
{
#ifndef __EMSCRIPTEN__
    char name[64];
    snprintf(name, sizeof name, "pond-%03d.bmp", a->shots++);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(a->pix, a->nx, a->ny, 32, a->nx * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { if (SDL_SaveBMP(s, name) == 0) printf("saved %s\n", name); SDL_FreeSurface(s); }
#else
    (void)a;
#endif
}

static void handle_key(app *a, SDL_Keycode k, int shift)
{
    wave *w = a->w;
    switch (k) {
    case SDLK_q: case SDLK_ESCAPE: a->running = 0; break;
    case SDLK_h: case SDLK_F1:
        a->show_help = !a->show_help;
        if (!a->mode3d) print_help();
        break;
    case SDLK_SPACE: a->paused = !a->paused; break;
    case SDLK_c: wave_clear(w); break;
    case SDLK_r: a->rain = !a->rain; break;
    case SDLK_b: a->breeze = !a->breeze; break;
    case SDLK_p: a->paddle = !a->paddle; break;
    case SDLK_v: a->rp.view = !a->rp.view; break;
    case SDLK_f: a->rp.floor_style = a->p3.floor_style = (a->p3.floor_style + 1) % 3; break;
    case SDLK_t: a->p3.glass = (a->p3.glass + 1) % 5; break;
    case SDLK_o: if (a->v3) view3d_reset_camera(a->v3, w); break;
    case SDLK_s:
        if (a->mode3d) a->shot_pending = 1; else save_screenshot_2d(a);
        break;
    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: set_preset(a, k - SDLK_1); break;
    case SDLK_i: a->rain_rate *= shift ? 1.5 : 1.0 / 1.5; break;
    case SDLK_k: a->paddle_div *= shift ? 1.0 / 1.25 : 1.25; break;
    /* basin dimensions, 5 % per press; keys auto-repeat, so holding one sweeps smoothly.
     * The grid stays put, so the cells stretch; the aspect ratio is kept within 4:1. */
    case SDLK_LEFTBRACKET: case SDLK_RIGHTBRACKET: {
        double f = (k == SDLK_RIGHTBRACKET) ? 1.05 : 1.0 / 1.05;
        double Lx = shift ? w->Lx : w->Lx * f, Ly = shift ? w->Ly * f : w->Ly;
        if (Lx / Ly <= 4.0 && Ly / Lx <= 4.0) { wave_set_pool(w, Lx, Ly, w->depth); pool_changed(a); }
        break;
    }
    case SDLK_COMMA:  wave_set_pool(w, w->Lx, w->Ly, w->depth / 1.1); pool_changed(a); break;
    case SDLK_PERIOD: wave_set_pool(w, w->Lx, w->Ly, w->depth * 1.1); pool_changed(a); break;
    case SDLK_BACKSLASH: {   /* make it square again, keeping the area */
        double L = sqrt(w->Lx * w->Ly);
        wave_set_pool(w, L, L, w->depth); pool_changed(a); break;
    }
    case SDLK_MINUS: case SDLK_KP_MINUS: a->warp /= 1.5; break;
    case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: a->warp *= 1.5; break;
    case SDLK_x: wave_set_damping(w, shift ? w->gamma0 * 2.0 : w->gamma0 / 2.0); break;
    case SDLK_g: a->rp.gain = a->p3.gain = a->p3.gain * (shift ? 1.5f : 1.0f / 1.5f); break;
    case SDLK_LEFT:  if (a->v3) view3d_orbit(a->v3, -5.0f, 0.0f); return;
    case SDLK_RIGHT: if (a->v3) view3d_orbit(a->v3,  5.0f, 0.0f); return;
    case SDLK_UP:    if (a->v3) view3d_orbit(a->v3, 0.0f,  3.0f); return;
    case SDLK_DOWN:  if (a->v3) view3d_orbit(a->v3, 0.0f, -3.0f); return;
    case SDLK_PAGEUP:   if (a->v3) view3d_zoom(a->v3, 0.8f); return;
    case SDLK_PAGEDOWN: if (a->v3) view3d_zoom(a->v3, 1.25f); return;
    default: return;
    }
    a->hud_dirty = 1;
}

static void handle_events(app *a)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: a->running = 0; break;
        case SDL_KEYDOWN:
            handle_key(a, e.key.keysym.sym, (e.key.keysym.mod & KMOD_SHIFT) != 0);
            break;
        case SDL_MOUSEBUTTONDOWN: {
            SDL_Keymod mods = SDL_GetModState();
            int shift = (mods & KMOD_SHIFT) != 0, modifier = (mods & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0;
            double x, y;
            a->mx = e.button.x; a->my = e.button.y;
            if (e.button.button == SDL_BUTTON_LEFT && !modifier && to_basin(a, e.button.x, e.button.y, &x, &y)) {
                /* on the water: drop, or a finger if the button stays down */
                double sz = (shift ? 0.06 : 0.03) * sqrt(a->w->Lx * a->w->Ly);
                wave_add_drop(a->w, x, y, sz, -0.15 * sz);
                a->dragging = !shift;
            } else if (a->mode3d) {
                /* off the water, with a modifier, or any other button: orbit */
                a->orbiting = 1;
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                drop_at(a, e.button.x, e.button.y, 0.06);
            }
            break;
        }
        case SDL_MOUSEBUTTONUP:
            a->dragging = 0;
            a->orbiting = 0;
            break;
        case SDL_MOUSEMOTION:
            if (a->orbiting && a->v3 && !a->touch_active)
                view3d_orbit(a->v3, 0.3f * (float)e.motion.xrel, -0.3f * (float)e.motion.yrel);
            a->mx = e.motion.x; a->my = e.motion.y;
            break;
        case SDL_MULTIGESTURE:
            /* two fingers: drag orbits, pinch zooms; the single-finger mouse emulation is suppressed */
            if (a->v3 && e.mgesture.numFingers >= 2) {
                int ww, wh;
                SDL_GetWindowSize(a->win, &ww, &wh);
                if (a->touch_active)
                    view3d_orbit(a->v3, 0.3f * (e.mgesture.x - a->tx) * (float)ww, -0.3f * (e.mgesture.y - a->ty) * (float)wh);
                float z = 1.0f - 4.0f * e.mgesture.dDist;
                if (z < 0.5f) z = 0.5f;
                if (z > 2.0f) z = 2.0f;
                view3d_zoom(a->v3, z);
                a->tx = e.mgesture.x; a->ty = e.mgesture.y;
                a->touch_active = 1;
                a->dragging = 0;
            }
            break;
        case SDL_FINGERUP:
            a->touch_active = 0;
            break;
        case SDL_MOUSEWHEEL:
            if (a->v3) { if (e.wheel.y > 0) view3d_zoom(a->v3, 0.85f); else if (e.wheel.y < 0) view3d_zoom(a->v3, 1.0f / 0.85f); }
            else { a->warp *= e.wheel.y > 0 ? 1.25 : (e.wheel.y < 0 ? 0.8 : 1.0); a->hud_dirty = 1; }
            break;
        default: break;
        }
    }
}

/* Sources for one frame of simulated duration dts. */
static void apply_sources(app *a, double dts)
{
    wave *w = a->w;
    const double L = sqrt(w->Lx * w->Ly);     /* size scale for drops, paddle, breeze */

    if (a->dragging) {
        double x, y;
        if (to_basin(a, a->mx, a->my, &x, &y)) {
            double s = 0.015 * L;
            wave_add_drop(w, x, y, s, -a->finger_gain * 4.0 * s * dts);
        }
    }
    if (a->rain) {
        /* Poisson number of drops this frame */
        double lam = a->rain_rate * dts, p = exp(-lam), u = rand() / (RAND_MAX + 1.0);
        int n = 0;
        while (u > p && n < 50) { u -= p; n++; p *= lam / n; }
        for (int i = 0; i < n; i++) {
            double x = rand() / (RAND_MAX + 1.0) * w->Lx, y = rand() / (RAND_MAX + 1.0) * w->Ly;
            double s = 0.02 * L * (0.5 + rand() / (RAND_MAX + 1.0));
            wave_add_drop(w, x, y, s, -0.15 * s);
        }
    }
    if (a->paddle) {
        double k = 2.0 * M_PI * a->paddle_div / L;
        double om = wave_omega(w, k);
        double width = 0.006 * L;
        if (width < 2.0 * w->dx) width = 2.0 * w->dx;
        double accel = a->paddle_gain * 0.0025 * L * om * om * cos(om * w->t);
        wave_add_paddle(w, width, accel, dts);
    }
    if (a->breeze) {
        double k0 = 2.0 * M_PI * 8.0 / L;        /* spectral peak at L/8 */
        wave_breeze(w, k0, a->breeze_gain * 3.0e-4 * L, dts);
    }
}

static void advance(app *a, double dt_real)
{
    double dts = a->paused ? 0.0 : dt_real * a->warp;
    if (!a->paused) apply_sources(a, dts);
    a->acc += dts;
    double sub = a->warp / SUBSTEPS_PER_SEC;
    int n = (int)(a->acc / sub);
    if (n > 240) { n = 240; a->acc = 0.0; } else a->acc -= n * sub;
    wave_step(a->w, sub, n);       /* also injects pending sources when n == 0 */
}

static void frame(void *ud)
{
    app *a = ud;
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - a->prev) / (double)SDL_GetPerformanceFrequency();
    a->prev = now;
    if (dt > 0.1) dt = 0.1;
    if (dt < 0.0) dt = 0.0;

    handle_events(a);
    if (!a->running) {
#ifdef __EMSCRIPTEN__
        emscripten_cancel_main_loop();
#endif
        return;
    }

    advance(a, dt);
    wave_realize(a->w);
    if (a->hud_dirty) update_hud(a);

    if (a->mode3d) {
        int last = (a->frames_left == 1);
        int capture = a->shot_pending || (last && a->snap_path);
        if (capture) view3d_request_capture(a->v3);
        view3d_render(a->v3, a->w, &a->p3);
        if (capture) {
            int cw, ch;
            uint8_t *rgba = view3d_take_capture(a->v3, &cw, &ch);
            if (rgba) {
                char name[64];
                const char *path = (last && a->snap_path) ? a->snap_path : name;
                if (path == name) snprintf(name, sizeof name, "pond-%03d.bmp", a->shots++);
                save_bmp_rgba(path, rgba, cw, ch);
                free(rgba);
            }
            a->shot_pending = 0;
        }
    } else {
        int ow, oh;
        SDL_GetRendererOutputSize(a->ren, &ow, &oh);
        double ar = a->w->Ly / a->w->Lx;
        int dw = ow, dh = (int)(ow * ar);
        if (dh > oh) { dh = oh; dw = (int)(oh / ar); }
        a->dst.x = (ow - dw) / 2; a->dst.y = (oh - dh) / 2; a->dst.w = dw; a->dst.h = dh;
        render_frame(a->w, &a->rp, a->pix);
        SDL_UpdateTexture(a->tex, NULL, a->pix, a->nx * (int)sizeof(uint32_t));
        SDL_SetRenderDrawColor(a->ren, 8, 8, 10, 255);
        SDL_RenderClear(a->ren);
        SDL_RenderCopy(a->ren, a->tex, NULL, &a->dst);
        SDL_RenderPresent(a->ren);
    }
    if (a->frames_left > 0 && --a->frames_left == 0) a->running = 0;
}

static int bench(app *a, int frames, const char *snap, const char *scene)
{
    const double dt = 1.0 / 60.0;
    a->rain_rate = 3.0;
    if (!scene || !strcmp(scene, "all")) a->rain = a->paddle = a->breeze = 1;
    else { a->rain = !!strstr(scene, "rain"); a->paddle = !!strstr(scene, "paddle"); a->breeze = !!strstr(scene, "breeze"); }
    Uint64 f = SDL_GetPerformanceFrequency();
    double t_src = 0, t_step = 0, t_real = 0, t_rend = 0;
    for (int i = 0; i < frames; i++) {
        Uint64 t0 = SDL_GetPerformanceCounter();
        apply_sources(a, dt);
        Uint64 t1 = SDL_GetPerformanceCounter();
        a->acc += dt;
        double sub = 1.0 / SUBSTEPS_PER_SEC;
        int n = (int)(a->acc / sub); a->acc -= n * sub;
        wave_step(a->w, sub, n);
        Uint64 t2 = SDL_GetPerformanceCounter();
        wave_realize(a->w);
        Uint64 t3 = SDL_GetPerformanceCounter();
        render_frame(a->w, &a->rp, a->pix);
        Uint64 t4 = SDL_GetPerformanceCounter();
        t_src += (double)(t1 - t0) / f; t_step += (double)(t2 - t1) / f;
        t_real += (double)(t3 - t2) / f; t_rend += (double)(t4 - t3) / f;
    }
    double tot = t_src + t_step + t_real + t_rend;
    printf("grid %dx%d, %d frames (%.1f s simulated), preset %s\n", a->nx, a->ny, frames, frames * dt, presets[a->preset].name);
    printf("  sources %.2f ms   inject+step %.2f ms   inverse DCT %.2f ms   2-D render %.2f ms   total %.2f ms/frame (%.0f fps)\n",
           1e3 * t_src / frames, 1e3 * t_step / frames, 1e3 * t_real / frames, 1e3 * t_rend / frames,
           1e3 * tot / frames, frames / tot);
    printf("  rms slope %.4f   mode norm %.3e\n", wave_rms_slope(a->w), wave_norm(a->w));
    if (snap) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(a->pix, a->nx, a->ny, 32, a->nx * 4, SDL_PIXELFORMAT_ARGB8888);
        if (s && SDL_SaveBMP(s, snap) == 0) printf("  saved %s\n", snap);
        if (s) SDL_FreeSurface(s);
    }
    return 0;
}

#ifdef __EMSCRIPTEN__
/* The browser passes no arguments.  A no-argument main also avoids clang's
 * __main_argc_argv rename, which Emscripten releases before ~3.1.8 (e.g. the
 * one Debian/Ubuntu package) do not know how to export. */
static int pond_main(int argc, char **argv);
int main(void)
{
    char *argv[] = { "pond", NULL };
    return pond_main(1, argv);
}
#define POND_MAIN static int pond_main
#else
#define POND_MAIN int main
#endif

POND_MAIN(int argc, char **argv)
{
    int grid = 512, winw = 1280, winh = 800, bench_frames = 0, preset0 = 1, mode3d = 1, glass0 = 0;
    const char *snap = NULL, *scene = NULL, *snap3d = NULL, *cam = NULL, *basin = NULL;
    double depth_arg = 0;
    int a_frames = 0, a_help = 0, msaa = 1;
#ifdef __EMSCRIPTEN__
    grid = emscripten_run_script_int("(function(){var v=new URLSearchParams(location.search).get('grid');return v?(v|0):0;})()");
    if (grid <= 0) grid = 256;
    if (grid > 512) grid = 512;      /* fixed 128 MB heap in the browser */
    winw = 1280; winh = 800;         /* the shell's CSS keeps this 16:10 shape */
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--grid") && i + 1 < argc) grid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            const char *g = argv[++i], *x = strchr(g, 'x');
            winw = atoi(g);
            winh = x ? atoi(x + 1) : winw * 10 / 16;
        }
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) preset0 = atoi(argv[++i]) - 1;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc) snap = argv[++i];
        else if (!strcmp(argv[i], "--snap3d") && i + 1 < argc) snap3d = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) a_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cam") && i + 1 < argc) cam = argv[++i];
        else if (!strcmp(argv[i], "--basin") && i + 1 < argc) basin = argv[++i];
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth_arg = atof(argv[++i]);
        else if (!strcmp(argv[i], "--glass") && i + 1 < argc) glass0 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--2d")) mode3d = 0;
        else if (!strcmp(argv[i], "--overlay")) a_help = 1;
        else if (!strcmp(argv[i], "--nomsaa")) msaa = 0;
        else { print_help(); return !strcmp(argv[i], "--help") ? 0 : 1; }
    }
    if (grid < 16 || (grid & (grid - 1))) { fprintf(stderr, "grid must be a power of two >= 16\n"); return 1; }
    if (preset0 < 0 || preset0 >= NPRESETS) preset0 = 1;
    if (glass0 < 0 || glass0 > 4) glass0 = 0;

    /* static: with Emscripten the main-loop call unwinds main's stack frame */
    static app a;
    memset(&a, 0, sizeof a);
    a.nx = a.ny = grid;
    a.running = 1;
    a.warp = 1.0;
    a.rain_rate = 2.0;
    a.paddle_div = 8.0;
    a.paddle_gain = a.breeze_gain = a.finger_gain = 1.0;
    a.frames_left = a_frames;
    a.snap_path = snap3d;
    a.mode3d = mode3d;
    a.show_help = a_help;
    render_defaults(&a.rp);
    a.p3.gain = 1.0f;
    a.p3.glass = glass0;
    { float s[3] = { 0.30f, 0.90f, -0.25f }; float n = 1.0f / sqrtf(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
      a.p3.sun[0] = s[0] * n; a.p3.sun[1] = s[1] * n; a.p3.sun[2] = s[2] * n; }
    a.pix = malloc((size_t)grid * grid * sizeof(uint32_t));
    a.w = wave_create(grid, grid, presets[preset0].L, presets[preset0].L, presets[preset0].depth);
    if (!a.pix || !a.w) { fprintf(stderr, "out of memory\n"); return 1; }
    a.preset = preset0;
    a.rp.floor_style = a.p3.floor_style = presets[preset0].floor;
    if (basin || depth_arg > 0) {
        double Lx = a.w->Lx, Ly = a.w->Ly, h = depth_arg > 0 ? depth_arg : a.w->depth;
        if (basin) { const char *x = strchr(basin, 'x'); Lx = atof(basin); Ly = x ? atof(x + 1) : Lx; }
        if (Lx > 0 && Ly > 0) wave_set_pool(a.w, Lx, Ly, h);
    }

    if (scene && bench_frames == 0) {
        a.rain = !!strstr(scene, "rain"); a.paddle = !!strstr(scene, "paddle"); a.breeze = !!strstr(scene, "breeze");
    }
    if (bench_frames > 0) {
        SDL_Init(0);
        int rc = bench(&a, bench_frames, snap, scene);
        wave_destroy(a.w); free(a.pix);
        SDL_Quit();
        return rc;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (mode3d ? SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI : 0);
    if (mode3d) view3d_gl_attributes(msaa);
    a.win = SDL_CreateWindow("pond", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winw, winh, wflags);
    if (!a.win && mode3d && msaa) {
        /* no multisampled visual on this display: try again without */
        fprintf(stderr, "no multisampled GL visual (%s); retrying without\n", SDL_GetError());
        SDL_GL_ResetAttributes();
        view3d_gl_attributes(0);
        a.win = SDL_CreateWindow("pond", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winw, winh, wflags);
    }
    if (!a.win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    if (mode3d) {
        a.v3 = view3d_create(a.win, grid, grid);
        if (!a.v3) { fprintf(stderr, "3-D view unavailable; try --2d\n"); return 1; }
        view3d_reset_camera(a.v3, a.w);
        if (cam) {
            float yaw = 35, pitch = 42, dist = 1.5f;
            sscanf(cam, "%f,%f,%f", &yaw, &pitch, &dist);
            view3d_set_camera(a.v3, yaw, pitch, dist);
        }
    } else {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        a.ren = SDL_CreateRenderer(a.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!a.ren) a.ren = SDL_CreateRenderer(a.win, -1, 0);
        if (!a.ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
        a.tex = SDL_CreateTexture(a.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, grid, grid);
        if (!a.tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return 1; }
        print_help();
    }

    a.hud_dirty = 1;
    a.prev = SDL_GetPerformanceCounter();

    /* something to look at right away */
    wave_add_drop(a.w, 0.5 * a.w->Lx, 0.5 * a.w->Ly, 0.03 * a.w->Lx, -0.15 * 0.03 * a.w->Lx);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(frame, &a, 0, 1);
#else
    while (a.running) frame(&a);
    if (a.v3) view3d_destroy(a.v3);
    if (a.tex) SDL_DestroyTexture(a.tex);
    if (a.ren) SDL_DestroyRenderer(a.ren);
    SDL_DestroyWindow(a.win);
    SDL_Quit();
    wave_destroy(a.w);
    free(a.pix);
#endif
    return 0;
}
