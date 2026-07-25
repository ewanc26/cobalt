#pragma once

/*
 * Per-surface rendering context.
 *
 * One of these exists for the TV and one for the GamePad. They own their own
 * SDL_Renderer, their own font sizes (AGENTS.md §5: the GamePad gets its own
 * type scale, not a scaled TV layout) and their own cached textures.
 *
 * AGENTS.md §9 asks for no per-frame allocation in the render loop. Naively
 * calling TTF_RenderUTF8_Blended every frame allocates a surface and uploads a
 * texture for every visible string, every frame, so text goes through an LRU
 * texture cache instead. Backgrounds and tile corners are likewise baked into
 * textures once at startup rather than being drawn line-by-line.
 */

#include "ui/theme.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_FONT_TITLE = 0,
   COBALT_FONT_HEADING,
   COBALT_FONT_BODY,
   COBALT_FONT_CAPTION,
   COBALT_FONT_COUNT,
} cobalt_font_id;

typedef struct cobalt_render cobalt_render;

/*
 * Creates the window and renderer for the given surface.
 *
 * `font_path` is the TTF to load; if it cannot be opened the context still
 * comes up, drawing shapes but silently skipping text, so a missing font
 * degrades to a usable-but-wrong screen rather than a failure to boot.
 *
 * `prevent_swap` maps to SDL_WINDOW_WIIU_PREVENT_SWAP. Presenting swaps both
 * of the Wii U's scanbuffers, so when two surfaces are live exactly one of
 * them must swap or the app swaps twice per frame. Convention here: the TV is
 * created with prevent_swap = true and the GamePad, presented last, performs
 * the single swap for both. A lone surface must always have it false.
 */
cobalt_render *cobalt_render_create(cobalt_surface_id surface, const char *font_path,
                                    bool prevent_swap);
void cobalt_render_destroy(cobalt_render *r);

const cobalt_metrics *cobalt_render_metrics(const cobalt_render *r);
bool cobalt_render_has_font(const cobalt_render *r);

/* Clear to the background gradient. */
void cobalt_render_begin(cobalt_render *r);

/* Present the frame. See the swap note on cobalt_render_create(). */
void cobalt_render_end(cobalt_render *r);

/* --- primitives --- */

void cobalt_fill_rect(cobalt_render *r, const SDL_Rect *rect, SDL_Color colour);
void cobalt_fill_rounded_rect(cobalt_render *r, const SDL_Rect *rect, int radius,
                              SDL_Color colour);

/*
 * A Wii U menu style tile: rounded, light, with a soft drop shadow and a top
 * sheen. `focus` is 0..1 and drives the highlight and lift (AGENTS.md §5 asks
 * for tiles that respond to focus rather than static flat cards).
 */
void cobalt_draw_tile(cobalt_render *r, const SDL_Rect *rect, float focus);

/* --- text --- */

/* Returns the drawn width, or 0 if there is no font. */
int cobalt_draw_text(cobalt_render *r, cobalt_font_id font, const char *utf8,
                     int x, int y, SDL_Color colour);

/* Draws centred horizontally within [x, x + width). */
int cobalt_draw_text_centred(cobalt_render *r, cobalt_font_id font, const char *utf8,
                             int x, int y, int width, SDL_Color colour);

/*
 * Word-wraps to `max_width` and draws up to `max_lines` lines, appending an
 * ellipsis if the text did not fit. Returns the height consumed.
 * Post text is UTF-8 and may contain any script; wrapping is byte-safe and
 * never splits a multi-byte sequence.
 */
int cobalt_draw_text_wrapped(cobalt_render *r, cobalt_font_id font, const char *utf8,
                             int x, int y, int max_width, int max_lines,
                             SDL_Color colour);

void cobalt_text_size(cobalt_render *r, cobalt_font_id font, const char *utf8,
                      int *out_w, int *out_h);

int cobalt_font_line_height(cobalt_render *r, cobalt_font_id font);

/* Drop cached text textures. Call when switching screens to bound memory. */
void cobalt_render_flush_text_cache(cobalt_render *r);

#ifdef __cplusplus
}
#endif
