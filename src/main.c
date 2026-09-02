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
#include "app.h"
#include "param.h"
#include "config.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define SUBSTEPS_PER_SEC 240.0   /* rotor table granularity at time warp 1 */

static const char *const help_lines[] = {
    "pond - dispersive waves in a rectangular basin",
    "",
    "click / shift-click on water   drop / big drop",
    "drag on water                  finger",
    "drag elsewhere, ctrl+drag,     orbit",
    "  right/middle drag, two fingers",
    "arrows (or keypad 4/6/8/2)     orbit",
    "PgUp/PgDn                      zoom",
    "  hold to sweep; shift 3x faster, ctrl finer",
    "wheel                          zoom",
    "o / Home                       reset camera",
    "t         container: opaque, floor only,",
    "          glass, glass+bottom, none",
    "",
    "n         basin shape: rectangle / disk",
    "y         nonlinear (HOS) on / off",
    "m  a/A    sound on/off, volume down/up",
    "j/J u/U   drop level, rain-bed level",
    "z/Z w/W   brown noise, breeze level",
    "e/E       breeze harshness (gusts, rustle)",
    "1-4       tray 30 cm, pond 3 m, pool 12 m, sea 80 m",
    "[ ]  { }  width, length (5%; hold to sweep)",
    ", .  \\    depth;  square it up (same area)",
    "r i/I     rain on/off, rate",
    "b         breeze (wind sea)",
    "p / P     wavemaker on/off, next wall",
    "k/K l/L   its frequency, its span (size)",
    "< >       slide it along the wall",
    "          (its outline is drawn on the water)",
    "- =       time warp        x/X   damping",
    "g/G f     display gain, floor pattern",
    "d         hide / show this settings box",
    "F11 / alt+enter                full screen",
    "c space   clear, pause     s     screenshot",
    "h / F1    help             q/esc quit",
    "",
    "w^2 = (gk + sk^3/rho) tanh(kh)   gamma = 2nuk^2 + g0",
};
#define NHELP ((int)(sizeof help_lines / sizeof help_lines[0]))


static void print_help(void)
{
    for (int k = 0; k < NHELP; k++) puts(help_lines[k]);
    puts("\nBefore the program starts:\n"
         "  --grid N        mode grid (power of two, default 512 native / 256 web)\n"
         "  --window WxH    window size in pixels (default 1280x800; one number means 16:10)\n"
         "  --2d            top-down CPU renderer instead of the 3-D view\n"
         "  --nomsaa        do not ask for a multisampled framebuffer\n"
         "  --cpu-caustics  compute the caustic light map on the CPU (default: GPU when possible)\n"
         "  --no-audio      do not open a sound device at all (--mute keeps it open but silent)\n"
         "  --hos-nc N --hos-order M   the nonlinear correction's modes per axis and order (64, 3)\n"
         "  --preset N      start with preset 1..4;  --shape rect|disk;  --basin WxL;  --depth H\n"
         "\n"
         "Everything else is a parameter, settable as --name value (booleans: --name, --no-name):\n"
         "  --list-params   show them all with their ranges and current values, and exit\n"
         "  e.g. --paddle --paddle-freq 1.2 --paddle-span 0.15 --glass floor --yaw 60 --no-hud\n"
         "  --scene S       any of rain,paddle,breeze     --cam Y,P,D   camera yaw, pitch, distance\n"
         "  --sound K=V,... the five sound levels           --hos         nonlinear correction on\n"
         "\n"
         "Per run:\n"
         "  --frames N      quit after N displayed frames;  --snap3d F  save the last 3-D frame to F (bmp)\n"
         "  --bench N       run N headless frames of the CPU path, print timings, exit;  --snap F  save the last\n"
         "  --config F      read F instead of the default config file;  --no-config  ignore it\n"
         "  --write-config [F]  write the settings as a config file and exit\n"
         "\n"
         "Settings come from built-in defaults, then the config file, then these options.\n"
         "The file is the first of $POND_CONFIG, $XDG_CONFIG_HOME/pond/pond.conf,\n"
         "~/.config/pond/pond.conf, ~/.pondrc, ./pond.conf that exists.  --write-config makes\n"
         "one from the current settings, so \"pond --volume 0.3 --glass floor --write-config\"\n"
         "saves that as the way pond starts from now on.\n");
}

static void update_hud(app *a)
{
    static const char *glass_names[] = { "opaque", "floor only", "glass walls", "glass walls + bottom", "no walls" };
    char buf[640], line2[200];
    char nl[48];
    if (a->hos_on && a->hs) snprintf(nl, sizeof nl, "  HOS M=%d nc=%d%s", hos_order(a->hs), hos_nc(a->hs), a->hos_skipped ? " (too steep)" : "");
    else if (a->hos_on) snprintf(nl, sizeof nl, "  HOS: rectangle only");
    else snprintf(nl, sizeof nl, "  linear");
    char snd[24];
    if (!a->au) snprintf(snd, sizeof snd, "  no sound");
    else if (audio_muted(a->au)) snprintf(snd, sizeof snd, "  muted");
    else snprintf(snd, sizeof snd, "  vol %.0f%%", 100.0 * audio_volume(a->au));
    snprintf(line2, sizeof line2, "warp %.2gx  gain %.2g  damp %.3g/s%s%s%s%s%s%s",
             a->warp, (double)a->p3.gain, a->w->gamma0, nl, snd,
             a->rain ? "  rain" : "", a->breeze ? " breeze" : "", a->paddle ? " paddle" : "", a->paused ? "  PAUSED" : "");
    char dims[64];
    if (a->shape == WAVE_DISK) snprintf(dims, sizeof dims, "disk D=%.3g m", a->w->Lx);
    else snprintf(dims, sizeof dims, "%.3g x %.3g m", a->w->Lx, a->w->Ly);
    char line3[160] = "";
    if (a->au)
        snprintf(line3, sizeof line3, "\nsnd: drops %.0f%%  bed %.0f%%  brown %.0f%%  breeze %.0f%%  harsh %.0f%%",
                 100 * audio_knob(a->au, SND_DROPS), 100 * audio_knob(a->au, SND_BED), 100 * audio_knob(a->au, SND_BROWN),
                 100 * audio_knob(a->au, SND_BREEZE), 100 * audio_knob(a->au, SND_HARSH));
    char line4[128] = "";
    if (a->paddle) {
        static const char *const wall_names[] = { "wall x=0", "wall x=Lx", "wall y=0", "wall y=Ly" };
        const double k = app_paddle_k(a);
        snprintf(line4, sizeof line4, "\npaddle: %.3g Hz  lambda %.3g m  %s  pos %.0f%%  span %.0f%%",
                 app_paddle_hz(a), 2.0 * M_PI / k,
                 a->shape == WAVE_DISK ? "rim" : wall_names[a->paddle_wall],
                 100.0 * a->paddle_pos, 100.0 * a->paddle_span);
    }
    snprintf(buf, sizeof buf, "%s  %s  h=%.3g m  %s\n%s%s%s",
             presets[a->preset].name, dims, a->w->depth, glass_names[a->p3.glass], line2, line3, line4);
    if (a->v3) view3d_set_overlay(a->v3, buf, help_lines, NHELP, a->show_help, a->show_hud);
    char title[300];
    snprintf(title, sizeof title, "pond  |  %s  %s  h=%.3g m  |  %s",
             presets[a->preset].name, dims, a->w->depth, line2);
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

/* a drop into the water, seen and heard */
static void splash(app *a, double x, double y, double s, double amp)
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

static void drop_at(app *a, int mx, int my, double rel_size)
{
    double x, y;
    if (!to_basin(a, mx, my, &x, &y)) return;
    double s = rel_size * sqrt(a->w->Lx * a->w->Ly);
    splash(a, x, y, s, -0.15 * s);
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

/* Held keys steer the camera every frame, so holding one sweeps smoothly
 * instead of stuttering along with the keyboard auto-repeat. */
static void camera_keys(app *a, double dt)
{
    if (!a->v3 || !(SDL_GetWindowFlags(a->win) & SDL_WINDOW_INPUT_FOCUS)) return;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    SDL_Keymod m = SDL_GetModState();
    float sp = (m & KMOD_SHIFT) ? 3.0f : ((m & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) ? 0.25f : 1.0f);
    float deg = sp * 75.0f * (float)dt;                /* degrees per second */
    float yaw = 0.0f, pitch = 0.0f;
    if (ks[SDL_SCANCODE_LEFT]  || ks[SDL_SCANCODE_KP_4]) yaw   -= deg;
    if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_KP_6]) yaw   += deg;
    if (ks[SDL_SCANCODE_UP]    || ks[SDL_SCANCODE_KP_8]) pitch += 0.6f * deg;
    if (ks[SDL_SCANCODE_DOWN]  || ks[SDL_SCANCODE_KP_2]) pitch -= 0.6f * deg;
    if (yaw != 0.0f || pitch != 0.0f) view3d_orbit(a->v3, yaw, pitch);
    int in = ks[SDL_SCANCODE_PAGEUP] != 0, out = ks[SDL_SCANCODE_PAGEDOWN] != 0;
    if (in != out) view3d_zoom(a->v3, powf(2.0f, (out ? 1.0f : -1.0f) * sp * (float)dt));
}

/* Keys that are a parameter nudge: lower case down, upper case up, or a toggle /
 * cycle.  shift: 0 without, 1 with, -1 either way.  What is not a nudge -- quit,
 * clear, screenshot, the presets, reset camera -- is in the switch below. */
typedef struct { SDL_Keycode key; int shift; const char *name; int dir; } keybind;
static const keybind keybinds[] = {
    { SDLK_r, -1, "rain", 0 },
    { SDLK_i,  0, "rain-rate", -1 },      { SDLK_i, 1, "rain-rate", +1 },
    { SDLK_b, -1, "breeze", 0 },
    { SDLK_p,  0, "paddle", 0 },          { SDLK_p, 1, "paddle-wall", +1 },
    { SDLK_k,  0, "paddle-freq", -1 },    { SDLK_k, 1, "paddle-freq", +1 },
    { SDLK_l,  0, "paddle-span", -1 },    { SDLK_l, 1, "paddle-span", +1 },
    { SDLK_COMMA, 1, "paddle-pos", -1 },  { SDLK_PERIOD, 1, "paddle-pos", +1 },
    { SDLK_COMMA, 0, "depth", -1 },       { SDLK_PERIOD, 0, "depth", +1 },
    { SDLK_LEFTBRACKET, 0, "width", -1 }, { SDLK_RIGHTBRACKET, 0, "width", +1 },
    { SDLK_LEFTBRACKET, 1, "length", -1 },{ SDLK_RIGHTBRACKET, 1, "length", +1 },
    { SDLK_n, -1, "shape", 0 },
    { SDLK_y, -1, "nonlinear", 0 },
    { SDLK_x,  0, "damping", -1 },        { SDLK_x, 1, "damping", +1 },
    { SDLK_MINUS, -1, "warp", -1 },       { SDLK_KP_MINUS, -1, "warp", -1 },
    { SDLK_EQUALS, -1, "warp", +1 },      { SDLK_PLUS, -1, "warp", +1 },  { SDLK_KP_PLUS, -1, "warp", +1 },
    { SDLK_g,  0, "gain", -1 },           { SDLK_g, 1, "gain", +1 },
    { SDLK_f, -1, "floor", +1 },
    { SDLK_t, -1, "glass", +1 },
    { SDLK_d, -1, "hud", 0 },
    { SDLK_v, -1, "view", 0 },
    { SDLK_SPACE, -1, "paused", 0 },
    { SDLK_F11, -1, "fullscreen", 0 },
    { SDLK_m, -1, "mute", 0 },
    { SDLK_a,  0, "volume", -1 },         { SDLK_a, 1, "volume", +1 },
    { SDLK_j,  0, "sound.drops", -1 },    { SDLK_j, 1, "sound.drops", +1 },
    { SDLK_u,  0, "sound.bed", -1 },      { SDLK_u, 1, "sound.bed", +1 },
    { SDLK_z,  0, "sound.brown", -1 },    { SDLK_z, 1, "sound.brown", +1 },
    { SDLK_w,  0, "sound.breeze", -1 },   { SDLK_w, 1, "sound.breeze", +1 },
    { SDLK_e,  0, "sound.harsh", -1 },    { SDLK_e, 1, "sound.harsh", +1 },
};
#define NKEYBINDS ((int)(sizeof keybinds / sizeof keybinds[0]))

static void handle_key(app *a, SDL_Keycode k, int shift)
{
    wave *w = a->w;
    const int alt = (SDL_GetModState() & KMOD_ALT) != 0;
    for (int i = 0; i < NKEYBINDS; i++)
        if (keybinds[i].key == k && (keybinds[i].shift < 0 || keybinds[i].shift == shift)) {
            param_nudge(a, keybinds[i].name, keybinds[i].dir);
            return;
        }
    switch (k) {
    case SDLK_q: a->running = 0; break;
    case SDLK_ESCAPE:                       /* leave full screen first, then quit */
        if (a->fullscreen) param_set(a, "fullscreen", 0); else a->running = 0;
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (alt) param_nudge(a, "fullscreen", 0);
        break;
    case SDLK_h: case SDLK_F1:
        param_nudge(a, "help", 0);
        if (!a->mode3d) print_help();
        break;
    case SDLK_c: wave_clear(w); break;
    case SDLK_o: case SDLK_HOME: app_reset_camera(a); break;
    case SDLK_s:
        if (a->mode3d) a->shot_pending = 1; else save_screenshot_2d(a);
        break;
    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: param_set(a, "preset", k - SDLK_1); break;
    case SDLK_BACKSLASH: {   /* make it square again, keeping the area */
        double L = sqrt(w->Lx * w->Ly);
        wave_set_pool(w, L, L, w->depth); app_pool_changed(a); break;
    }
    /* the arrows and PgUp/PgDn are read as held keys in camera_keys() */
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
                splash(a, x, y, sz, -0.15 * sz);
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
            if (a->au && w->t - a->finger_t > 0.12) {       /* a trail of small plinks */
                double pan = 0, att = 1;
                if (a->v3) view3d_listen(a->v3, x, y, &pan, &att);
                audio_splash(a->au, 0.4 * s * 0.3 / L, pan, 0.5 * att);
                a->finger_t = w->t;
            }
        }
    }
    if (a->rain) {
        /* Poisson number of drops this frame */
        double lam = a->rain_rate * dts, p = exp(-lam), u = rand() / (RAND_MAX + 1.0);
        int n = 0;
        while (u > p && n < 50) { u -= p; n++; p *= lam / n; }
        for (int i = 0; i < n; i++) {
            double x, y;
            do {
                x = rand() / (RAND_MAX + 1.0) * w->Lx; y = rand() / (RAND_MAX + 1.0) * w->Ly;
            } while (w->shape == WAVE_DISK && (x - w->R) * (x - w->R) + (y - w->R) * (y - w->R) > w->R * w->R);
            double s = 0.02 * L * (0.5 + rand() / (RAND_MAX + 1.0));
            splash(a, x, y, s, -0.15 * s);
        }
    }
    if (a->paddle) {
        /* A wavemaker run at constant speed rather than constant stroke: peak velocity
         * u0, so the stroke is u0/omega, longest at the low frequencies, and the
         * acceleration is u0 * omega.  A fixed stroke would put the radiated height
         * in proportion to omega and leave the bottom of the dial invisible; at
         * constant speed the frequency knob changes the wavelength and viscosity
         * alone decides how the height falls off.  u0 is set so the default -- eight
         * wavelengths across the basin -- looks exactly as it always did. */
        const double om = wave_omega(w, app_paddle_k(a));
        const double om_ref = wave_omega(w, 2.0 * M_PI * 8.0 / L);
        const double u0 = a->paddle_gain * 0.0025 * L * om_ref;
        const double accel = u0 * om * cos(om * w->t);
        wave_add_paddle(w, a->paddle_wall, a->paddle_pos, a->paddle_span, 0.006 * L, accel, dts);
    }
    if (a->breeze) {
        double k0 = 2.0 * M_PI * 8.0 / L;        /* spectral peak at L/8 */
        double gust = a->au ? 0.5 + audio_gust(a->au) : 1.0;   /* the gusts you hear roughen the water */
        wave_breeze(w, k0, a->breeze_gain * 3.0e-4 * L * gust, dts);
    }
    /* continuous layers */
    if (a->au) {
        audio_set_rain(a->au, a->rain ? (a->rain_rate / 3.0 > 1.0 ? 1.0 : a->rain_rate / 3.0) : 0.0);
        audio_set_wind(a->au, a->breeze ? 1.0 : 0.0);
        if (++a->frame_no % 10 == 0) {
            double Lmax = w->Lx > w->Ly ? w->Lx : w->Ly;
            if (Lmax >= 40.0 && (a->breeze || a->paddle)) {
                double sl = wave_rms_slope(w);
                audio_set_sea(a->au, sl / 0.04 > 1.0 ? 1.0 : sl / 0.04, sl / 0.06);
            } else audio_set_sea(a->au, 0.0, 0.0);
        }
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
    if (a->hos_on && a->shape == WAVE_RECT && n > 0) {
        /* Strang split: half the linear sub-steps, the nonlinear correction, the other half */
        if (!a->hs) {
            a->hs = hos_create(a->w, a->hos_nc, a->hos_order);
            if (!a->hs) { fprintf(stderr, "HOS: could not create (nc=%d)\n", a->hos_nc); a->hos_on = 0; a->hud_dirty = 1; }
            else a->hud_dirty = 1;
        }
        if (a->hs) {
            int n1 = n / 2;
            wave_step(a->w, sub, n1);
            int applied = hos_step(a->hs, a->w, n * sub);
            if (applied == a->hos_skipped) { a->hos_skipped = !applied; a->hud_dirty = 1; }
            wave_step(a->w, sub, n - n1);
            return;
        }
    }
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

    camera_keys(a, dt);
    advance(a, dt);
    wave_realize(a->w);
    if (a->hud_dirty) update_hud(a);

    /* the wavemaker's outline, so its place and size can be seen while they change;
     * d takes it away with the rest of the writing on the screen */
    a->p3.paddle = a->paddle && a->show_hud;
    a->p3.paddle_wall = a->paddle_wall;
    a->p3.paddle_pos = (float)a->paddle_pos;
    a->p3.paddle_span = (float)a->paddle_span;
    a->p3.paddle_width = (float)(0.006 * sqrt(a->w->Lx * a->w->Ly));

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
                if (getenv("POND_DUMP")) {   /* the height field next to the screenshot, for debugging */
                    FILE *fp = fopen(getenv("POND_DUMP"), "wb");
                    if (fp) { fwrite(&a->w->nx, sizeof(int), 1, fp); fwrite(&a->w->ny, sizeof(int), 1, fp);
                              fwrite(a->w->eta, sizeof(float), (size_t)a->w->nx * a->w->ny, fp); fclose(fp); }
                }
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

static int bench(app *a, int frames, const char *snap)
{
    const double dt = 1.0 / 60.0;
    a->rain_rate = 3.0;
    if (!a->rain && !a->paddle && !a->breeze) a->rain = a->paddle = a->breeze = 1;   /* no --scene: everything */
    Uint64 f = SDL_GetPerformanceFrequency();
    double t_src = 0, t_step = 0, t_real = 0, t_rend = 0;
    for (int i = 0; i < frames; i++) {
        Uint64 t0 = SDL_GetPerformanceCounter();
        apply_sources(a, dt);
        Uint64 t1 = SDL_GetPerformanceCounter();
        a->acc += dt;
        double sub = 1.0 / SUBSTEPS_PER_SEC;
        int n = (int)(a->acc / sub); a->acc -= n * sub;
        if (a->hos_on && a->shape == WAVE_RECT) {
            if (!a->hs) a->hs = hos_create(a->w, a->hos_nc, a->hos_order);
            wave_step(a->w, sub, n / 2);
            if (a->hs) hos_step(a->hs, a->w, n * sub);
            wave_step(a->w, sub, n - n / 2);
        } else wave_step(a->w, sub, n);
        Uint64 t2 = SDL_GetPerformanceCounter();
        wave_realize(a->w);
        Uint64 t3 = SDL_GetPerformanceCounter();
        if (a->shape == WAVE_RECT) render_frame(a->w, &a->rp, a->pix);
        Uint64 t4 = SDL_GetPerformanceCounter();
        t_src += (double)(t1 - t0) / f; t_step += (double)(t2 - t1) / f;
        t_real += (double)(t3 - t2) / f; t_rend += (double)(t4 - t3) / f;
    }
    double tot = t_src + t_step + t_real + t_rend;
    printf("grid %dx%d, %d frames (%.1f s simulated), preset %s\n", a->nx, a->ny, frames, frames * dt, presets[a->preset].name);
    printf("  sources %.2f ms   inject+step%s %.2f ms   inverse DCT %.2f ms   2-D render %.2f ms   total %.2f ms/frame (%.0f fps)\n",
           1e3 * t_src / frames, a->hos_on ? "+HOS" : "", 1e3 * t_step / frames, 1e3 * t_real / frames, 1e3 * t_rend / frames,
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
    pond_config cfg;
    config_defaults(&cfg);

    /* the config file, first: the command line below overrides it */
    const char *cfg_path = NULL, *write_cfg = NULL;
    int no_config = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg_path = argv[i + 1];
        else if (!strcmp(argv[i], "--no-config")) no_config = 1;
    }
    if (!no_config) {
        int asked = cfg_path != NULL;
        if (!cfg_path) cfg_path = config_path(0);
        if (cfg_path && config_load(&cfg, cfg_path) != 0 && asked)
            fprintf(stderr, "cannot read %s\n", cfg_path);
    }

    /* argv-only: these are per-run, not preferences */
    int bench_frames = 0, a_frames = 0, list_params = 0;
    const char *snap = NULL, *snap3d = NULL;

#ifdef __EMSCRIPTEN__
    cfg.grid = emscripten_run_script_int("(function(){var v=new URLSearchParams(location.search).get('grid');return v?(v|0):0;})()");
    if (cfg.grid <= 0) cfg.grid = 256;
    if (cfg.grid > 512) cfg.grid = 512;      /* fixed 160 MB heap in the browser */
    cfg.winw = 1280; cfg.winh = 800;         /* the shell's CSS keeps this 16:10 shape */
#endif

    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "--config") && i + 1 < argc) i++;                 /* handled above */
        else if (!strcmp(o, "--no-config")) ;                            /* handled above */
        else if (!strcmp(o, "--help") || !strcmp(o, "-h")) { print_help(); return 0; }
        else if (!strcmp(o, "--write-config")) write_cfg = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : config_path(1);
        else if (!strcmp(o, "--list-params")) list_params = 1;
        else if (!strcmp(o, "--bench") && i + 1 < argc) bench_frames = atoi(argv[++i]);
        else if (!strcmp(o, "--snap") && i + 1 < argc) snap = argv[++i];
        else if (!strcmp(o, "--snap3d") && i + 1 < argc) snap3d = argv[++i];
        else if (!strcmp(o, "--frames") && i + 1 < argc) a_frames = atoi(argv[++i]);
        /* flags from before there was a table */
        else if (!strcmp(o, "--2d")) config_set(&cfg, "mode", "2d");
        else if (!strcmp(o, "--nomsaa")) config_set(&cfg, "msaa", "off");
        else if (!strcmp(o, "--cpu-caustics")) config_set(&cfg, "cpu-caustics", "on");
        else if (!strcmp(o, "--no-audio")) config_set(&cfg, "no-audio", "on");
        else if (!strcmp(o, "--hos")) config_set(&cfg, "nonlinear", "on");
        else if (!strcmp(o, "--overlay")) config_set(&cfg, "help", "on");
        else if (!strncmp(o, "--", 2)) {
            /* --name value for any setting; a boolean parameter needs no value,
             * and --no-name turns it off */
            const char *name = o + 2, *val = NULL;
            const param *p = param_find(name);
            if (!p && !strncmp(name, "no-", 3) && (p = param_find(name + 3)) && p->kind == PK_BOOL) { name += 3; val = "off"; }
            else if (p && p->kind == PK_BOOL && (i + 1 >= argc || !strncmp(argv[i + 1], "--", 2))) val = "on";
            else if (i + 1 < argc) val = argv[++i];
            if (!val || config_set(&cfg, name, val) != 0) {
                fprintf(stderr, "unknown option %s (--list-params shows every setting)\n", o);
                return 1;
            }
        }
        else { print_help(); return 1; }
    }
    if (cfg.grid < 16 || (cfg.grid & (cfg.grid - 1))) { fprintf(stderr, "grid must be a power of two >= 16\n"); return 1; }

    /* static: with Emscripten the main-loop call unwinds main's stack frame */
    static app a;
    memset(&a, 0, sizeof a);
    a.nx = a.ny = cfg.grid;
    a.running = 1;
    a.warp = 1.0;
    a.rain_rate = 2.0;
    a.paddle_div = 8.0;                    /* eight wavelengths across, whatever the basin */
    a.paddle_wall = 0; a.paddle_pos = 0.5; a.paddle_span = 1.0;
    a.paddle_gain = a.breeze_gain = a.finger_gain = 1.0;
    a.volume = 0.7; a.mute = 0;
    a.knob[SND_DROPS] = 1.0; a.knob[SND_BED] = 1.0; a.knob[SND_BROWN] = 0.0; a.knob[SND_BREEZE] = 1.0; a.knob[SND_HARSH] = 0.15;
    a.frames_left = a_frames;
    a.snap_path = snap3d;
    a.mode3d = cfg.mode3d;
    a.show_hud = 1;
    render_defaults(&a.rp);
    a.p3.gain = 1.0f;
    a.p3.cpu_caustics = cfg.cpu_caustics;
    { float sv[3] = { 0.30f, 0.90f, -0.25f }; float n = 1.0f / sqrtf(sv[0]*sv[0] + sv[1]*sv[1] + sv[2]*sv[2]);
      a.p3.sun[0] = sv[0] * n; a.p3.sun[1] = sv[1] * n; a.p3.sun[2] = sv[2] * n; }
    a.pix = malloc((size_t)cfg.grid * cfg.grid * sizeof(uint32_t));
    a.shape = cfg.shape;
    a.hos_nc = cfg.hos_nc; a.hos_order = cfg.hos_order;
    if (cfg.shape == WAVE_DISK && !cfg.mode3d) { fprintf(stderr, "the disk basin needs the 3-D view\n"); return 1; }
    a.w = app_make_wave(cfg.shape, cfg.grid, presets[cfg.preset].L, presets[cfg.preset].L, presets[cfg.preset].depth);
    if (!a.pix || !a.w) { fprintf(stderr, "out of memory\n"); return 1; }
    a.preset = cfg.preset;
    a.rp.floor_style = a.p3.floor_style = presets[cfg.preset].floor;
    if (cfg.Lx > 0 || cfg.depth > 0) {
        double Lx = cfg.Lx > 0 ? cfg.Lx : a.w->Lx, Ly = cfg.Ly > 0 ? cfg.Ly : a.w->Ly;
        double h = cfg.depth > 0 ? cfg.depth : a.w->depth;
        if (cfg.shape == WAVE_DISK) Ly = Lx;
        wave_set_pool(a.w, Lx, Ly, h);
    }
    app_reset_camera(&a);

    /* the headless uses: everything the file and the options asked for, without a window */
    if (list_params || write_cfg || bench_frames > 0) {
        config_apply(&cfg, &a);
        int rc = 0;
        if (list_params) param_list(&a, stdout);
        else if (write_cfg) {
            if (config_write(&cfg, &a, write_cfg) != 0) { fprintf(stderr, "cannot write %s\n", write_cfg); rc = 1; }
            else printf("wrote %s\n", write_cfg);
        } else {
            SDL_Init(0);
            rc = bench(&a, bench_frames, snap);
            SDL_Quit();
        }
        wave_destroy(a.w); free(a.pix);
        return rc;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (!cfg.no_audio) {
        a.au = audio_open();
        if (!a.au) fprintf(stderr, "no audio device; running silent\n");
    }
    Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (cfg.mode3d ? SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI : 0);
    if (cfg.mode3d) view3d_gl_attributes(cfg.msaa);
    a.win = SDL_CreateWindow("pond", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.winw, cfg.winh, wflags);
    if (!a.win && cfg.mode3d && cfg.msaa) {
        /* no multisampled visual on this display: try again without */
        fprintf(stderr, "no multisampled GL visual (%s); retrying without\n", SDL_GetError());
        SDL_GL_ResetAttributes();
        view3d_gl_attributes(0);
        a.win = SDL_CreateWindow("pond", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.winw, cfg.winh, wflags);
    }
    if (!a.win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    if (cfg.mode3d) {
        a.v3 = view3d_create(a.win, a.w, a.p3.cpu_caustics);
        if (!a.v3) { fprintf(stderr, "3-D view unavailable; try --2d\n"); return 1; }
        app_reset_camera(&a);
    } else {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        a.ren = SDL_CreateRenderer(a.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!a.ren) a.ren = SDL_CreateRenderer(a.win, -1, 0);
        if (!a.ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
        a.tex = SDL_CreateTexture(a.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, cfg.grid, cfg.grid);
        if (!a.tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return 1; }
        print_help();
    }

    /* now everything exists: the sound levels reach the device, the camera the view,
     * and the config file and command line get their say */
    param_set(&a, "volume", a.volume);
    param_set(&a, "mute", a.mute);
    for (int k = 0; k < SND_NUM; k++) { char nm[32]; snprintf(nm, sizeof nm, "sound.%s", snd_knob_names[k]); param_set(&a, nm, a.knob[k]); }
    config_apply(&cfg, &a);

    a.hud_dirty = 1;
    a.prev = SDL_GetPerformanceCounter();

    /* something to look at right away */
    splash(&a, 0.5 * a.w->Lx, 0.5 * a.w->Ly, 0.03 * a.w->Lx, -0.15 * 0.03 * a.w->Lx);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(frame, &a, 0, 1);
#else
    while (a.running) frame(&a);
    if (a.au) audio_close(a.au);
    if (a.hs) hos_destroy(a.hs);
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
