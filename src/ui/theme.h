#pragma once

/*
 * Visual language.
 *
 * AGENTS.md §5 is specific about this: Cobalt should read as "a Wii U app that
 * happens to show Bluesky content", not as the Bluesky website in a console
 * wrapper. That means the Wii U menu's blues/whites/light-greys, rounded and
 * slightly glassy tiles rather than flat rectangles, and depth cues rather
 * than a flat mobile-app look.
 *
 * The two screens are NOT the same layout at different sizes. The GamePad is
 * a fixed 854x480 held at arm's length; the TV is 1280x720 viewed from across
 * a room. Each gets its own type scale and spacing (AGENTS.md §5), which is
 * why metrics are a per-surface struct rather than a set of globals.
 */

#include <SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which physical output a surface is being drawn for. */
typedef enum {
   COBALT_SURFACE_TV = 0,
   COBALT_SURFACE_DRC,
   COBALT_SURFACE_COUNT,
} cobalt_surface_id;

#define COBALT_TV_WIDTH   1280
#define COBALT_TV_HEIGHT   720
#define COBALT_DRC_WIDTH   854
#define COBALT_DRC_HEIGHT  480

/* Palette. Deliberately Wii U menu blues rather than Bluesky brand blue. */
extern const SDL_Color COBALT_COLOUR_BG_TOP;
extern const SDL_Color COBALT_COLOUR_BG_BOTTOM;
extern const SDL_Color COBALT_COLOUR_TILE;
extern const SDL_Color COBALT_COLOUR_TILE_FOCUS;
extern const SDL_Color COBALT_COLOUR_TILE_EDGE;
extern const SDL_Color COBALT_COLOUR_TEXT;
extern const SDL_Color COBALT_COLOUR_TEXT_DIM;
extern const SDL_Color COBALT_COLOUR_ACCENT;
extern const SDL_Color COBALT_COLOUR_ERROR;

/* Per-surface type scale and spacing. */
typedef struct {
   int width;
   int height;

   int font_title;
   int font_heading;
   int font_body;
   int font_caption;

   int pad_edge;    /* screen margin */
   int pad_tile;    /* padding inside a tile */
   int gap;         /* gap between tiles */
   int tile_radius;
   int line_gap;    /* extra leading between wrapped body lines */
} cobalt_metrics;

const cobalt_metrics *cobalt_metrics_for(cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
