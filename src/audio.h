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

/* audio.h — sound from the simulation, on the noise-suite synthesis core.
 *
 * Every drop the simulation makes is also spawned in the audio: as a bubble
 * "plink" at the Minnaert frequency of the bubble a drop of that size
 * entrains, panned to where it landed relative to the camera, plus a noise
 * burst for the splash itself.  Rain adds its hiss bed; the breeze plays the
 * wind layer and reads its gust envelope back so gusts you hear roughen the
 * water; on a big basin the sea layer follows the surface roughness.
 */
#ifndef POND_AUDIO_H
#define POND_AUDIO_H

typedef struct audio audio;

audio *audio_open(void);                         /* NULL if no device; everything then no-ops */
void   audio_close(audio *a);

void   audio_set_volume(audio *a, double v);     /* 0..1 */
double audio_volume(const audio *a);
void   audio_set_mute(audio *a, int mute);
int    audio_muted(const audio *a);

/* the mix, tunable at run time: multipliers on the built-in levels (1 = as designed) */
typedef enum { SND_DROPS, SND_BED, SND_BROWN, SND_BREEZE, SND_HARSH, SND_NUM } snd_knob;
extern const char *const snd_knob_names[SND_NUM];
void   audio_set_knob(audio *a, snd_knob k, double v);
double audio_knob(const audio *a, snd_knob k);

/* continuous layers, levels 0..1; set each frame */
void   audio_set_rain(audio *a, double level);
void   audio_set_wind(audio *a, double level);
void   audio_set_sea(audio *a, double level, double env);
double audio_gust(const audio *a);               /* wind gust envelope, 0..1 */

/* a drop of crater radius `size_m` landing; pan -1..1, att 0..1 distance attenuation */
void   audio_splash(audio *a, double size_m, double pan, double att);

#endif
