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

#define _POSIX_C_SOURCE 200809L     /* opendir, stat */
#include "attract.h"
#include "param.h"
#include "script.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

struct attract {
    char  **files; int n, cap;
    int     cur;              /* the show playing, an index into files */
    int     played;
    double  t;                /* seconds into the current show */
    double  last;             /* its script clock last frame: a drop means it looped */
    double *saved;            /* every parameter, as the attract mode found it */
    unsigned seed;
};

static int push_file(attract *at, const char *path)
{
    if (at->n == at->cap) {
        int nc = at->cap ? 2 * at->cap : 16;
        char **nf = realloc(at->files, (size_t)nc * sizeof *nf);
        if (!nf) return -1;
        at->files = nf; at->cap = nc;
    }
    at->files[at->n] = malloc(strlen(path) + 1);
    if (!at->files[at->n]) return -1;
    strcpy(at->files[at->n++], path);
    return 0;
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

static int by_name(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

/* the .pond files in a directory, sorted; only those starting with prefix (if any) */
static int add_dir(attract *at, const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    const int first = at->n;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !ends_with(e->d_name, ".pond")) continue;
        if (prefix && strncmp(e->d_name, prefix, strlen(prefix))) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s%s%s", dir, ends_with(dir, "/") ? "" : "/", e->d_name);
        if (push_file(at, path) != 0) break;
    }
    closedir(d);
    if (at->n > first) qsort(at->files + first, (size_t)(at->n - first), sizeof *at->files, by_name);
    return at->n - first;
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void shuffle(attract *at, const char *was)
{
    /* Fisher-Yates; the show just played (was) is not first again */
    if (at->n < 2) was = NULL;
    for (int i = at->n - 1; i > 0; i--) {
        at->seed = at->seed * 1664525u + 1013904223u;
        const int j = (int)((at->seed >> 8) % (unsigned)(i + 1));
        char *t = at->files[i]; at->files[i] = at->files[j]; at->files[j] = t;
    }
    if (was && at->files[0] == was) { char *t = at->files[0]; at->files[0] = at->files[at->n - 1]; at->files[at->n - 1] = t; }
}

attract *attract_new(const char *const *paths, int npaths, const char *base, char *err, size_t errn)
{
    if (err && errn) err[0] = 0;
    attract *at = calloc(1, sizeof *at);
    if (!at) return NULL;
    at->seed = (unsigned)time(NULL) * 2654435761u + 12345u;
    for (int i = 0; i < npaths; i++) {
        if (is_dir(paths[i])) add_dir(at, paths[i], NULL);
        else {
            FILE *f = fopen(paths[i], "rb");
            if (f) { fclose(f); push_file(at, paths[i]); }
            else if (err && errn) snprintf(err, errn, "attract: cannot read %s", paths[i]);
        }
    }
    if (npaths == 0) {
        /* your recordings, here; or else the demos, wherever they were installed */
        if (add_dir(at, ".", "pond-rec-") == 0) {
            char dir[1024];
            int got = 0;
            if (base) {
                snprintf(dir, sizeof dir, "%sdemos", base);
                got = add_dir(at, dir, NULL);
                if (!got) { snprintf(dir, sizeof dir, "%s../share/pond/demos", base); got = add_dir(at, dir, NULL); }
            }
            if (!got) add_dir(at, "demos", NULL);
        }
    }
    if (at->n == 0) {
        if (err && errn && !err[0]) snprintf(err, errn, "attract: no shows (no pond-rec-*.pond here, and no demos found)");
        attract_free(at, NULL);
        return NULL;
    }
    shuffle(at, NULL);
    return at;
}

int         attract_count(const attract *at) { return at ? at->n : 0; }
const char *attract_file(const attract *at, int i) { return at && i >= 0 && i < at->n ? at->files[i] : NULL; }
const char *attract_current(const attract *at) { return at ? at->files[at->cur] : NULL; }
int         attract_shows_played(const attract *at) { return at ? at->played : 0; }

/* the settings as they were: preset first (the table's order), so a size or a depth
 * set after it stands; the window's own (full screen, help, HUD) are left alone */
static void restore(attract *at, app *a)
{
    for (int i = 0; i < param_count(); i++) {
        const param *p = param_at(i);
        if (!strcmp(p->name, "fullscreen") || !strcmp(p->name, "help") || !strcmp(p->name, "hud")) continue;
        if (p->get(a) != at->saved[i]) param_set(a, p->name, at->saved[i]);
    }
    app_clear_floats(a);
    a->caption[0] = 0;
    wave_clear(a->w);
}

/* on to the next show; at the end of the list, a new order */
static void advance_show(attract *at)
{
    if (at->cur + 1 < at->n) { at->cur++; return; }
    shuffle(at, at->files[at->cur]);
    at->cur = 0;
}

/* the next show that loads; 0 if none will */
static int start_show(attract *at, app *a)
{
    for (int tries = 0; tries < at->n; tries++) {
        if (a->sc) { script_free(a->sc); a->sc = NULL; }
        restore(at, a);
        char err[256];
        script *s = script_load(at->files[at->cur], err, sizeof err);
        if (s) {
            a->sc = s;
            script_start(s, a);
            at->t = 0; at->last = 0;
            return 1;
        }
        fprintf(stderr, "%s (skipped)\n", err);
        advance_show(at);
    }
    return 0;
}

void attract_begin(attract *at, app *a)
{
    at->saved = malloc(sizeof(double) * (size_t)param_count());
    for (int i = 0; at->saved && i < param_count(); i++) at->saved[i] = param_at(i)->get(a);
    at->cur = 0; at->played = 0;
    if (!start_show(at, a)) fprintf(stderr, "attract: none of the shows would load\n");
}

void attract_update(attract *at, app *a, double dt)
{
    if (!a->sc) return;
    script_update(a->sc, a, dt);
    at->t += dt;
    const double now = script_time(a->sc);
    const int looped = now < at->last - 1e-9;
    at->last = now;
    if (looped || !script_running(a->sc) || at->t > ATTRACT_MAX) {
        at->played++;
        advance_show(at);
        start_show(at, a);
    }
}

float attract_fade(const attract *at)
{
    if (!at || at->t >= ATTRACT_FADE) return 0.0f;
    const double u = at->t / ATTRACT_FADE;
    return (float)(1.0 - u * u * (3.0 - 2.0 * u));
}

void attract_free(attract *at, app *a)
{
    if (!at) return;
    if (a && a->sc) { script_free(a->sc); a->sc = NULL; }
    for (int i = 0; i < at->n; i++) free(at->files[i]);
    free(at->files); free(at->saved); free(at);
}
