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

/* Palette: Wii U menu greys and whites with a cobalt-blue accent. The values
 * are runtime state rather than constants because the Settings screen can
 * switch between the light Wii U look, a dark one and a high-contrast one. */
typedef enum {
   COBALT_THEME_LIGHT = 0,
   COBALT_THEME_DARK,
} cobalt_theme;

/*
 * Percentages, not point sizes: the two surfaces have their own body size and
 * the setting means the same amount of change on both. NORMAL is the size the
 * cards already used, so an untouched install looks exactly as it did.
 */
typedef enum {
   COBALT_TEXT_SCALE_SMALL = 100,
   COBALT_TEXT_SCALE_NORMAL = 115,
   COBALT_TEXT_SCALE_LARGE = 130,
} cobalt_text_scale;

extern SDL_Color COBALT_COLOUR_BG_TOP;
extern SDL_Color COBALT_COLOUR_BG_BOTTOM;
extern SDL_Color COBALT_COLOUR_BAND_TOP;
extern SDL_Color COBALT_COLOUR_BAND_BOTTOM;
extern SDL_Color COBALT_COLOUR_TILE;
extern SDL_Color COBALT_COLOUR_TILE_EDGE;
extern SDL_Color COBALT_COLOUR_TEXT;
extern SDL_Color COBALT_COLOUR_TEXT_DIM;
/* Page titles sit on the header band, which stays dark in every palette, so
 * they are deliberately not TILE (which is the card surface). */
extern SDL_Color COBALT_COLOUR_HEADER_TEXT;
extern SDL_Color COBALT_COLOUR_ACCENT;
/* Darker blue for text on white; ACCENT itself is too light to read at caption size. */
extern SDL_Color COBALT_COLOUR_ACCENT_TEXT;
/* Text placed on an ACCENT fill (a control prompt's key chip). Dark themes use
 * a light accent, so this cannot simply be white. */
extern SDL_Color COBALT_COLOUR_ON_ACCENT;
extern SDL_Color COBALT_COLOUR_ERROR;
/* The control-prompt pill is a translucent surface, not a tile: it sits over
 * the footer and has to stay legible against either background. */
extern SDL_Color COBALT_COLOUR_PILL;
/* Small read-only metadata chips (counts, domains) inside a tile. */
extern SDL_Color COBALT_COLOUR_CHIP;

/* Switch the palette. The app calls this at startup from the saved setting and
 * immediately when the Settings screen toggles it, before the next frame.
 * High contrast is a separate switch and takes precedence over the theme. */
void cobalt_theme_apply(cobalt_theme theme);
cobalt_theme cobalt_theme_current(void);

/* Accessibility override: black ground, white text, brighter accents. It
 * applies to whichever theme is selected and is saved independently. */
void cobalt_theme_set_high_contrast(bool on);
bool cobalt_theme_high_contrast(void);

/* Bumped on every palette change. Renderers cache the background gradient and
 * header band textures, so they use this to know when those caches are stale. */
unsigned cobalt_theme_generation(void);

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
