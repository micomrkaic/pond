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
#include "param.h"
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
    c->mode3d = 1;
    c->msaa = 1;
    c->cpu_caustics = 0;
    c->no_audio = 0;
    c->preset = 1;                 /* pond */
    c->shape = WAVE_RECT;
    c->Lx = c->Ly = c->depth = 0;  /* the preset's */
    c->hos_nc = 64; c->hos_order = 3;
    c->nlate = 0;
}

/* -------------------------------------------------------------- small helpers */

/* compare ignoring case, with '_' and '-' the same character */
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

/* "1280x800", or "1280" for 16:10 from the width */
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

static int queue(pond_config *c, const char *k, const char *v)
{
    if (c->nlate >= CONFIG_MAX_LATE) { fprintf(stderr, "too many settings; '%s' dropped\n", k); return -1; }
    snprintf(c->late[c->nlate].name, sizeof c->late[0].name, "%s", k);
    snprintf(c->late[c->nlate].val, sizeof c->late[0].val, "%s", v);
    c->nlate++;
    return 0;
}

/* --------------------------------------------------------------- one setting */

int config_set(pond_config *c, const char *k, const char *v)
{
    if (!c || !k || !v) return -1;

    /* what has to be known before the program exists */
    if (keyeq(k, "grid"))         { c->grid = atoi(v); return 0; }
    if (keyeq(k, "window"))       { as_size(v, &c->winw, &c->winh); return 0; }
    if (keyeq(k, "mode"))         { c->mode3d = !keyeq(v, "2d"); return 0; }
    if (keyeq(k, "msaa"))         { c->msaa = as_bool(v, c->msaa); return 0; }
    if (keyeq(k, "cpu-caustics")) { c->cpu_caustics = as_bool(v, c->cpu_caustics); return 0; }
    if (keyeq(k, "no-audio") || keyeq(k, "silent")) { c->no_audio = as_bool(v, c->no_audio); return 0; }
    if (keyeq(k, "hos-nc"))       { int n = atoi(v); if (n > 0) c->hos_nc = n; return 0; }
    if (keyeq(k, "hos-order"))    { int n = atoi(v); if (n == 2 || n == 3) c->hos_order = n; return 0; }
    /* the basin it starts with: creation-time too, so the wave is built once */
    if (keyeq(k, "preset"))       { int p = atoi(v) - 1; if (p >= 0 && p < 4) c->preset = p; return 0; }
    if (keyeq(k, "shape"))        { c->shape = keyeq(v, "disk") ? WAVE_DISK : WAVE_RECT; return 0; }
    if (keyeq(k, "basin"))        { if (*v) as_pair(v, &c->Lx, &c->Ly); return 0; }
    if (keyeq(k, "width"))        { double x = atof(v); if (x > 0) { c->Lx = x; if (c->Ly <= 0) c->Ly = x; } return 0; }
    if (keyeq(k, "length"))       { double x = atof(v); if (x > 0) { c->Ly = x; if (c->Lx <= 0) c->Lx = x; } return 0; }
    if (keyeq(k, "depth"))        { double d = atof(v); if (d > 0) c->depth = d; return 0; }

    /* spellings from before there was a table */
    if (keyeq(k, "scene")) {
        queue(c, "rain", strstr(v, "rain") ? "on" : "off");
        queue(c, "paddle", strstr(v, "paddle") ? "on" : "off");
        queue(c, "breeze", strstr(v, "breeze") ? "on" : "off");
        return 0;
    }
    if (keyeq(k, "camera") || keyeq(k, "cam")) {
        double y, p, d = 0;
        const int n = sscanf(v, "%lf,%lf,%lf", &y, &p, &d);
        if (n >= 2) {
            char b[32];
            snprintf(b, sizeof b, "%g", y); queue(c, "yaw", b);
            snprintf(b, sizeof b, "%g", p); queue(c, "pitch", b);
            if (n == 3) { snprintf(b, sizeof b, "%g", d); queue(c, "dist", b); }
        }
        return 0;
    }
    if (keyeq(k, "hos")) return queue(c, "nonlinear", v);
    if (keyeq(k, "sound")) {                     /* k=v,k=v */
        char tmp[256]; snprintf(tmp, sizeof tmp, "%s", v);
        for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            char key[64]; snprintf(key, sizeof key, "sound.%s", tok);
            if (!param_find(key)) { fprintf(stderr, "unknown sound knob '%s'\n", tok); continue; }
            queue(c, key, eq + 1);
        }
        return 0;
    }
    if (keyeq(k, "snd.drops") || keyeq(k, "snd.bed") || keyeq(k, "snd.brown") || keyeq(k, "snd.breeze") || keyeq(k, "snd.harsh")) {
        char key[64]; snprintf(key, sizeof key, "sound.%s", strchr(k, '.') + 1);
        return queue(c, key, v);
    }

    /* everything the running program can change: queued for the parameter table */
    if (param_find(k)) return queue(c, k, v);
    return -1;
}

int config_apply(const pond_config *c, struct app *a)
{
    int refused = 0;
    for (int i = 0; i < c->nlate; i++) {
        const int rc = param_set_str(a, c->late[i].name, c->late[i].val);
        if (rc != 0) { fprintf(stderr, "cannot set %s = %s\n", c->late[i].name, c->late[i].val); refused++; }
    }
    return refused;
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
    if (comment && *comment) fprintf(f, "%-14s = %-12s # %s\n", key, val, comment);
    else                     fprintf(f, "%-14s = %s\n", key, val);
}

static const char *onoff(int b) { return b ? "on" : "off"; }

int config_write(const pond_config *c, const struct app *a, const char *path)
{
    if (!c || !a || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    char v[80];

    fprintf(f,
        "# pond configuration.  Written by --write-config; edit freely.\n"
        "# Comments run from # or ; to the end of the line.  Settings are read\n"
        "# here first and then overridden by the command line; --no-config\n"
        "# ignores this file altogether.  Every name below is also a --name\n"
        "# option, and --list-params shows them all with their ranges.\n");

    fprintf(f, "\n# ---- before the program starts ----\n");
    snprintf(v, sizeof v, "%dx%d", c->winw, c->winh);      put(f, "window", v, "pixels, or one number for 16:10");
    snprintf(v, sizeof v, "%d", c->grid);                  put(f, "grid", v, "modes per axis, a power of two");
    put(f, "mode", c->mode3d ? "3d" : "2d", "3d, or 2d for the CPU renderer");
    put(f, "msaa", onoff(c->msaa), "ask for a multisampled framebuffer");
    put(f, "cpu-caustics", onoff(c->cpu_caustics), "force the CPU caustic splat");
    put(f, "no-audio", onoff(c->no_audio), "do not open a sound device at all");
    snprintf(v, sizeof v, "%d", c->hos_nc);                put(f, "hos-nc", v, "HOS: modes per axis that take part");
    snprintf(v, sizeof v, "%d", c->hos_order);             put(f, "hos-order", v, "HOS: 2 or 3");

    const char *group = "";
    for (int i = 0; i < param_count(); i++) {
        const param *p = param_at(i);
        if (strcmp(p->group, group) != 0) {
            group = p->group;
            fprintf(f, "\n# ---- %s ----\n", group);
            if (!strcmp(group, "basin"))
                fprintf(f, "# preset is applied first; a width, length, depth or floor given here overrides\n"
                           "# it (commented lines are the preset's own values, for reference)\n");
            if (!strcmp(group, "sound"))
                fprintf(f, "# the five sound.* levels multiply the designed ones: 1 = as designed, 0 = off\n");
        }
        param_get_str(a, p->name, v, sizeof v);
        if (param_is_derived(a, p->name)) {
            /* what the preset gives: left commented so the file keeps following the preset */
            char k2[40];
            snprintf(k2, sizeof k2, "# %s", p->name);
            put(f, k2, v, p->help);
        } else put(f, p->name, v, p->help);
    }
    fclose(f);
    return 0;
}
