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

/* -std=c17 sets __STRICT_ANSI__, which hides mkdir() in glibc's headers */
#ifndef __EMSCRIPTEN__
#define _POSIX_C_SOURCE 200809L
#endif

#include "config.h"
#include "wave.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(pond_config *c)
{
    memset(c, 0, sizeof *c);
    c->grid = 512;
    c->winw = 1280; c->winh = 800;
    c->fullscreen = 0;
    c->mode3d = 1;
    c->glass = 0;
    c->floor_style = -1;
    c->msaa = 1;
    c->cpu_caustics = 0;
    c->show_hud = 1;
    c->show_help = 0;
    c->cam_set = 0; c->cam_yaw = 35.0f; c->cam_pitch = 42.0f; c->cam_dist = 1.5f;

    c->preset = 1;                 /* pond */
    c->shape = WAVE_RECT;
    c->Lx = c->Ly = c->depth = 0;  /* the preset's */
    c->hos_on = 0; c->hos_nc = 64; c->hos_order = 3;

    c->rain = c->breeze = c->paddle = 0;
    c->rain_rate = 2.0;
    c->warp = 1.0;
    c->paddle_freq = 0.0;      /* auto */
    c->paddle_pos = 0.5;
    c->paddle_span = 1.0;
    c->paddle_stroke = 1.0;
    c->paddle_wall = 0;

    c->no_audio = 0;
    c->mute = 0;
    c->volume = 0.7;
    c->knob[SND_DROPS] = 1.0;
    c->knob[SND_BED] = 1.0;
    c->knob[SND_BROWN] = 0.0;
    c->knob[SND_BREEZE] = 1.0;
    c->knob[SND_HARSH] = 0.15;
}

/* -------------------------------------------------------------- small helpers */

/* -std=c17 hides the POSIX strcasecmp, so: compare ignoring case, with '_'
 * and '-' the same character, so depth_arg and depth-arg both work. */
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

static int as_bool(const char *v, int dflt)
{
    if (!v || !*v) return dflt;
    if (keyeq(v, "on") || keyeq(v, "yes") || keyeq(v, "true") || keyeq(v, "1")) return 1;
    if (keyeq(v, "off") || keyeq(v, "no") || keyeq(v, "false") || keyeq(v, "0")) return 0;
    return dflt;
}

/* "1280x800", "1280" (16:10 from the width), "800p" is not a thing here */
static void as_size(const char *v, int *w, int *h)
{
    const char *x = strchr(v, 'x');
    if (!x) x = strchr(v, 'X');
    int a = atoi(v);
    if (a <= 0) return;
    *w = a;
    *h = x ? atoi(x + 1) : a * 10 / 16;
    if (*h <= 0) *h = a * 10 / 16;
}

static void as_pair(const char *v, double *a, double *b)
{
    const char *x = strchr(v, 'x');
    if (!x) x = strchr(v, 'X');
    double p = atof(v);
    if (p <= 0) return;
    *a = p;
    *b = x ? atof(x + 1) : p;
    if (*b <= 0) *b = p;
}

/* --------------------------------------------------------------- one setting */

int config_set(pond_config *c, const char *k, const char *v)
{
    if (!c || !k || !v) return -1;

    /* sound.<knob> — the prefix keeps "breeze" the wind and "sound.breeze" its level */
    if (!strncmp(k, "sound.", 6) || !strncmp(k, "snd.", 4)) {
        const char *name = strchr(k, '.') + 1;
        for (int i = 0; i < SND_NUM; i++)
            if (keyeq(name, snd_knob_names[i])) {
                double x = atof(v);
                c->knob[i] = x < 0 ? 0 : (x > 4 ? 4 : x);
                return 0;
            }
        return -1;
    }

    /* window and view */
    if (keyeq(k, "grid"))         { c->grid = atoi(v); return 0; }
    if (keyeq(k, "window"))       { as_size(v, &c->winw, &c->winh); return 0; }
    if (keyeq(k, "fullscreen"))   { c->fullscreen = as_bool(v, c->fullscreen); return 0; }
    if (keyeq(k, "mode"))         { c->mode3d = !keyeq(v, "2d"); return 0; }
    if (keyeq(k, "glass"))        { int g = atoi(v); c->glass = (g < 0 || g > 4) ? 0 : g; return 0; }
    if (keyeq(k, "floor")) {
        if (keyeq(v, "auto") || keyeq(v, "preset")) c->floor_style = -1;
        else { int f = atoi(v); c->floor_style = (f < 0 || f > 2) ? -1 : f; }
        return 0;
    }
    if (keyeq(k, "msaa"))         { c->msaa = as_bool(v, c->msaa); return 0; }
    if (keyeq(k, "cpu-caustics")) { c->cpu_caustics = as_bool(v, c->cpu_caustics); return 0; }
    if (keyeq(k, "hud"))          { c->show_hud = as_bool(v, c->show_hud); return 0; }
    if (keyeq(k, "help"))         { c->show_help = as_bool(v, c->show_help); return 0; }
    if (keyeq(k, "camera")) {
        float y = c->cam_yaw, p = c->cam_pitch, d = c->cam_dist;
        if (sscanf(v, "%f,%f,%f", &y, &p, &d) >= 2) {
            c->cam_yaw = y; c->cam_pitch = p; c->cam_dist = d > 0 ? d : 1.5f; c->cam_set = 1;
        }
        return 0;
    }

    /* basin */
    if (keyeq(k, "preset"))    { int p = atoi(v) - 1; if (p >= 0 && p < 4) c->preset = p; return 0; }
    if (keyeq(k, "shape"))     { c->shape = keyeq(v, "disk") ? WAVE_DISK : WAVE_RECT; return 0; }
    if (keyeq(k, "basin"))     { if (*v) as_pair(v, &c->Lx, &c->Ly); return 0; }
    if (keyeq(k, "depth"))     { double d = atof(v); if (d > 0) c->depth = d; return 0; }
    if (keyeq(k, "nonlinear") || keyeq(k, "hos")) { c->hos_on = as_bool(v, c->hos_on); return 0; }
    if (keyeq(k, "hos-nc"))    { int n = atoi(v); if (n > 0) c->hos_nc = n; return 0; }
    if (keyeq(k, "hos-order")) { int n = atoi(v); if (n == 2 || n == 3) c->hos_order = n; return 0; }

    /* sources */
    if (keyeq(k, "scene")) {
        c->rain = strstr(v, "rain") != NULL;
        c->paddle = strstr(v, "paddle") != NULL;
        c->breeze = strstr(v, "breeze") != NULL;
        return 0;
    }
    if (keyeq(k, "rain"))      { c->rain = as_bool(v, c->rain); return 0; }
    if (keyeq(k, "breeze"))    { c->breeze = as_bool(v, c->breeze); return 0; }
    if (keyeq(k, "paddle"))    { c->paddle = as_bool(v, c->paddle); return 0; }
    if (keyeq(k, "rain-rate")) { double r = atof(v); if (r > 0) c->rain_rate = r; return 0; }
    if (keyeq(k, "paddle-freq")) { double x = atof(v); c->paddle_freq = x > 0 ? x : 0; return 0; }
    if (keyeq(k, "paddle-pos"))  { double x = atof(v); c->paddle_pos = x < 0 ? 0 : (x > 1 ? 1 : x); return 0; }
    if (keyeq(k, "paddle-span")) { double x = atof(v); c->paddle_span = x < 0.02 ? 0.02 : (x > 1 ? 1 : x); return 0; }
    if (keyeq(k, "paddle-stroke")) { double x = atof(v); if (x >= 0) c->paddle_stroke = x; return 0; }
    if (keyeq(k, "paddle-wall")) {
        if (keyeq(v, "x=0") || keyeq(v, "x0")) c->paddle_wall = 0;
        else if (keyeq(v, "x=Lx") || keyeq(v, "xL")) c->paddle_wall = 1;
        else if (keyeq(v, "y=0") || keyeq(v, "y0")) c->paddle_wall = 2;
        else if (keyeq(v, "y=Ly") || keyeq(v, "yL")) c->paddle_wall = 3;
        else { int n = atoi(v); c->paddle_wall = (n < 0 || n > 3) ? 0 : n; }
        return 0;
    }
    if (keyeq(k, "warp"))      { double w = atof(v); if (w > 0) c->warp = w; return 0; }

    /* sound */
    if (keyeq(k, "no-audio") || keyeq(k, "silent")) { c->no_audio = as_bool(v, c->no_audio); return 0; }
    if (keyeq(k, "mute"))      { c->mute = as_bool(v, c->mute); return 0; }
    if (keyeq(k, "volume"))    { double x = atof(v); c->volume = x < 0 ? 0 : (x > 1 ? 1 : x); return 0; }

    return -1;
}

/* ------------------------------------------------------------------ the file */

#ifdef __EMSCRIPTEN__
const char *config_path(int for_writing) { (void)for_writing; return NULL; }
#else
static int exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

const char *config_path(int for_writing)
{
    static char buf[512];
    const char *env = getenv("POND_CONFIG");
    if (env && *env) { snprintf(buf, sizeof buf, "%s", env); return buf; }

    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (!for_writing) {
        if (xdg && *xdg) { snprintf(buf, sizeof buf, "%s/pond/pond.conf", xdg); if (exists(buf)) return buf; }
        if (home && *home) {
            snprintf(buf, sizeof buf, "%s/.config/pond/pond.conf", home); if (exists(buf)) return buf;
            snprintf(buf, sizeof buf, "%s/.pondrc", home); if (exists(buf)) return buf;
        }
        snprintf(buf, sizeof buf, "pond.conf");
        return exists(buf) ? buf : NULL;
    }
    /* writing: $XDG_CONFIG_HOME/pond/ or ~/.config/pond/, created if need be */
    {
        char dir[400];
        if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/pond", xdg);
        else if (home && *home) {
            snprintf(dir, sizeof dir, "%s/.config", home);
            mkdir(dir, 0755);
            snprintf(dir, sizeof dir, "%s/.config/pond", home);
        } else { snprintf(buf, sizeof buf, "pond.conf"); return buf; }
        mkdir(dir, 0755);
        snprintf(buf, sizeof buf, "%s/pond.conf", dir);
        return buf;
    }
}
#endif

int config_load(pond_config *c, const char *path)
{
    if (!c || !path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        char *hash = strpbrk(s, "#;");
        if (hash) *hash = 0;
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) continue;

        /* key = value, key: value, or key value */
        char *sep = strpbrk(s, "=:");
        char *key = s, *val;
        if (sep) { *sep = 0; val = sep + 1; }
        else {
            char *sp = s;
            while (*sp && !isspace((unsigned char)*sp)) sp++;
            if (!*sp) { fprintf(stderr, "%s:%d: no value for '%s'\n", path, lineno, s); continue; }
            *sp = 0; val = sp + 1;
        }
        for (char *e = key + strlen(key); e > key && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        while (*val && isspace((unsigned char)*val)) val++;
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        if (*val == '"' || *val == '\'') {
            char q = *val++;
            char *e = strchr(val, q);
            if (e) *e = 0;
        }
        if (config_set(c, key, val) != 0)
            fprintf(stderr, "%s:%d: unknown setting '%s'\n", path, lineno, key);
    }
    fclose(f);
    return 0;
}

static void put(FILE *f, const char *key, const char *val, const char *comment)
{
    if (comment && *comment) fprintf(f, "%-13s = %-11s # %s\n", key, val, comment);
    else                     fprintf(f, "%-13s = %s\n", key, val);
}

static const char *onoff(int b) { return b ? "on" : "off"; }

int config_write(const pond_config *c, const char *path)
{
    if (!c || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    char v[80];

    fprintf(f,
        "# pond configuration.  Written by --write-config; edit freely.\n"
        "# Comments run from # or ; to the end of the line.  Settings are read\n"
        "# here first and then overridden by the command line; --no-config\n"
        "# ignores this file altogether.\n");

    fprintf(f, "\n# ---- window and view ----\n");
    snprintf(v, sizeof v, "%dx%d", c->winw, c->winh);      put(f, "window", v, "pixels, or one number for 16:10");
    put(f, "fullscreen", onoff(c->fullscreen), "F11 or alt+enter toggles it");
    snprintf(v, sizeof v, "%d", c->grid);                  put(f, "grid", v, "modes per axis, a power of two");
    put(f, "mode", c->mode3d ? "3d" : "2d", "3d, or 2d for the CPU renderer");
    snprintf(v, sizeof v, "%d", c->glass);                 put(f, "glass", v, "0 opaque, 1 floor only, 2 glass,");
    fprintf(f, "%-13s   %-11s # %s\n", "", "", "3 glass+bottom, 4 no container");
    if (c->floor_style < 0) snprintf(v, sizeof v, "auto"); else snprintf(v, sizeof v, "%d", c->floor_style);
    put(f, "floor", v, "auto, or 0 tiles, 1 checker, 2 sand");
    put(f, "msaa", onoff(c->msaa), "ask for a multisampled framebuffer");
    put(f, "cpu-caustics", onoff(c->cpu_caustics), "force the CPU caustic splat");
    put(f, "hud", onoff(c->show_hud), "the settings box, top left (key: d)");
    put(f, "help", onoff(c->show_help), "start with the help panel up (h/F1)");
    snprintf(v, sizeof v, "%g,%g,%g", (double)c->cam_yaw, (double)c->cam_pitch, (double)c->cam_dist);
    put(f, "camera", v, "yaw, pitch (deg), distance in lengths");

    fprintf(f, "\n# ---- basin ----\n");
    snprintf(v, sizeof v, "%d", c->preset + 1);
    put(f, "preset", v, "1 tray 30 cm, 2 pond 3 m,");
    fprintf(f, "%-13s   %-11s # %s\n", "", "", "3 pool 12 m, 4 sea 80 m");
    put(f, "shape", c->shape == WAVE_DISK ? "disk" : "rect", "rect or disk");
    if (c->Lx > 0 && c->Ly > 0) snprintf(v, sizeof v, "%gx%g", c->Lx, c->Ly); else v[0] = 0;
    put(f, "basin", v, "width x length in metres; empty = preset");
    snprintf(v, sizeof v, "%g", c->depth);                 put(f, "depth", v, "metres; 0 = the preset's");
    put(f, "nonlinear", onoff(c->hos_on), "the HOS correction (rectangle only)");
    snprintf(v, sizeof v, "%d", c->hos_nc);                put(f, "hos-nc", v, "modes per axis that take part");
    snprintf(v, sizeof v, "%d", c->hos_order);             put(f, "hos-order", v, "2 or 3");

    fprintf(f, "\n# ---- sources at start ----\n");
    put(f, "rain", onoff(c->rain), NULL);
    put(f, "breeze", onoff(c->breeze), NULL);
    put(f, "paddle", onoff(c->paddle), NULL);
    snprintf(v, sizeof v, "%g", c->rain_rate);             put(f, "rain-rate", v, "drops per simulated second");
    snprintf(v, sizeof v, "%g", c->warp);                  put(f, "warp", v, "simulated seconds per real second");
    fprintf(f, "\n# ---- wavemaker ----\n");
    if (c->paddle_freq > 0) snprintf(v, sizeof v, "%g", c->paddle_freq); else snprintf(v, sizeof v, "auto");
    put(f, "paddle-freq", v, "Hz; auto = 8 wavelengths across");
    {
        static const char *const wn[] = { "x=0", "x=Lx", "y=0", "y=Ly" };
        put(f, "paddle-wall", wn[c->paddle_wall], "x=0, x=Lx, y=0, y=Ly (disk: the rim)");
    }
    snprintf(v, sizeof v, "%g", c->paddle_pos);            put(f, "paddle-pos", v, "0..1 along that wall");
    snprintf(v, sizeof v, "%g", c->paddle_span);           put(f, "paddle-span", v, "0..1 of it; 1 = the whole wall");
    snprintf(v, sizeof v, "%g", c->paddle_stroke);         put(f, "paddle-stroke", v, "multiplier on the stroke");

    fprintf(f, "\n# ---- sound ----\n");
    put(f, "no-audio", onoff(c->no_audio), "do not open a device at all");
    put(f, "mute", onoff(c->mute), "device open but silent; m unmutes");
    snprintf(v, sizeof v, "%g", c->volume);                put(f, "volume", v, "0..1");
    fprintf(f, "# multipliers on the designed levels: 1 = as designed, 0 = off\n");
    for (int i = 0; i < SND_NUM; i++) {
        char key[32];
        snprintf(key, sizeof key, "sound.%s", snd_knob_names[i]);
        snprintf(v, sizeof v, "%g", c->knob[i]);
        put(f, key, v, NULL);
    }
    fclose(f);
    return 0;
}
