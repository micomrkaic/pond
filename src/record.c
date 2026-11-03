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

#include "record.h"
#include "param.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* a real being swept by a held key: written as one line once the key is up */
typedef struct {
    char   name[32];
    double t0, t1;          /* first and last press */
    double v0, v1;          /* the value before the first press, and after the last */
    double acc;             /* for a value that wraps round: the steps, summed the short way */
    int    wrap;
} sweep;
#define MAX_SWEEP 8
#define SWEEP_GAP 0.45      /* seconds without a press: the sweep is over */

#define FINGER_MAX 512
#define FINGER_LINE 40      /* points on one script line (the parser takes 64 words) */

struct recorder {
    FILE  *f;
    int    own;             /* the file is ours to close */
    char   path[256];
    double t;
    int    last_sec;
    sweep  sw[MAX_SWEEP]; int nsw;
    /* the camera: a keyframe is written where it comes to rest */
    double cy, cp, cd;      /* as last sampled */
    double k0, ky, kp, kd;  /* an open keyframe: when it began, and where the camera was */
    double kacc;            /* the yaw turned since, summed the short way round */
    double krest;           /* how long the camera has been still */
    int    kopen;
    int    sy, sp, sd;      /* which way yaw, pitch and dist have been going: a turn starts a new keyframe */
    /* a finger through the water: its path, written when it lifts */
    double fx[FINGER_MAX], fy[FINGER_MAX], ft[FINGER_MAX]; int nf;
    double f_t0;
    int    f_down;          /* seen this frame */
};

static void fnum(FILE *f, double v) { fprintf(f, "%.6g", v); }

static void at(recorder *r, double t) { fprintf(r->f, "at %-7.2f ", t); }

/* the shortest way round from a to b on a range of w */
static double short_way(double a, double b, double w)
{
    double d = fmod(b - a, w);
    if (d > 0.5 * w) d -= w;
    if (d < -0.5 * w) d += w;
    return d;
}

static void write_sweep(recorder *r, const sweep *s)
{
    const double over = s->t1 - s->t0;
    at(r, s->t0);
    if (s->wrap) {
        fprintf(r->f, "%s %s ", s->name, s->acc < 0 ? "-=" : "+=");
        fnum(r->f, fabs(s->acc));
    } else {
        fprintf(r->f, "%s ", s->name);
        fnum(r->f, s->v1);
    }
    if (over >= 0.15) { fprintf(r->f, " over "); fnum(r->f, over); }
    fputc('\n', r->f);
}

static void flush_sweeps(recorder *r, int all)
{
    for (int i = 0; i < r->nsw; i++)
        if (all || r->t - r->sw[i].t1 > SWEEP_GAP) {
            write_sweep(r, &r->sw[i]);
            r->sw[i] = r->sw[--r->nsw]; i--;
        }
}

/* the camera's keyframe: where it was still, in one line, the yaw the short way round */
static void close_keyframe(recorder *r)
{
    if (!r->kopen) return;
    r->kopen = 0;
    const double over = r->t - r->krest - r->k0;
    const int dy = fabs(r->kacc) > 0.05, dp = fabs(r->cp - r->kp) > 0.05, dd = fabs(r->cd - r->kd) > 1e-3 * r->kd;
    if (!dy && !dp && !dd) return;
    at(r, r->k0);
    const char *ov = over >= 0.15 ? " over " : "";
    char os[32] = "";
    if (over >= 0.15) snprintf(os, sizeof os, "%.6g", over);
    if (dy) { fprintf(r->f, "yaw %s ", r->kacc < 0 ? "-=" : "+="); fnum(r->f, fabs(r->kacc)); fprintf(r->f, "%s%s  ", ov, os); }
    if (dp) { fprintf(r->f, "pitch "); fnum(r->f, r->cp); fprintf(r->f, "%s%s  ", ov, os); }
    if (dd) { fprintf(r->f, "dist "); fnum(r->f, r->cd); fprintf(r->f, "%s%s", ov, os); }
    fputc('\n', r->f);
}

static void write_finger(recorder *r)
{
    if (r->nf == 0) return;
    /* a long drag goes over several lines, each taking up where the last left off */
    for (int i = 0; i + 1 < r->nf || i == 0; ) {
        int j = i + FINGER_LINE - 1;
        if (j >= r->nf) j = r->nf - 1;
        at(r, r->ft[i]);
        fprintf(r->f, "finger");
        for (int k = i; k <= j; k++) fprintf(r->f, " %.4g,%.4g", r->fx[k], r->fy[k]);
        const double over = r->ft[j] - r->ft[i];
        if (over > 0.0) { fprintf(r->f, " over "); fnum(r->f, over); }
        fputc('\n', r->f);
        if (j == r->nf - 1) break;
        i = j;
    }
    r->nf = 0;
}

/* every parameter, grouped, as the show's opening state */
static void snapshot(recorder *r, app *a)
{
    char v[64];
    fprintf(r->f, "# a show recorded with pond: play it with  pond --script %s\n", r->path);
    fprintf(r->f, "# (the opening lines are the state it was recorded from; what follows is what was done)\n");
    param_get_str(a, "preset", v, sizeof v);
    fprintf(r->f, "at 0       preset %s  ", v);
    const char *grp = "basin";
    for (int i = 0; i < param_count(); i++) {
        const param *p = param_at(i);
        if (!strcmp(p->name, "preset") || !strcmp(p->name, "help") || !strcmp(p->name, "fullscreen") || !strcmp(p->name, "paused")) continue;
        if (strcmp(p->group, grp)) { grp = p->group; fprintf(r->f, "\nat 0       "); }
        param_get_str(a, p->name, v, sizeof v);
        fprintf(r->f, "%s %s  ", p->name, v);
    }
    fprintf(r->f, "\nat 0       clear\n");
}

recorder *rec_start(app *a, const char *path)
{
    recorder *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (path && !strcmp(path, "-")) { r->f = stdout; snprintf(r->path, sizeof r->path, "stdout"); }
    else {
        if (path) snprintf(r->path, sizeof r->path, "%s", path);
        else {
            /* the first name not taken */
            for (int n = 1; n < 1000; n++) {
                snprintf(r->path, sizeof r->path, "pond-rec-%03d.pond", n);
                FILE *t = fopen(r->path, "rb");
                if (!t) break;
                fclose(t);
            }
        }
        r->f = fopen(r->path, "wb");
        if (!r->f) { free(r); return NULL; }
        r->own = 1;
    }
    r->cy = param_get(a, "yaw"); r->cp = param_get(a, "pitch"); r->cd = param_get(a, "dist");
    snapshot(r, a);
    fflush(r->f);
    return r;
}

void rec_stop(recorder *r, app *a, int loop)
{
    if (!r) return;
    (void)a;
    flush_sweeps(r, 1);
    r->krest = 0; close_keyframe(r);
    write_finger(r);
    at(r, r->t);
    fprintf(r->f, "%s\n", loop ? "loop" : "end");
    if (r->own) fclose(r->f); else fflush(r->f);
    free(r);
}

void rec_tick(recorder *r, app *a, double dt)
{
    if (!r) return;
    r->t += dt;
    flush_sweeps(r, 0);
    /* the camera */
    {
        const double y = param_get(a, "yaw"), p = param_get(a, "pitch"), d = param_get(a, "dist");
        const double dy = short_way(r->cy, y, 360.0), dp = p - r->cp, dd = d - r->cd;
        const int my = fabs(dy) > 1e-3, mp = fabs(dp) > 1e-3, md = fabs(dd) > 1e-6 * d;
        if (my || mp || md) {
            /* a turn -- the camera coming back the way it went -- is where a keyframe belongs */
            const int ty = my && r->sy && (dy > 0) != (r->sy > 0), tp = mp && r->sp && (dp > 0) != (r->sp > 0), td = md && r->sd && (dd > 0) != (r->sd > 0);
            if (r->kopen && (ty || tp || td)) { r->krest = 0; close_keyframe(r); }
            if (!r->kopen) { r->kopen = 1; r->k0 = r->t - dt; r->ky = r->cy; r->kp = r->cp; r->kd = r->cd; r->kacc = 0; r->sy = r->sp = r->sd = 0; }
            if (my) r->sy = dy > 0 ? 1 : -1;
            if (mp) r->sp = dp > 0 ? 1 : -1;
            if (md) r->sd = dd > 0 ? 1 : -1;
            r->kacc += dy;
            r->krest = 0;
        } else if (r->kopen) {
            r->krest += dt;
            if (r->krest > 0.35) close_keyframe(r);
        }
        r->cy = y; r->cp = p; r->cd = d;
    }
    /* the finger */
    if (!r->f_down && r->nf) write_finger(r);
    r->f_down = 0;
    if ((int)r->t != r->last_sec) { r->last_sec = (int)r->t; a->hud_dirty = 1; }
}

void rec_param(recorder *r, app *a, const char *name, double before)
{
    if (!r) return;
    const param *p = param_find(name);
    if (!p) return;
    if (!strcmp(p->name, "help") || !strcmp(p->name, "fullscreen")) return;
    char v[64];
    param_get_str(a, p->name, v, sizeof v);
    if (p->kind != PK_REAL || !strcmp(p->name, "floats")) {
        at(r, r->t);
        fprintf(r->f, "%s %s\n", p->name, v);
        fflush(r->f);
        return;
    }
    /* a real: one press, or one more of a sweep */
    const double val = p->get(a);
    const double step = p->wrap == 1 ? short_way(before, val, p->hi - p->lo) : val - before;
    for (int i = 0; i < r->nsw; i++) {
        sweep *s = &r->sw[i];
        if (strcmp(s->name, p->name)) continue;
        if (r->t - s->t1 > SWEEP_GAP) break;
        s->acc += step;
        s->v1 = val; s->t1 = r->t;
        return;
    }
    if (r->nsw == MAX_SWEEP) flush_sweeps(r, 1);
    sweep *s = &r->sw[r->nsw++];
    snprintf(s->name, sizeof s->name, "%s", p->name);
    s->t0 = s->t1 = r->t;
    s->wrap = p->wrap == 1;
    s->v0 = before; s->v1 = val;
    s->acc = step;
}

void rec_drop(recorder *r, app *a, double x, double y, double s)
{
    if (!r) return;
    const wave *w = a->w;
    at(r, r->t);
    fprintf(r->f, "drop %.4g,%.4g %.4g\n", x / w->Lx, y / w->Ly, s / sqrt(w->Lx * w->Ly));
}

void rec_clear(recorder *r)
{
    if (!r) return;
    at(r, r->t);
    fprintf(r->f, "clear\n");
}

void rec_jolt(recorder *r, double dux, double duy)
{
    if (!r) return;
    at(r, r->t);
    fprintf(r->f, "jolt %.4g,%.4g\n", dux, duy);
}

void rec_finger(recorder *r, app *a, double x, double y)
{
    if (!r) return;
    const wave *w = a->w;
    const double fx = x / w->Lx, fy = y / w->Ly;
    r->f_down = 1;
    if (r->nf == FINGER_MAX) write_finger(r);
    /* a point every tenth of a second, or sooner if it has moved a hundredth of the basin */
    if (r->nf) {
        const int last = r->nf - 1;
        if (r->t - r->ft[last] < 0.1 && fabs(fx - r->fx[last]) < 0.01 && fabs(fy - r->fy[last]) < 0.01) return;
    }
    r->fx[r->nf] = fx; r->fy[r->nf] = fy; r->ft[r->nf] = r->t; r->nf++;
}

const char *rec_path(const recorder *r) { return r ? r->path : ""; }
double      rec_time(const recorder *r) { return r ? r->t : 0.0; }
