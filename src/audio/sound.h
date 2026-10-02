#pragma once

/*
 * UI sound effects. SDL2_mixer is not in the Wii U toolchain, so the sounds are
 * synthesised once at start-up into small PCM buffers and mixed in a plain SDL
 * audio callback. Everything here is a harmless no-op until cobalt_sound_init()
 * succeeds, so the host tests and a console with no usable audio stay silent
 * rather than failing.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_SFX_MOVE = 0,
   COBALT_SFX_SELECT,
   COBALT_SFX_BACK,
   COBALT_SFX_LIKE,
   COBALT_SFX_REPOST,
   COBALT_SFX_SENT,
   COBALT_SFX_NOTICE,
   COBALT_SFX_COUNT,
} cobalt_sfx;

bool cobalt_sound_init(void);
void cobalt_sound_shutdown(void);
void cobalt_sound_play(cobalt_sfx sfx);

#ifdef __cplusplus
}
#endif
