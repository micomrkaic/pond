/* dsp.h — noise-suite synthesis core as a library: every sound is a struct
 * with its own state, initialised for a sample rate and run one sample at a
 * time; a mixer sums any set of them with per-layer gains.  libm only, no
 * allocation, no globals, so it compiles to a zero-import wasm module and
 * embeds in other programs (pond uses it for rain and wind you can see).
 *
 * The synthesis is unchanged from the original single-generator dsp.c; see
 * README.md for the mathematics.  Point-source layers (rain drops, stream
 * bubbles) carry a stereo pan per voice and can be spawned from outside,
 * which is how a program that knows where a drop landed makes it sound
 * from there.
 *
 * Copyright (C) 2026 Mico <https://github.com/micomrkaic>
 * GNU General Public License v3 or later; see LICENSE.
 *
 * Vendored into pond unchanged from noise-suite ("dsp.c as a library",
 * September 2026): https://github.com/micomrkaic/noise-suite
 */
#ifndef NOISE_DSP_H
#define NOISE_DSP_H

#include <stdint.h>

#define DSP_TWO_PI 6.28318530717958647692
#define DSP_MAX_DROPS   64
#define DSP_MAX_BUBBLES 32
#define DSP_MAX_BIRDS   3

typedef struct { uint64_t s; } dsp_rng;          /* xorshift64*, no libc rand */
typedef struct { double y; } dsp_lp1;            /* one-pole low-pass state */

double dsp_rng_uniform(dsp_rng *r);              /* [0,1) */
double dsp_rng_white(dsp_rng *r);                /* [-1,1) */
double dsp_lp1_run(dsp_lp1 *f, double x, double a);
double dsp_lp_coef(double fc, double rate);      /* alpha = 1 - exp(-2 pi fc / rate) */

/* ---- coloured noise ---- */
typedef struct { dsp_rng rng; dsp_lp1 tone; double rate, fc; } dsp_white;
typedef struct { dsp_rng rng; dsp_lp1 tone; double rate, fc, b[7]; } dsp_pink;
typedef struct { dsp_rng rng; double rate, leak, acc; } dsp_brown;
typedef struct { dsp_rng rng; double rate, p2, a1, a2; } dsp_deep;

void   dsp_white_init(dsp_white *s, double rate);
double dsp_white_run(dsp_white *s);
void   dsp_pink_init(dsp_pink *s, double rate);
double dsp_pink_run(dsp_pink *s);
void   dsp_brown_init(dsp_brown *s, double rate);
double dsp_brown_run(dsp_brown *s);
void   dsp_deep_init(dsp_deep *s, double rate);
double dsp_deep_run(dsp_deep *s);

/* ---- rain: Poisson shot noise of enveloped noise bursts over a hiss bed ---- */
typedef struct { double ed, dd, ea, da, coef, amp, pan; dsp_lp1 f; int alive; } dsp_drop;
typedef struct {
    dsp_rng rng; double rate;
    double spawn_rate;      /* internal Poisson rate, drops/s (0 = drops only from dsp_rain_spawn) */
    double tone;            /* drop tone, Hz */
    double drop_level, bed_level;
    dsp_drop d[DSP_MAX_DROPS];
    dsp_lp1 hiss_hp, bed_lp, bed_lp2, wob_lp;
} dsp_rain;
void dsp_rain_init(dsp_rain *s, double rate);
/* amp: 0 = draw as the internal process does; tone: 0 = the layer's tone; pan -1..1;
 * decay_ms: 0 = draw (8..30 ms) */
void dsp_rain_spawn(dsp_rain *s, double amp, double tone, double pan, double decay_ms);
void dsp_rain_run(dsp_rain *s, double *l, double *r);

/* ---- sea: co-modulated surf and rumble ---- */
typedef struct {
    dsp_rng rng; double rate, t;
    double period, crash_pow, bright_hz, rumble_level;
    double ext_env;         /* < 0: internal two-swell envelope; else this value (0..1) drives the surf */
    double env;             /* the envelope actually used, last sample */
    dsp_lp1 surf, r1, r2;
} dsp_sea;
void   dsp_sea_init(dsp_sea *s, double rate);
double dsp_sea_run(dsp_sea *s);

/* ---- wind: gust-modulated body plus leaf rustle ---- */
typedef struct {
    dsp_rng rng; double rate;
    double gustiness, rustle, tone;
    double gust;            /* the gust envelope, last sample, 0.05..1 (read it to drive something else) */
    dsp_lp1 wg, wf, wb1, wb2, wr_hp, wr_lp;
} dsp_wind;
void   dsp_wind_init(dsp_wind *s, double rate);
double dsp_wind_run(dsp_wind *s);

/* ---- stream: rising-chirp bubbles over a flow bed ---- */
typedef struct { double ph, f, c, ed, dd, ea, da, amp, pan; int alive; } dsp_bubble;
typedef struct {
    dsp_rng rng; double rate;
    double spawn_rate;      /* internal Poisson rate, bubbles/s (0 = only from dsp_stream_spawn) */
    double pitch, bubble_level, flow_level;
    dsp_bubble b[DSP_MAX_BUBBLES];
    dsp_lp1 hp, lp, wob;
} dsp_stream;
void dsp_stream_init(dsp_stream *s, double rate);
/* f0: 0 = draw around the layer's pitch; amp: 0 = draw; decay_ms: 0 = draw (10..40 ms);
 * chirp: per-sample frequency ratio - 1 (0 = draw), e.g. 0.0003 rises audibly */
void dsp_stream_spawn(dsp_stream *s, double f0, double amp, double pan, double decay_ms, double chirp);
void dsp_stream_run(dsp_stream *s, double *l, double *r);

/* ---- birds: a two-level point process of gliding, trilling chirps ---- */
typedef struct {
    int active, chirping, chirps_left;
    double gap, ph, f0, f, glide, trill_m, trill_f, trill_ph;
    double ed, dd, ea, da, amp, dur, t;
} dsp_birdv;
typedef struct {
    dsp_rng rng; double rate;
    double songs_per_10s, pitch, ambience;
    dsp_birdv v[DSP_MAX_BIRDS];
    dsp_lp1 amb_hp, amb_lp;
} dsp_birds;
void   dsp_birds_init(dsp_birds *s, double rate);
double dsp_birds_run(dsp_birds *s);

/* ---- mixer ---- */
typedef enum { DSP_WHITE, DSP_PINK, DSP_BROWN, DSP_DEEP, DSP_RAIN, DSP_SEA, DSP_WIND, DSP_STREAM, DSP_BIRDS, DSP_NUM } dsp_kind;
extern const char *const dsp_kind_names[DSP_NUM];

typedef struct {
    double rate;
    double gain[DSP_NUM];   /* per layer; 0 = not run at all */
    dsp_white white; dsp_pink pink; dsp_brown brown; dsp_deep deep;
    dsp_rain rain; dsp_sea sea; dsp_wind wind; dsp_stream stream; dsp_birds birds;
} dsp_mixer;
void dsp_mixer_init(dsp_mixer *m, double rate);
void dsp_mixer_run(dsp_mixer *m, double *l, double *r);           /* one stereo sample */
void dsp_mixer_render(dsp_mixer *m, float *interleaved, int frames, double gain);   /* stereo frames, clamped */

/* ---- the original single-generator interface, on a global mixer (the web UI uses it) ---- */
void   dsp_init(double rate);
void   dsp_set_sound(int i);
void   dsp_set_param(int i, double v);
double dsp_get_param(int i);
int    dsp_num_params(void);
float *dsp_get_buf(void);
void   dsp_render(int n);

#endif
