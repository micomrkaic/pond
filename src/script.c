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

#include "script.h"
#include "param.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { A_SET, A_DROP, A_CLEAR, A_CAMERA, A_SAY, A_LOOP, A_END } action_kind;

typedef struct {
    action_kind kind;
    char   name[32];        /* A_SET: the parameter */
    char   sval[160];       /* A_SET: the value as written; A_SAY: the text */
    int    rel;             /* A_SET: +1 for +=, -1 for -=, 0 absolute */
    double over;            /* seconds to tween over; 0 = at once */
    double x, y, size;      /* A_DROP (fractions of the basin; size relative); A_CAMERA: yaw, pitch */
    double z;               /* A_CAMERA: dist, or < 0 to keep */
    int    random;          /* A_DROP: anywhere */
} action;

typedef struct {
    double t;
    int    first, n;        /* its actions, in the script's action array */
    int    line;
} event;

typedef struct {
    char   name[32];
    double v0, v1, t0, t1;
} tween;

#define MAX_TWEENS 32

struct script {
    char    name[128];
    action *acts; int nacts, cap_acts;
    event  *evs;  int nevs, cap_evs;
    /* running */
    int     running;
    double  t;
    int     next;           /* the next event to fire */
    tween   tw[MAX_TWEENS]; int ntw;
};

/* ------------------------------------------------------------------ parsing */

static int push_action(script *s, const action *a)
{
    if (s->nacts == s->cap_acts) {
        int nc = s->cap_acts ? 2 * s->cap_acts : 32;
        action *na = realloc(s->acts, (size_t)nc * sizeof *na);
        if (!na) return -1;
        s->acts = na; s->cap_acts = nc;
    }
    s->acts[s->nacts++] = *a;
    return 0;
}

static int push_event(script *s, const event *e)
{
    if (s->nevs == s->cap_evs) {
        int nc = s->cap_evs ? 2 * s->cap_evs : 16;
        event *ne = realloc(s->evs, (size_t)nc * sizeof *ne);
        if (!ne) return -1;
        s->evs = ne; s->cap_evs = nc;
    }
    s->evs[s->nevs++] = *e;
    return 0;
}

/* tokens: whitespace separated, ; is whitespace, "..." is one token (quotes stripped) */
static int tokenize(char *line, char **tok, int maxtok)
{
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (!*p) break;
        if (n == maxtok) return -1;
        if (*p == '"') {
            tok[n++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            tok[n++] = p;
            while (*p && !isspace((unsigned char)*p) && *p != ';') p++;
            if (*p) *p++ = 0;
        }
    }
    return n;
}

static int is_number(const char *t)
{
    char *e;
    strtod(t, &e);
    return e != t && *e == 0;
}

static void fail(char *err, size_t errn, const char *name, int line, const char *msg, const char *what)
{
    if (!err || !errn) return;
    if (what) snprintf(err, errn, "%s:%d: %s '%s'", name, line, msg, what);
    else      snprintf(err, errn, "%s:%d: %s", name, line, msg);
}

script *script_parse(const char *text, const char *name, char *err, size_t errn)
{
    if (err && errn) err[0] = 0;
    script *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->name, sizeof s->name, "%s", name ? name : "script");

    double tprev = 0.0;
    int lineno = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p += len + (nl ? 1 : 0);
        lineno++;

        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *tok[64];
        int n = tokenize(line, tok, 64);
        if (n < 0) { fail(err, errn, s->name, lineno, "too many words on one line", NULL); script_free(s); return NULL; }
        if (n == 0) continue;

        /* the time: "at T", "+T", or nothing (the previous line's time) */
        event ev = { tprev, s->nacts, 0, lineno };
        int i = 0;
        if (!strcmp(tok[0], "at")) {
            if (n < 2 || !is_number(tok[1])) { fail(err, errn, s->name, lineno, "'at' needs a time in seconds", NULL); script_free(s); return NULL; }
            ev.t = atof(tok[1]); i = 2;
        } else if (tok[0][0] == '+' && is_number(tok[0] + 1)) {
            ev.t = tprev + atof(tok[0] + 1); i = 1;
        }
        if (ev.t < tprev - 1e-9) { fail(err, errn, s->name, lineno, "time runs backwards", NULL); script_free(s); return NULL; }
        tprev = ev.t;

        /* the actions */
        while (i < n) {
            action a; memset(&a, 0, sizeof a);
            const char *w = tok[i++];
            if (!strcmp(w, "loop")) a.kind = A_LOOP;
            else if (!strcmp(w, "end") || !strcmp(w, "stop")) a.kind = A_END;
            else if (!strcmp(w, "clear")) a.kind = A_CLEAR;
            else if (!strcmp(w, "say") || !strcmp(w, "caption")) {
                a.kind = A_SAY;
                if (i < n) snprintf(a.sval, sizeof a.sval, "%s", tok[i++]);
            }
            else if (!strcmp(w, "drop")) {
                a.kind = A_DROP; a.size = 0.03; a.random = 1;
                if (i < n && !strcmp(tok[i], "random")) i++;
                else if (i < n && sscanf(tok[i], "%lf,%lf", &a.x, &a.y) == 2) { a.random = 0; i++; }
                if (i < n && is_number(tok[i])) a.size = atof(tok[i++]);
            }
            else if (!strcmp(w, "camera")) {
                a.kind = A_CAMERA; a.z = -1;
                if (i >= n || sscanf(tok[i], "%lf,%lf,%lf", &a.x, &a.y, &a.z) < 2) {
                    fail(err, errn, s->name, lineno, "camera needs yaw,pitch[,dist]", NULL); script_free(s); return NULL;
                }
                i++;
            }
            else if (param_find(w)) {
                a.kind = A_SET;
                snprintf(a.name, sizeof a.name, "%s", param_find(w)->name);
                if (i >= n) { fail(err, errn, s->name, lineno, "no value for", w); script_free(s); return NULL; }
                const char *v = tok[i++];
                /* += 30, +=30, -= 5 */
                if (!strcmp(v, "+=") || !strcmp(v, "-=")) {
                    a.rel = v[0] == '+' ? 1 : -1;
                    if (i >= n) { fail(err, errn, s->name, lineno, "no value after", v); script_free(s); return NULL; }
                    v = tok[i++];
                } else if ((v[0] == '+' || v[0] == '-') && v[1] == '=') { a.rel = v[0] == '+' ? 1 : -1; v += 2; }
                snprintf(a.sval, sizeof a.sval, "%s", v);
            }
            else { fail(err, errn, s->name, lineno, "unknown word", w); script_free(s); return NULL; }

            if (i + 1 < n && !strcmp(tok[i], "over") && is_number(tok[i + 1])) { a.over = atof(tok[i + 1]); i += 2; }
            if (push_action(s, &a) != 0) { script_free(s); return NULL; }
            ev.n++;
        }
        if (ev.n && push_event(s, &ev) != 0) { script_free(s); return NULL; }
    }
    if (s->nevs == 0) { fail(err, errn, s->name, 0, "nothing in it", NULL); script_free(s); return NULL; }
    return s;
}

script *script_load(const char *path, char *err, size_t errn)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (err && errn) snprintf(err, errn, "cannot read %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 1 << 20) { fclose(f); if (err && errn) snprintf(err, errn, "%s: not a script", path); return NULL; }
    char *text = malloc((size_t)len + 1);
    if (!text) { fclose(f); return NULL; }
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = 0;
    const char *base = strrchr(path, '/');
    script *s = script_parse(text, base ? base + 1 : path, err, errn);
    free(text);
    return s;
}

void script_free(script *s)
{
    if (!s) return;
    free(s->acts); free(s->evs); free(s);
}

/* ------------------------------------------------------------------ running */

static void drop_tween(script *s, const char *name)
{
    for (int i = 0; i < s->ntw; i++)
        if (!strcmp(s->tw[i].name, name)) { s->tw[i] = s->tw[--s->ntw]; i--; }
}

static void start_tween(script *s, app *a, const char *name, double v1, double over)
{
    drop_tween(s, name);
    if (over <= 0) { param_set(a, name, v1); return; }
    if (s->ntw == MAX_TWEENS) { param_set(a, name, v1); return; }
    tween *t = &s->tw[s->ntw++];
    snprintf(t->name, sizeof t->name, "%s", name);
    t->v0 = param_get(a, name); t->v1 = v1;
    t->t0 = s->t; t->t1 = s->t + over;
}

static void run_action(script *s, app *a, const action *ac)
{
    const wave *w = a->w;
    switch (ac->kind) {
    case A_SET: {
        const param *p = param_find(ac->name);
        if (!p) return;
        if (p->kind == PK_REAL || p->kind == PK_INT) {
            char *e;
            double v = strtod(ac->sval, &e);
            if (e == ac->sval) return;
            if (*e == '%') v /= 100.0;
            if (ac->rel) v = param_get(a, ac->name) + ac->rel * v;
            start_tween(s, a, ac->name, v, ac->over);
        } else {
            drop_tween(s, ac->name);
            param_set_str(a, ac->name, ac->sval);
        }
        break;
    }
    case A_CAMERA:
        start_tween(s, a, "yaw", ac->x, ac->over);
        start_tween(s, a, "pitch", ac->y, ac->over);
        if (ac->z > 0) start_tween(s, a, "dist", ac->z, ac->over);
        break;
    case A_DROP: {
        double x, y;
        if (ac->random) {
            do {
                x = rand() / (RAND_MAX + 1.0) * w->Lx; y = rand() / (RAND_MAX + 1.0) * w->Ly;
            } while (w->shape == WAVE_DISK && (x - w->R) * (x - w->R) + (y - w->R) * (y - w->R) > 0.9 * w->R * w->R);
        } else { x = ac->x * w->Lx; y = ac->y * w->Ly; }
        const double sz = ac->size * sqrt(w->Lx * w->Ly);
        app_splash(a, x, y, sz, -0.15 * sz);
        break;
    }
    case A_CLEAR: wave_clear(a->w); break;
    case A_SAY:
        snprintf(a->caption, sizeof a->caption, "%s", ac->sval);
        a->hud_dirty = 1;
        break;
    case A_LOOP:
        s->t -= s->evs[s->next].t;      /* keep the phase within the frame */
        s->next = 0;
        s->ntw = 0;
        break;
    case A_END:
        s->running = 0;
        s->ntw = 0;
        a->caption[0] = 0;
        a->hud_dirty = 1;
        break;
    }
}

void script_start(script *s, app *a)
{
    s->running = 1; s->t = 0; s->next = 0; s->ntw = 0;
    a->caption[0] = 0;
    a->hud_dirty = 1;
}

void script_stop(script *s) { s->running = 0; s->ntw = 0; }
int  script_running(const script *s) { return s && s->running; }
const char *script_name(const script *s) { return s ? s->name : ""; }
double script_time(const script *s) { return s ? s->t : 0; }

void script_user_set(script *s, const char *name)
{
    if (s && s->running) drop_tween(s, name);
}

void script_update(script *s, app *a, double dt)
{
    if (!s || !s->running) return;
    s->t += dt;
    a->in_script = 1;
    /* fire what is due; a loop resets next, so guard against spinning */
    for (int guard = 0; s->running && s->next < s->nevs && s->evs[s->next].t <= s->t && guard < 1000; guard++) {
        const event *e = &s->evs[s->next];
        int looped = 0;
        for (int i = 0; i < e->n; i++) {
            const action *ac = &s->acts[e->first + i];
            run_action(s, a, ac);
            if (ac->kind == A_LOOP) { looped = 1; break; }
            if (!s->running) break;
        }
        if (!looped && s->running) s->next++;
    }
    if (s->running && s->next >= s->nevs && s->ntw == 0) s->running = 0;   /* ran off the end */
    /* the tweens: smoothstep from v0 to v1 */
    for (int i = 0; i < s->ntw; i++) {
        tween *t = &s->tw[i];
        double u = (s->t - t->t0) / (t->t1 - t->t0);
        if (u >= 1.0) { param_set(a, t->name, t->v1); s->tw[i] = s->tw[--s->ntw]; i--; continue; }
        if (u < 0) u = 0;
        const double e = u * u * (3.0 - 2.0 * u);
        param_set(a, t->name, t->v0 + (t->v1 - t->v0) * e);
    }
    a->in_script = 0;
}
