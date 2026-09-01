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
    double gust;
    FILE *wav; long wav_frames;     /* POND_WAV=file: record what is played (float32 stereo WAV) */
};

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

static void callback(void *ud, Uint8 *stream, int len)
{
    audio *a = ud;
    const int frames = len / (int)(2 * sizeof(float));
    dsp_mixer_render(&a->mix, (float *)stream, frames, a->mute ? 0.0 : a->volume * 0.35);
    a->gust = a->mix.wind.gust;
    if (a->wav) { fwrite(stream, 1, (size_t)len, a->wav); a->wav_frames += frames; }
}

static void apply_levels(audio *a)
{
    dsp_mixer *m = &a->mix;
    m->gain[DSP_RAIN]   = 1.0;                         /* always on: it carries the splashes */
    m->rain.bed_level   = 0.15 * a->rain_level;        /* the hiss follows the rain switch */
    m->gain[DSP_STREAM] = 1.0;                         /* bubbles only; no flow bed */
    m->gain[DSP_WIND]   = 0.8 * a->wind_level;
    m->gain[DSP_SEA]    = 0.6 * a->sea_level;
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
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!a->dev) { free(a); return NULL; }
    a->rate = have.freq;
    a->volume = 0.7;
    dsp_mixer_init(&a->mix, a->rate);
    a->mix.rain.spawn_rate = 0.0;      /* drops come from the simulation */
    a->mix.rain.drop_level = 1.6;
    a->mix.rain.bed_level = 0.0;
    a->mix.stream.spawn_rate = 0.0;
    a->mix.stream.flow_level = 0.0;
    a->mix.stream.bubble_level = 0.9;
    a->mix.sea.ext_env = 0.0;
    apply_levels(a);
    if (getenv("POND_WAV")) { a->wav = fopen(getenv("POND_WAV"), "wb"); if (a->wav) wav_header(a->wav, (int)a->rate, 0); }
    SDL_PauseAudioDevice(a->dev, 0);
    return a;
}

void audio_close(audio *a)
{
    if (!a) return;
    SDL_CloseAudioDevice(a->dev);
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
    SDL_LockAudioDevice(a->dev);
    a->rain_level = level; apply_levels(a);
    SDL_UnlockAudioDevice(a->dev);
}
void audio_set_wind(audio *a, double level)
{
    if (!a || a->wind_level == level) return;
    SDL_LockAudioDevice(a->dev);
    a->wind_level = level; apply_levels(a);
    SDL_UnlockAudioDevice(a->dev);
}
void audio_set_sea(audio *a, double level, double env)
{
    if (!a) return;
    SDL_LockAudioDevice(a->dev);
    a->sea_level = level; a->mix.sea.ext_env = env < 0 ? 0 : (env > 1 ? 1 : env); apply_levels(a);
    SDL_UnlockAudioDevice(a->dev);
}

void audio_splash(audio *a, double size_m, double pan, double att)
{
    if (!a) return;
    /* the entrained bubble: radius roughly a third of the crater's, Minnaert f = 3.26 m/s / R,
     * kept within the audible-and-sensible band; bigger bubbles ring longer and rise slower */
    double Rb = 0.3 * size_m;
    double f = 3.26 / (Rb > 1e-4 ? Rb : 1e-4);
    if (f < 80.0) f = 80.0;
    if (f > 4000.0) f = 4000.0;
    double big = size_m / 0.005;                       /* 1 = a 5 mm crater, a fat raindrop */
    if (big > 20.0) big = 20.0;
    double decay = 20.0 + 8.0 * big;                   /* ms: bigger bubbles ring longer */
    double chirp = 4.0e-4 / (1.0 + 0.5 * big);
    double amp = att * (0.30 + 0.12 * sqrt(big));
    if (amp > 0.9) amp = 0.9;
    SDL_LockAudioDevice(a->dev);
    dsp_stream_spawn(&a->mix.stream, f, amp, pan, decay, chirp);
    /* the splash: a noise burst, deeper and louder for a bigger drop */
    double tone = 600.0 / sqrt(big > 0.25 ? big : 0.25);
    dsp_rain_spawn(&a->mix.rain, att * (0.15 + 0.10 * sqrt(big)), tone, pan, 8.0 + 6.0 * sqrt(big));
    SDL_UnlockAudioDevice(a->dev);
}
