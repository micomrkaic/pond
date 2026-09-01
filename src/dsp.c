/* dsp.c — noise-suite synthesis core.  See dsp.h and README.md.
 * Vendored into pond unchanged from noise-suite ("dsp: delayed onsets, glide,
 * grains", September 2026): https://github.com/micomrkaic/noise-suite
 *
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
#include "dsp.h"
#include <math.h>

/* ---- primitives ---- */

double dsp_rng_uniform(dsp_rng *r)
{
    r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27;
    return (double)((r->s * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}
double dsp_rng_white(dsp_rng *r) { return 2.0 * dsp_rng_uniform(r) - 1.0; }
double dsp_lp1_run(dsp_lp1 *f, double x, double a) { f->y += a * (x - f->y); return f->y; }
double dsp_lp_coef(double fc, double rate) { return 1.0 - exp(-DSP_TWO_PI * fc / rate); }

static void seed(dsp_rng *r, uint64_t k) { r->s = 0x9E3779B97F4A7C15ull ^ (k * 0xD1B54A32D192ED03ull); if (!r->s) r->s = 1; }
static double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void pan2(double s, double pan, double *l, double *r)
{
    /* constant-power pan, pan in [-1, 1] */
    double a = 0.25 * DSP_TWO_PI * (0.5 * (pan + 1.0));   /* 0 .. pi/2 */
    *l += s * cos(a); *r += s * sin(a);
}

/* ---- coloured noise ---- */

void dsp_white_init(dsp_white *s, double rate) { seed(&s->rng, 1); s->rate = rate; s->fc = 20000.0; s->tone.y = 0; }
double dsp_white_run(dsp_white *s) { return dsp_lp1_run(&s->tone, dsp_rng_white(&s->rng), dsp_lp_coef(s->fc, s->rate)); }

void dsp_pink_init(dsp_pink *s, double rate)
{
    seed(&s->rng, 2); s->rate = rate; s->fc = 20000.0; s->tone.y = 0;
    for (int i = 0; i < 7; i++) s->b[i] = 0;
}
double dsp_pink_run(dsp_pink *s)
{
    double *b = s->b, w = dsp_rng_white(&s->rng);
    b[0] = 0.99886 * b[0] + w * 0.0555179;
    b[1] = 0.99332 * b[1] + w * 0.0750759;
    b[2] = 0.96900 * b[2] + w * 0.1538520;
    b[3] = 0.86650 * b[3] + w * 0.3104856;
    b[4] = 0.55000 * b[4] + w * 0.5329522;
    b[5] = -0.7616 * b[5] - w * 0.0168980;
    double out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
    b[6] = w * 0.115926;
    return dsp_lp1_run(&s->tone, out * 0.11, dsp_lp_coef(s->fc, s->rate));
}

void dsp_brown_init(dsp_brown *s, double rate) { seed(&s->rng, 3); s->rate = rate; s->leak = 0.995; s->acc = 0; }
double dsp_brown_run(dsp_brown *s)
{
    double L = s->leak;
    s->acc = L * s->acc + 0.02 * dsp_rng_white(&s->rng);
    double comp = sqrt((1.0 - 0.997 * 0.997) / (1.0 - L * L));
    return s->acc * 3.5 * comp;
}

void dsp_deep_init(dsp_deep *s, double rate) { seed(&s->rng, 4); s->rate = rate; s->p2 = 0.997; s->a1 = s->a2 = 0; }
double dsp_deep_run(dsp_deep *s)
{
    double p1 = 0.997, p2 = s->p2;
    s->a1 = p1 * s->a1 + dsp_rng_white(&s->rng);
    s->a2 = p2 * s->a2 + s->a1;
    double var = (1.0 + p1 * p2) / ((1.0 - p1 * p2) * (1.0 - p1 * p1) * (1.0 - p2 * p2)) / 3.0;
    return s->a2 / sqrt(var) * 0.5;
}

/* ---- rain ---- */

void dsp_rain_init(dsp_rain *s, double rate)
{
    seed(&s->rng, 5); s->rate = rate;
    s->spawn_rate = 60.0; s->tone = 600.0; s->drop_level = 1.6; s->bed_level = 0.15;
    s->grain_rate = 0.0; s->grain_level = 1.0;
    for (int i = 0; i < DSP_MAX_DROPS; i++) s->d[i].alive = 0;
    s->hiss_hp.y = s->bed_lp.y = s->bed_lp2.y = s->wob_lp.y = 0;
}

static dsp_drop *free_drop(dsp_rain *s)
{
    for (int i = 0; i < DSP_MAX_DROPS; i++) if (!s->d[i].alive) return &s->d[i];
    return 0;
}

void dsp_rain_spawn(dsp_rain *s, double amp, double tone, double pan, double decay_ms, double delay_ms)
{
    dsp_drop *d = free_drop(s);
    if (!d) return;
    double dec_ms = decay_ms > 0 ? decay_ms : 8.0 + dsp_rng_uniform(&s->rng) * 22.0;
    double atk_ms = 1.0 + dsp_rng_uniform(&s->rng) * 2.0;
    if (atk_ms > 0.5 * dec_ms) atk_ms = 0.5 * dec_ms;
    double T = tone > 0 ? tone : s->tone;
    d->alive = 1;
    d->wait = (int)(delay_ms * 0.001 * s->rate);
    d->f.y = d->h.y = 0.0;
    d->dd = exp(-1.0 / (dec_ms * 0.001 * s->rate));
    d->da = exp(-1.0 / (atk_ms * 0.001 * s->rate));
    d->ed = d->ea = 1.0;
    d->coef = dsp_lp_coef(T * 0.5 + dsp_rng_uniform(&s->rng) * T * 2.0, s->rate);
    d->hcoef = 0.0;
    d->fq = 0.0; d->ph = 0.0;
    d->amp = amp > 0 ? amp : 0.20 + dsp_rng_uniform(&s->rng) * dsp_rng_uniform(&s->rng) * 0.60;
    d->pan = clamp(pan, -1.0, 1.0);
}

/* a grain: a 0.5..2 ms burst of 2..8 kHz noise, or, one time in six, a 4..8 kHz plink of
 * the same length; random pan; amplitude skewed to the quiet */
static void spawn_grain(dsp_rain *s)
{
    dsp_drop *d = free_drop(s);
    if (!d) return;
    double u = dsp_rng_uniform(&s->rng);
    double dec_ms = 0.5 + 1.5 * u * u;
    d->alive = 1; d->wait = 0;
    d->f.y = d->h.y = 0.0;
    d->dd = exp(-1.0 / (dec_ms * 0.001 * s->rate));
    d->da = exp(-1.0 / (0.15 * 0.001 * s->rate));
    d->ed = d->ea = 1.0;
    if (dsp_rng_uniform(&s->rng) < 1.0 / 6.0) {
        d->fq = 4000.0 + 4000.0 * dsp_rng_uniform(&s->rng); d->ph = 0.0;
        d->coef = d->hcoef = 0.0;
    } else {
        d->fq = 0.0;
        d->coef = dsp_lp_coef(2000.0 + 6000.0 * dsp_rng_uniform(&s->rng), s->rate);
        d->hcoef = dsp_lp_coef(1500.0, s->rate);
    }
    double a = dsp_rng_uniform(&s->rng);
    d->amp = s->grain_level * (0.05 + 0.25 * a * a);
    d->pan = 2.0 * dsp_rng_uniform(&s->rng) - 1.0;
}

void dsp_rain_run(dsp_rain *s, double *l, double *r)
{
    if (s->spawn_rate > 0 && dsp_rng_uniform(&s->rng) < s->spawn_rate / s->rate) dsp_rain_spawn(s, 0, 0, 0, 0, 0);
    if (s->grain_rate > 0) {
        /* a Poisson process at a rate that may exceed one per sample */
        double lam = s->grain_rate / s->rate;
        while (lam > 0) {
            if (lam >= 1.0 || dsp_rng_uniform(&s->rng) < lam) spawn_grain(s);
            lam -= 1.0;
        }
    }
    double sl = 0, sr = 0;
    for (int i = 0; i < DSP_MAX_DROPS; i++) {
        dsp_drop *d = &s->d[i];
        if (!d->alive) continue;
        if (d->wait > 0) { d->wait--; continue; }
        double env = d->ed - d->ea, v;
        if (d->fq > 0) {
            v = d->amp * env * sin(d->ph);
            d->ph += DSP_TWO_PI * d->fq / s->rate;
        } else {
            double x = dsp_rng_white(&s->rng);
            if (d->hcoef > 0) x -= dsp_lp1_run(&d->h, x, d->hcoef);
            v = d->amp * env * dsp_lp1_run(&d->f, x, d->coef);
        }
        pan2(v, d->pan, &sl, &sr);
        d->ed *= d->dd; d->ea *= d->da;
        if (d->ed < 1e-4) d->alive = 0;
    }
    double w = dsp_rng_white(&s->rng);
    double hp = w - dsp_lp1_run(&s->hiss_hp, w, dsp_lp_coef(400.0, s->rate));
    double bed = dsp_lp1_run(&s->bed_lp2, dsp_lp1_run(&s->bed_lp, hp, dsp_lp_coef(4000.0, s->rate)), dsp_lp_coef(4000.0, s->rate));
    double wob = clamp(1.0 + 80.0 * dsp_lp1_run(&s->wob_lp, dsp_rng_white(&s->rng), dsp_lp_coef(0.3, s->rate)), 0.5, 1.5);
    double b = s->bed_level * bed * wob;
    /* the point sources were panned at unit power; the bed is centred */
    *l = s->drop_level * sl * 1.41421356 + b;
    *r = s->drop_level * sr * 1.41421356 + b;
}

/* ---- sea ---- */

void dsp_sea_init(dsp_sea *s, double rate)
{
    seed(&s->rng, 6); s->rate = rate; s->t = 0;
    s->period = 9.0; s->crash_pow = 3.0; s->bright_hz = 5500.0; s->rumble_level = 2.6;
    s->ext_env = -1.0; s->env = 0;
    s->surf.y = s->r1.y = s->r2.y = 0;
}
double dsp_sea_run(dsp_sea *s)
{
    s->t += 1.0 / s->rate;
    double e;
    if (s->ext_env >= 0) e = clamp(s->ext_env, 0.0, 1.0);
    else e = clamp(0.5 + 0.30 * sin(DSP_TWO_PI * s->t / s->period) + 0.20 * sin(DSP_TWO_PI * s->t / (1.522 * s->period) + 1.0), 0.0, 1.0);
    double crash = pow(e, s->crash_pow);
    s->env = crash;
    double fc = 250.0 + s->bright_hz * crash;
    double surf = dsp_lp1_run(&s->surf, dsp_rng_white(&s->rng), dsp_lp_coef(fc, s->rate)) * (0.15 + 0.85 * crash);
    double rum = dsp_lp1_run(&s->r2, dsp_lp1_run(&s->r1, dsp_rng_white(&s->rng), dsp_lp_coef(120.0, s->rate)), dsp_lp_coef(120.0, s->rate));
    return surf + s->rumble_level * rum;
}

/* ---- wind ---- */

void dsp_wind_init(dsp_wind *s, double rate)
{
    seed(&s->rng, 7); s->rate = rate;
    s->gustiness = 0.6; s->rustle = 0.6; s->tone = 250.0; s->gust = 0.5;
    s->wg.y = s->wf.y = s->wb1.y = s->wb2.y = s->wr_hp.y = s->wr_lp.y = 0;
}
double dsp_wind_run(dsp_wind *s)
{
    double g = clamp(0.5 + 400.0 * s->gustiness * dsp_lp1_run(&s->wg, dsp_rng_white(&s->rng), dsp_lp_coef(0.12, s->rate)), 0.05, 1.0);
    s->gust = g;
    double fl = clamp(1.0 + 30.0 * dsp_lp1_run(&s->wf, dsp_rng_white(&s->rng), dsp_lp_coef(1.5, s->rate)), 0.7, 1.3);
    double fc = s->tone * (0.6 + 1.8 * g);
    double body = dsp_lp1_run(&s->wb2, dsp_lp1_run(&s->wb1, dsp_rng_white(&s->rng), dsp_lp_coef(fc, s->rate)), dsp_lp_coef(fc, s->rate));
    double w = dsp_rng_white(&s->rng);
    double hp = w - dsp_lp1_run(&s->wr_hp, w, dsp_lp_coef(2000.0, s->rate));
    double rust = dsp_lp1_run(&s->wr_lp, hp, dsp_lp_coef(6000.0, s->rate));
    return (2.6 * body * (0.15 + 0.85 * g) + 0.55 * s->rustle * g * g * rust) * fl;
}

/* ---- stream ---- */

void dsp_stream_init(dsp_stream *s, double rate)
{
    seed(&s->rng, 8); s->rate = rate;
    s->spawn_rate = 50.0; s->pitch = 900.0; s->bubble_level = 0.9; s->flow_level = 0.12;
    for (int i = 0; i < DSP_MAX_BUBBLES; i++) s->b[i].alive = 0;
    s->hp.y = s->lp.y = s->wob.y = 0;
}
void dsp_stream_spawn(dsp_stream *s, double f0, double amp, double pan, double decay_ms, double glide, double delay_ms)
{
    for (int i = 0; i < DSP_MAX_BUBBLES; i++)
        if (!s->b[i].alive) {
            dsp_bubble *b = &s->b[i];
            double dec_ms = decay_ms > 0 ? decay_ms : 10.0 + dsp_rng_uniform(&s->rng) * 30.0;
            double atk_ms = glide > 0 ? 0.4 : 1.0 + dsp_rng_uniform(&s->rng) * 2.0;
            if (atk_ms > 0.5 * dec_ms) atk_ms = 0.5 * dec_ms;
            b->alive = 1;
            b->wait = (int)(delay_ms * 0.001 * s->rate);
            b->ph = 0.0;
            b->f = f0 > 0 ? f0 : s->pitch * (0.6 + 1.2 * dsp_rng_uniform(&s->rng));
            if (glide > 0) b->c = pow(glide, 1.0 / (dec_ms * 0.001 * s->rate));    /* reach `glide` after one decay time */
            else b->c = 1.0 + (0.3 + dsp_rng_uniform(&s->rng)) * 0.00045;
            b->dd = exp(-1.0 / (dec_ms * 0.001 * s->rate));
            b->da = exp(-1.0 / (atk_ms * 0.001 * s->rate));
            b->ed = b->ea = 1.0;
            b->amp = amp > 0 ? amp : 0.05 + dsp_rng_uniform(&s->rng) * dsp_rng_uniform(&s->rng) * 0.25;
            b->pan = clamp(pan, -1.0, 1.0);
            return;
        }
}
void dsp_stream_run(dsp_stream *s, double *l, double *r)
{
    if (s->spawn_rate > 0 && dsp_rng_uniform(&s->rng) < s->spawn_rate / s->rate) dsp_stream_spawn(s, 0, 0, 0, 0, 0, 0);
    double sl = 0, sr = 0;
    for (int i = 0; i < DSP_MAX_BUBBLES; i++) {
        dsp_bubble *b = &s->b[i];
        if (!b->alive) continue;
        if (b->wait > 0) { b->wait--; continue; }
        pan2(b->amp * (b->ed - b->ea) * sin(b->ph), b->pan, &sl, &sr);
        b->ph += DSP_TWO_PI * b->f / s->rate;
        b->f *= b->c;
        b->ed *= b->dd; b->ea *= b->da;
        if (b->ed < 1e-4) b->alive = 0;
    }
    double w = dsp_rng_white(&s->rng);
    double hp = w - dsp_lp1_run(&s->hp, w, dsp_lp_coef(700.0, s->rate));
    double bed = dsp_lp1_run(&s->lp, hp, dsp_lp_coef(3000.0, s->rate));
    double wob = clamp(1.0 + 40.0 * dsp_lp1_run(&s->wob, dsp_rng_white(&s->rng), dsp_lp_coef(2.0, s->rate)), 0.6, 1.4);
    double f = s->flow_level * bed * wob;
    *l = s->bubble_level * sl * 1.41421356 + f;
    *r = s->bubble_level * sr * 1.41421356 + f;
}

/* ---- birds ---- */

void dsp_birds_init(dsp_birds *s, double rate)
{
    seed(&s->rng, 9); s->rate = rate;
    s->songs_per_10s = 2.5; s->pitch = 2800.0; s->ambience = 0.03;
    for (int i = 0; i < DSP_MAX_BIRDS; i++) s->v[i].active = 0;
    s->amb_hp.y = s->amb_lp.y = 0;
}
static void start_chirp(dsp_birds *s, dsp_birdv *b)
{
    b->chirping = 1;
    b->t = 0.0; b->ph = 0.0;
    b->f0 = s->pitch * (0.8 + 0.5 * dsp_rng_uniform(&s->rng));
    b->f = b->f0;
    b->glide = (dsp_rng_uniform(&s->rng) < 0.5 ? 1.0 : -1.0) * (0.10 + 0.25 * dsp_rng_uniform(&s->rng));
    b->trill_m = dsp_rng_uniform(&s->rng) * 0.10;
    b->trill_f = 20.0 + dsp_rng_uniform(&s->rng) * 40.0;
    b->trill_ph = 0.0;
    b->dur = (0.04 + dsp_rng_uniform(&s->rng) * 0.11) * s->rate;
    double atk_ms = 3.0 + dsp_rng_uniform(&s->rng) * 6.0, dec_ms = 30.0 + dsp_rng_uniform(&s->rng) * 90.0;
    b->dd = exp(-1.0 / (dec_ms * 0.001 * s->rate));
    b->da = exp(-1.0 / (atk_ms * 0.001 * s->rate));
    b->ed = b->ea = 1.0;
    b->amp = 0.10 + dsp_rng_uniform(&s->rng) * 0.20;
}
double dsp_birds_run(dsp_birds *s)
{
    if (dsp_rng_uniform(&s->rng) < (s->songs_per_10s / 10.0) / s->rate) {
        for (int i = 0; i < DSP_MAX_BIRDS; i++)
            if (!s->v[i].active) {
                s->v[i].active = 1;
                s->v[i].chirps_left = 2 + (int)(dsp_rng_uniform(&s->rng) * 5.0);
                s->v[i].gap = 0.0;
                s->v[i].chirping = 0;
                break;
            }
    }
    double out = 0.0;
    for (int i = 0; i < DSP_MAX_BIRDS; i++) {
        dsp_birdv *b = &s->v[i];
        if (!b->active) continue;
        if (!b->chirping) {
            b->gap -= 1.0;
            if (b->gap <= 0.0) {
                if (b->chirps_left-- > 0) start_chirp(s, b);
                else { b->active = 0; continue; }
            }
        }
        if (b->chirping) {
            double frac = b->t / b->dur;
            if (frac > 1.0) frac = 1.0;
            out += b->amp * (b->ed - b->ea) * sin(b->ph);
            b->ph += DSP_TWO_PI * b->f * (1.0 + b->trill_m * sin(b->trill_ph)) / s->rate;
            b->trill_ph += DSP_TWO_PI * b->trill_f / s->rate;
            b->f = b->f0 * (1.0 + b->glide * frac);
            b->ed *= b->dd; b->ea *= b->da;
            b->t += 1.0;
            if (b->t >= b->dur && b->ed < 5e-3) {
                b->chirping = 0;
                b->gap = (0.06 + dsp_rng_uniform(&s->rng) * 0.14) * s->rate;
            }
        }
    }
    double w = dsp_rng_white(&s->rng);
    double hp = w - dsp_lp1_run(&s->amb_hp, w, dsp_lp_coef(400.0, s->rate));
    double amb = dsp_lp1_run(&s->amb_lp, hp, dsp_lp_coef(2500.0, s->rate));
    return out + s->ambience * amb;
}

/* ---- mixer ---- */

const char *const dsp_kind_names[DSP_NUM] = { "white", "pink", "brown", "deep", "rain", "sea", "wind", "stream", "birds" };

void dsp_mixer_init(dsp_mixer *m, double rate)
{
    m->rate = (rate > 8000.0 && rate < 384000.0) ? rate : 44100.0;
    for (int i = 0; i < DSP_NUM; i++) m->gain[i] = 0.0;
    dsp_white_init(&m->white, m->rate); dsp_pink_init(&m->pink, m->rate);
    dsp_brown_init(&m->brown, m->rate); dsp_deep_init(&m->deep, m->rate);
    dsp_rain_init(&m->rain, m->rate);   dsp_sea_init(&m->sea, m->rate);
    dsp_wind_init(&m->wind, m->rate);   dsp_stream_init(&m->stream, m->rate);
    dsp_birds_init(&m->birds, m->rate);
}

void dsp_mixer_run(dsp_mixer *m, double *l, double *r)
{
    double L = 0, R = 0, g, a, b;
    if ((g = m->gain[DSP_WHITE]) > 0) { a = g * dsp_white_run(&m->white); L += a; R += a; }
    if ((g = m->gain[DSP_PINK])  > 0) { a = g * dsp_pink_run(&m->pink);   L += a; R += a; }
    if ((g = m->gain[DSP_BROWN]) > 0) { a = g * dsp_brown_run(&m->brown); L += a; R += a; }
    if ((g = m->gain[DSP_DEEP])  > 0) { a = g * dsp_deep_run(&m->deep);   L += a; R += a; }
    if ((g = m->gain[DSP_RAIN])  > 0) { dsp_rain_run(&m->rain, &a, &b);   L += g * a; R += g * b; }
    if ((g = m->gain[DSP_SEA])   > 0) { a = g * dsp_sea_run(&m->sea);     L += a; R += a; }
    if ((g = m->gain[DSP_WIND])  > 0) { a = g * dsp_wind_run(&m->wind);   L += a; R += a; }
    if ((g = m->gain[DSP_STREAM]) > 0) { dsp_stream_run(&m->stream, &a, &b); L += g * a; R += g * b; }
    if ((g = m->gain[DSP_BIRDS]) > 0) { a = g * dsp_birds_run(&m->birds); L += a; R += a; }
    *l = L; *r = R;
}

void dsp_mixer_render(dsp_mixer *m, float *out, int frames, double gain)
{
    for (int i = 0; i < frames; i++) {
        double l, r;
        dsp_mixer_run(m, &l, &r);
        l = clamp(l * gain, -1.0, 1.0); r = clamp(r * gain, -1.0, 1.0);
        out[2 * i] = (float)l; out[2 * i + 1] = (float)r;
    }
}

/* ---- the original interface: one sound at a time on a global mixer ---- */

#define BUF_N 4096
static dsp_mixer g_mix;
static int g_cur = DSP_BROWN;
static float g_buf[BUF_N];
static int g_inited = 0;

void dsp_init(double rate)
{
    dsp_mixer_init(&g_mix, rate);
    g_inited = 1;
    g_mix.gain[g_cur] = 1.0;
}
void dsp_set_sound(int i)
{
    if (i < 0 || i >= DSP_NUM) return;
    if (!g_inited) dsp_init(44100.0);
    g_mix.gain[g_cur] = 0.0;
    g_cur = i;
    g_mix.gain[g_cur] = 1.0;
}
/* parameter indices as in the original dsp.c and the web UI */
void dsp_set_param(int i, double v)
{
    if (!g_inited) dsp_init(44100.0);
    dsp_mixer *m = &g_mix;
    switch (i) {
    case 0: m->white.fc = v; break;
    case 1: m->pink.fc = v; break;
    case 2: m->brown.leak = v; break;
    case 3: m->deep.p2 = v; break;
    case 4: m->rain.spawn_rate = v; break;
    case 5: m->rain.tone = v; break;
    case 6: m->rain.drop_level = v; break;
    case 7: m->rain.bed_level = v; break;
    case 8: m->sea.period = v; break;
    case 9: m->sea.crash_pow = v; break;
    case 10: m->sea.bright_hz = v; break;
    case 11: m->sea.rumble_level = v; break;
    case 12: m->wind.gustiness = v; break;
    case 13: m->wind.rustle = v; break;
    case 14: m->wind.tone = v; break;
    case 15: m->stream.spawn_rate = v; break;
    case 16: m->stream.pitch = v; break;
    case 17: m->stream.bubble_level = v; break;
    case 18: m->stream.flow_level = v; break;
    case 19: m->birds.songs_per_10s = v; break;
    case 20: m->birds.pitch = v; break;
    case 21: m->birds.ambience = v; break;
    default: break;
    }
}
double dsp_get_param(int i)
{
    if (!g_inited) dsp_init(44100.0);
    const dsp_mixer *m = &g_mix;
    switch (i) {
    case 0: return m->white.fc;      case 1: return m->pink.fc;
    case 2: return m->brown.leak;    case 3: return m->deep.p2;
    case 4: return m->rain.spawn_rate; case 5: return m->rain.tone;
    case 6: return m->rain.drop_level; case 7: return m->rain.bed_level;
    case 8: return m->sea.period;    case 9: return m->sea.crash_pow;
    case 10: return m->sea.bright_hz; case 11: return m->sea.rumble_level;
    case 12: return m->wind.gustiness; case 13: return m->wind.rustle; case 14: return m->wind.tone;
    case 15: return m->stream.spawn_rate; case 16: return m->stream.pitch;
    case 17: return m->stream.bubble_level; case 18: return m->stream.flow_level;
    case 19: return m->birds.songs_per_10s; case 20: return m->birds.pitch; case 21: return m->birds.ambience;
    default: return 0.0;
    }
}
int dsp_num_params(void) { return 22; }
float *dsp_get_buf(void) { return g_buf; }
void dsp_render(int n)
{
    if (!g_inited) dsp_init(44100.0);
    if (n > BUF_N) n = BUF_N;
    for (int i = 0; i < n; i++) {
        double l, r;
        dsp_mixer_run(&g_mix, &l, &r);
        /* 0.35 headroom, as before: raw generator peaks exceed +-1 and this clamp sits
         * before the volume gain in the web audio graph */
        double s = 0.5 * (l + r) * 0.35;
        g_buf[i] = (float)clamp(s, -1.0, 1.0);
    }
}
