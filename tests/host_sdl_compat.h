#pragma once

/*
 * Force-included into every host compile (see the Makefile's -include).
 *
 * The Wii U SDL2 port (GaryOderNichts/SDL_mirror) adds window flags upstream
 * SDL2 does not have, and Cobalt's renderer depends on them: one surface per
 * physical output, with exactly one of the two performing the scanbuffer swap
 * (AGENTS.md §5, and the note on cobalt_render_create).
 *
 * Defining them here lets the host compiler check the rest of ui/render.c —
 * which is most of it — instead of the file being dropped from the sweep. The
 * values are meaningless off-console; nothing in the host tests creates a
 * window, and the real flags come from the port's own SDL_video.h when
 * building for hardware.
 */

#include <SDL_version.h>

#ifndef SDL_WINDOW_WIIU_TV_ONLY
#define SDL_WINDOW_WIIU_TV_ONLY      0x00000000u
#define SDL_WINDOW_WIIU_GAMEPAD_ONLY 0x00000000u
#define SDL_WINDOW_WIIU_PREVENT_SWAP 0x00000000u
#define COBALT_HOST_SDL_SHIM 1
#endif
