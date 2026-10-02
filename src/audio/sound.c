#include "audio/sound.h"
#include "util/log.h"

#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define VOICES 8
#define MASTER 0.5f

typedef struct {
   int16_t *pcm;   /* mono */
   int len;
} sample;

typedef struct {
   const sample *s;
   int pos;
} voice;

static sample samples[COBALT_SFX_COUNT];
static voice voices[VOICES];
static SDL_AudioDeviceID dev;

/* Append one soft-edged sine note; returns the new length. */
static int
add_note(float *buf, int at, int cap, float f0, float f1, int ms, float vol)
{
   const int n = RATE * ms / 1000;
   float phase = 0.0f;
   for (int i = 0; i < n && at + i < cap; i++) {
      const float t = (float) i / (float) n;
      const float f = f0 + (f1 - f0) * t;
      phase += 6.2831853f * f / RATE;
      const float attack = i < 96 ? (float) i / 96.0f : 1.0f;
      const float env = attack * (1.0f - t) * (1.0f - t);
      buf[at + i] += sinf(phase) * env * vol;
   }
   return at + n;
}

static void
build(cobalt_sfx id, const float (*notes)[4], int count)
{
   int cap = 0;
   for (int i = 0; i < count; i++) cap += RATE * (int) notes[i][2] / 1000;
   float *buf = (float *) calloc((size_t) cap, sizeof(float));
   int16_t *pcm = (int16_t *) malloc((size_t) cap * sizeof(int16_t));
   if (!buf || !pcm) {
      free(buf);
      free(pcm);
      return;
   }
   int at = 0;
   for (int i = 0; i < count; i++) {
      at = add_note(buf, at, cap, notes[i][0], notes[i][1], (int) notes[i][2],
                    notes[i][3]);
   }
   for (int i = 0; i < cap; i++) {
      float v = buf[i] * MASTER;
      if (v > 1.0f) v = 1.0f;
      if (v < -1.0f) v = -1.0f;
      pcm[i] = (int16_t) (v * 32000.0f);
   }
   free(buf);
   samples[id].pcm = pcm;
   samples[id].len = cap;
}

static void
synthesise(void)
{
   /* {start Hz, end Hz, ms, volume} */
   static const float move[][4] = { { 1500, 1300, 28, 0.35f } };
   static const float select[][4] = { { 660, 660, 60, 0.5f }, { 990, 990, 110, 0.5f } };
   static const float back[][4] = { { 880, 880, 55, 0.45f }, { 587, 587, 110, 0.45f } };
   static const float like[][4] = { { 784, 784, 70, 0.5f }, { 1175, 1319, 180, 0.5f } };
   static const float repost[][4] = { { 523, 523, 70, 0.5f }, { 659, 659, 70, 0.5f },
                                      { 784, 784, 130, 0.5f } };
   static const float sent[][4] = { { 523, 523, 80, 0.5f }, { 659, 659, 80, 0.5f },
                                    { 784, 784, 80, 0.5f }, { 1047, 1047, 220, 0.5f } };
   static const float notice[][4] = { { 740, 740, 80, 0.4f }, { 554, 554, 150, 0.4f } };
   build(COBALT_SFX_MOVE, move, 1);
   build(COBALT_SFX_SELECT, select, 2);
   build(COBALT_SFX_BACK, back, 2);
   build(COBALT_SFX_LIKE, like, 2);
   build(COBALT_SFX_REPOST, repost, 3);
   build(COBALT_SFX_SENT, sent, 4);
   build(COBALT_SFX_NOTICE, notice, 2);
}

static void SDLCALL
callback(void *user, Uint8 *stream, int bytes)
{
   (void) user;
   int16_t *out = (int16_t *) stream;
   const int frames = bytes / (int) (2 * sizeof(int16_t));
   memset(stream, 0, (size_t) bytes);
   for (int v = 0; v < VOICES; v++) {
      voice *vc = &voices[v];
      if (!vc->s) continue;
      for (int i = 0; i < frames && vc->pos < vc->s->len; i++, vc->pos++) {
         for (int c = 0; c < 2; c++) {
            int mixed = out[i * 2 + c] + vc->s->pcm[vc->pos];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out[i * 2 + c] = (int16_t) mixed;
         }
      }
      if (vc->pos >= vc->s->len) vc->s = NULL;
   }
}

bool
cobalt_sound_init(void)
{
   const char *mute = getenv("COBALT_MUTE");
   if (dev || (mute && mute[0])) {
      return false;
   }
   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      COBALT_LOGW("sound: audio subsystem unavailable: %s", SDL_GetError());
      return false;
   }
   SDL_AudioSpec want, have;
   SDL_zero(want);
   want.freq = RATE;
   want.format = AUDIO_S16SYS;
   want.channels = 2;
   want.samples = 1024;
   want.callback = callback;
   dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
   if (!dev) {
      COBALT_LOGW("sound: no audio device: %s", SDL_GetError());
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      return false;
   }
   if (have.format != AUDIO_S16SYS || have.channels != 2 || have.freq != RATE) {
      COBALT_LOGW("sound: unsupported device format (%d Hz, %d ch, 0x%x)",
                  have.freq, have.channels, (unsigned) have.format);
      SDL_CloseAudioDevice(dev);
      dev = 0;
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      return false;
   }
   synthesise();
   SDL_PauseAudioDevice(dev, 0);
   COBALT_LOGI("sound: up at %d Hz", have.freq);
   return true;
}

void
cobalt_sound_shutdown(void)
{
   if (!dev) {
      return;
   }
   SDL_CloseAudioDevice(dev);
   dev = 0;
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
   for (int i = 0; i < COBALT_SFX_COUNT; i++) {
      free(samples[i].pcm);
      samples[i].pcm = NULL;
      samples[i].len = 0;
   }
   memset(voices, 0, sizeof voices);
}

void
cobalt_sound_play(cobalt_sfx sfx)
{
   if (!dev || sfx < 0 || sfx >= COBALT_SFX_COUNT || !samples[sfx].pcm) {
      return;
   }
   SDL_LockAudioDevice(dev);
   for (int v = 0; v < VOICES; v++) {
      if (!voices[v].s) {
         voices[v].s = &samples[sfx];
         voices[v].pos = 0;
         break;
      }
   }
   SDL_UnlockAudioDevice(dev);
}
