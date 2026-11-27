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

#include "audio.h"
#include "dsp.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The mixer runs in SDL's audio thread; the main thread changes it only under
 * SDL_LockAudioDevice, which is brief.  (Under Emscripten there is one thread
 * and the lock is a no-op.) */
struct audio {
    SDL_AudioDeviceID dev;
    dsp_mixer mix;
    double rate, volume;
    int mute;
    double rain_level, wind_level, sea_level;
    double knob[SND_NUM];
    double gust;
    /* the whitewater: the target the main thread sets, the level the audio thread has
     * eased to, where it is; three noises (shared, left, right), their band filters,
     * and the slow surge */
    double brk_target, brk_level, brk_pan, brk_att, brk_width;
    dsp_rng brk_rng;
    dsp_lp1 bk_lo[3], bk_hi[3], bk_surge;
    double bk_c_lo, bk_c_hi, bk_c_surge, bk_up, bk_down;
    FILE *wav; long wav_frames;     /* POND_WAV=file: record what is played (float32 stereo WAV) */
};

/* offline (no device, for tests) there is no audio thread to keep out */
static void lock(audio *a)   { if (a->dev) SDL_LockAudioDevice(a->dev); }
static void unlock(audio *a) { if (a->dev) SDL_UnlockAudioDevice(a->dev); }

static void wav_header(FILE *f, int rate, long frames)
{
    uint32_t data = (uint32_t)(frames * 8), riff = 36 + data;
    uint16_t fmt = 3, ch = 2, bits = 32, align = 8;
    uint32_t fmtlen = 16, srate = (uint32_t)rate, brate = (uint32_t)rate * 8;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtlen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&srate, 4, 1, f);
    fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

/* the whitewater's hiss, added to what the mixer rendered: noise band-passed to
 * 500 Hz .. 6 kHz (the rush of a spilling crest), its loudness surging by +-50 % at
 * a couple of hertz, and spread from a point at `pan` to fully decorrelated left and
 * right as the breaking spreads.  Eased in over ~40 ms, out over ~0.6 s, so it swells
 * with a breaker and trails off after it, as the foam does. */
static void render_break(audio *a, float *out, int frames, double gain)
{
    if (a->brk_target <= 0.0 && a->brk_level < 1e-4) { a->brk_level = 0.0; return; }
    const double p = a->brk_pan < -1 ? -1 : (a->brk_pan > 1 ? 1 : a->brk_pan);
    const double th = (p + 1.0) * 0.25 * 3.14159265358979;
    const double gl = cos(th) * 1.41421356, gr = sin(th) * 1.41421356;
    const double w = a->brk_width < 0 ? 0 : (a->brk_width > 1 ? 1 : a->brk_width);
    const double cs = sqrt(1.0 - w), ds = sqrt(w);
    const double k = 0.30 * a->knob[SND_BREAK] * a->brk_att * gain;
    for (int f = 0; f < frames; f++) {
        a->brk_level += (a->brk_target - a->brk_level) * (a->brk_target > a->brk_level ? a->bk_up : a->bk_down);
        double n[3];
        for (int c = 0; c < 3; c++) {
            const double lo = dsp_lp1_run(&a->bk_lo[c], dsp_rng_white(&a->brk_rng), a->bk_c_lo);   /* below 6 kHz */
            n[c] = lo - dsp_lp1_run(&a->bk_hi[c], lo, a->bk_c_hi);                                 /* above 500 Hz */
        }
        double s = dsp_lp1_run(&a->bk_surge, dsp_rng_white(&a->brk_rng), a->bk_c_surge) * 25.0;
        s = 1.0 + 0.5 * (s < -1 ? -1 : (s > 1 ? 1 : s));
        const double g = k * a->brk_level * s;
        float *o = out + 2 * f;
        double l = o[0] + g * gl * (cs * n[0] + ds * n[1]);
        double r = o[1] + g * gr * (cs * n[0] + ds * n[2]);
        o[0] = (float)(l < -1 ? -1 : (l > 1 ? 1 : l));
        o[1] = (float)(r < -1 ? -1 : (r > 1 ? 1 : r));
    }
}

static void callback(void *ud, Uint8 *stream, int len)
{
    audio *a = ud;
    const int frames = len / (int)(2 * sizeof(float));
    /* pond's sources are sparse point events; the suite's 0.35 headroom is for its loud
     * continuous generators and would leave this too quiet */
    const double gain = a->mute ? 0.0 : a->volume * 0.8;
    dsp_mixer_render(&a->mix, (float *)stream, frames, gain);
    render_break(a, (float *)stream, frames, gain);
    a->gust = a->mix.wind.gust;
    if (a->wav) { fwrite(stream, 1, (size_t)len, a->wav); a->wav_frames += frames; }
}

const char *const snd_knob_names[SND_NUM] = { "drops", "bed", "brown", "breeze", "harsh", "breaking" };

static void apply_levels(audio *a)
{
    dsp_mixer *m = &a->mix;
    const double *k = a->knob;
    m->gain[DSP_RAIN]   = 1.0;                         /* always on: it carries the splashes */
    m->rain.drop_level  = 1.6 * k[SND_DROPS];          /* the ticks and splashes of visible drops */
    m->stream.bubble_level = 0.9 * k[SND_DROPS];       /* their plinks */
    /* the bed: the crackle of drops too small to see, ~200 per visible drop, and only a
     * faint continuous hiss underneath for the heaviest rain */
    m->rain.grain_rate  = 250.0 * a->rain_level;
    m->rain.grain_level = 0.9 * k[SND_BED];
    m->rain.bed_level   = 0.05 * a->rain_level * sqrt(a->rain_level) * k[SND_BED];   /* the bed grows with the rain, the grains with it */
    m->gain[DSP_STREAM] = 1.0;                         /* bubbles only; no flow bed */
    m->gain[DSP_BROWN]  = 0.25 * k[SND_BROWN];         /* a little room tone, off by default */
    /* the breeze: a sea breeze, not a forest gale.  At harsh = 0: a steady, soft,
     * mid-high hiss with no leaf rustle and no tremor; harsh = 1 is the suite's wind -
     * gusty, fluttering, low-bodied, leaves rustling */
    double h = k[SND_HARSH];
    m->wind.gustiness   = 0.05 + 0.55 * h;
    m->wind.rustle      = 0.60 * h;
    m->wind.tone        = 550.0 - 300.0 * h;
    m->wind.flutter     = 0.15 + 0.85 * h;
    m->gain[DSP_WIND]   = 0.45 * k[SND_BREEZE] * a->wind_level;
    m->gain[DSP_SEA]    = 0.6 * a->sea_level;
}

void audio_set_knob(audio *a, snd_knob k, double v)
{
    if (!a || k < 0 || k >= SND_NUM) return;
    if (v < 0) v = 0;
    if (v > 4) v = 4;
    lock(a);
    a->knob[k] = v; apply_levels(a);
    unlock(a);
}
double audio_knob(const audio *a, snd_knob k) { return (a && k >= 0 && k < SND_NUM) ? a->knob[k] : 0.0; }

static void init_state(audio *a, double rate)
{
    a->rate = rate;
    a->volume = 0.7;
    dsp_mixer_init(&a->mix, a->rate);
    a->mix.rain.spawn_rate = 0.0;      /* drops come from the simulation */
    a->mix.rain.drop_level = 1.6;
    a->mix.rain.bed_level = 0.0;
    a->mix.stream.spawn_rate = 0.0;
    a->mix.stream.flow_level = 0.0;
    a->mix.stream.bubble_level = 0.9;
    a->mix.sea.ext_env = 0.0;
    a->knob[SND_DROPS] = 1.0; a->knob[SND_BED] = 1.0; a->knob[SND_BROWN] = 0.0;
    a->knob[SND_BREEZE] = 1.0; a->knob[SND_HARSH] = 0.15; a->knob[SND_BREAK] = 1.0;
    a->brk_rng.s = 0x5DEECE66DULL;
    a->bk_c_lo = dsp_lp_coef(6000.0, a->rate);
    a->bk_c_hi = dsp_lp_coef(500.0, a->rate);
    a->bk_c_surge = dsp_lp_coef(2.0, a->rate);
    a->bk_up = 1.0 - exp(-1.0 / (0.040 * a->rate));
    a->bk_down = 1.0 - exp(-1.0 / (0.600 * a->rate));
    a->brk_att = 1.0;
    apply_levels(a);
}

audio *audio_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return NULL;
    audio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024;
    want.callback = callback; want.userdata = a;
    /* the state is set up before the device starts calling back: it starts paused */
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!a->dev) { free(a); return NULL; }
    init_state(a, have.freq);
    if (getenv("POND_WAV")) { a->wav = fopen(getenv("POND_WAV"), "wb"); if (a->wav) wav_header(a->wav, (int)a->rate, 0); }
    SDL_PauseAudioDevice(a->dev, 0);
    return a;
}

audio *audio_open_offline(double rate)
{
    audio *a = calloc(1, sizeof *a);
    if (a) init_state(a, rate);
    return a;
}

void audio_render(audio *a, float *out, int frames)
{
    if (a) callback(a, (Uint8 *)out, frames * (int)(2 * sizeof(float)));
}

void audio_close(audio *a)
{
    if (!a) return;
    if (a->dev) SDL_CloseAudioDevice(a->dev);
    if (a->wav) { wav_header(a->wav, (int)a->rate, a->wav_frames); fclose(a->wav); }
    free(a);
}

void audio_set_volume(audio *a, double v) { if (a) a->volume = v < 0 ? 0 : (v > 1 ? 1 : v); }
double audio_volume(const audio *a) { return a ? a->volume : 0.0; }
void audio_set_mute(audio *a, int mute) { if (a) a->mute = mute; }
int audio_muted(const audio *a) { return a ? a->mute : 1; }
double audio_gust(const audio *a) { return a ? a->gust : 0.5; }

void audio_set_rain(audio *a, double level)
{
    if (!a || a->rain_level == level) return;
    lock(a);
    a->rain_level = level; apply_levels(a);
    unlock(a);
}
void audio_set_wind(audio *a, double level)
{
    if (!a || a->wind_level == level) return;
    lock(a);
    a->wind_level = level; apply_levels(a);
    unlock(a);
}
void audio_set_sea(audio *a, double level, double env)
{
    if (!a) return;
    lock(a);
    a->sea_level = level; a->mix.sea.ext_env = env < 0 ? 0 : (env > 1 ? 1 : env); apply_levels(a);
    unlock(a);
}

void audio_set_break(audio *a, double level, double pan, double att, double width, double dt)
{
    if (!a) return;
    level = level < 0 ? 0 : (level > 1 ? 1 : level);
    lock(a);
    a->brk_target = level; a->brk_pan = pan; a->brk_att = att; a->brk_width = width;
    /* the crackle: bubbles bursting in the foam, a few milliseconds of bright noise
     * each, up to ~150 a second, scattered across the breaking's width */
    if (level > 0.0 && dt > 0.0 && a->knob[SND_BREAK] > 0.0) {
        dsp_rng *rg = &a->brk_rng;
        double lam = 150.0 * level * dt, q = exp(-lam), u = dsp_rng_uniform(rg);
        int n = 0;
        while (u > q && n < 20) { u -= q; n++; q *= lam / n; }
        for (int i = 0; i < n; i++) {
            const double pp = pan + width * (2.0 * dsp_rng_uniform(rg) - 1.0);
            dsp_rain_spawn(&a->mix.rain, 0.035 * att * a->knob[SND_BREAK] * (0.4 + 0.6 * dsp_rng_uniform(rg)),
                           2500.0 + 4000.0 * dsp_rng_uniform(rg), pp < -1 ? -1 : (pp > 1 ? 1 : pp),
                           1.0 + 2.5 * dsp_rng_uniform(rg), 1000.0 * dt * dsp_rng_uniform(rg));
        }
    }
    unlock(a);
}

void audio_splash(audio *a, double size_m, double pan, double att)
{
    if (!a) return;
    dsp_rng *rg = &a->mix.stream.rng;
    /* The bubble a drop entrains: radius about a third of the crater's, Minnaert
     * f = 3.26 m/s / R, within the band that makes sense.  Its ring time is set by
     * its Q (20..40 for millimetre bubbles): tau = Q / (pi f), so a 3 kHz plink rings
     * a few milliseconds and a 500 Hz plonk twenty.  It pinches off under the free
     * surface and moves away from it, so the pitch glides up a little - more for a
     * big drop - and it does so only after the crater has retracted: a tick at
     * impact, a pause, then the plink (Pumphrey & Crum). */
    double Rb = 0.3 * size_m;
    double f = 3.26 / (Rb > 1e-4 ? Rb : 1e-4);
    if (f < 80.0) f = 80.0;
    if (f > 4000.0) f = 4000.0;
    double big = size_m / 0.005;                       /* 1 = a 5 mm crater, a fat raindrop */
    if (big > 20.0) big = 20.0;
    double Q = 20.0 + 20.0 * dsp_rng_uniform(rg);
    double decay_ms = 1000.0 * Q / (3.14159265 * f);
    double glide = 1.08 + 0.17 * (big / 20.0);
    double delay_ms = (15.0 + 10.0 * sqrt(big)) * (0.7 + 0.6 * dsp_rng_uniform(rg));
    double amp = att * (0.35 + 0.12 * sqrt(big));
    if (amp > 0.9) amp = 0.9;
    lock(a);
    /* the impact: a short, bright tick */
    dsp_rain_spawn(&a->mix.rain, att * (0.10 + 0.06 * sqrt(big)), 3000.0 + 1000.0 * dsp_rng_uniform(rg), pan, 1.5, 0.0);
    /* the bubble */
    dsp_stream_spawn(&a->mix.stream, f, amp, pan, decay_ms, glide, delay_ms);
    /* a big drop also throws water: a lower, longer splash right after the tick */
    if (big > 2.0) dsp_rain_spawn(&a->mix.rain, att * 0.06 * sqrt(big), 600.0 / sqrt(big), pan, 6.0 + 3.0 * sqrt(big), 2.0);
    unlock(a);
}
