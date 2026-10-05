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
   COBALT_FONT_ICON,       /* Phosphor Regular subset (icons.ttf) */
   COBALT_FONT_ICON_FILL,  /* Phosphor Fill subset (icons-fill.ttf) */
   COBALT_FONT_COUNT,
} cobalt_font_id;

/* Phosphor Icons codepoints (UTF-8), drawn with COBALT_FONT_ICON or
 * COBALT_FONT_ICON_FILL. */
#define COBALT_ICON_LIKE "\xEE\x8A\xA8"
#define COBALT_ICON_REPOST "\xEE\x8F\xB6"
#define COBALT_ICON_REPLY "\xEE\x85\xA8"
#define COBALT_ICON_BELL "\xEE\x83\x8E"
#define COBALT_ICON_SEARCH "\xEE\x8C\x8C"
#define COBALT_ICON_USER "\xEE\x93\x82"
#define COBALT_ICON_GEAR "\xEE\x89\xB0"
#define COBALT_ICON_HOUSE "\xEE\x8B\x82"
#define COBALT_ICON_LIST "\xEE\x8B\xB0"
#define COBALT_ICON_PENCIL "\xEE\x8E\xB4"
#define COBALT_ICON_IMAGE "\xEE\x8B\x8A"
#define COBALT_ICON_HASH "\xEE\x8A\xA2"
#define COBALT_ICON_BACK "\xEE\x81\x98"
#define COBALT_ICON_PIN "\xEE\x8F\xA2"
#define COBALT_ICON_USERS "\xEE\x93\x96"
#define COBALT_ICON_OFFLINE "\xEE\x93\xB2"
#define COBALT_ICON_WARNING "\xEE\x93\xA0"

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

#ifdef COBALT_E2E_HOST
SDL_Renderer *cobalt_render_sdl_renderer(cobalt_render *r);
#endif

/* Clear to the background gradient. */
void cobalt_render_begin(cobalt_render *r);

/* Present the frame. See the swap note on cobalt_render_create(). */
void cobalt_render_end(cobalt_render *r);

/* --- primitives --- */

void cobalt_fill_rect(cobalt_render *r, const SDL_Rect *rect, SDL_Color colour);
void cobalt_fill_rounded_rect(cobalt_render *r, const SDL_Rect *rect, int radius,
                              SDL_Color colour);

/*
 * Control prompts as a row of pills along the bottom edge. `spec` is a list of
 * "Key: label" entries separated by two or more spaces; an entry without a
 * colon is drawn as a plain pill. Rows wrap upward if they run out of width.
 */
void cobalt_draw_hints(cobalt_render *r, const char *spec);

/* A single control-prompt pill ("key" may be NULL/empty for a plain one), and
 * its size, for callers that place pills themselves (the header Back pill). */
int cobalt_pill_height(cobalt_render *r);
int cobalt_pill_width(cobalt_render *r, const char *key, const char *label);
void cobalt_draw_pill(cobalt_render *r, const char *key, const char *label, int x, int y);

/*
 * A Wii U menu style tile: rounded, light, with a soft drop shadow and a top
 * sheen. `focus` is 0..1 and drives the highlight and lift (AGENTS.md §5 asks
 * for tiles that respond to focus rather than static flat cards).
 */
void cobalt_draw_tile(cobalt_render *r, const SDL_Rect *rect, float focus);

/*
 * Draw a texture this context owns into `dst`.
 *
 * For textures made outside render.c — see ui/imagecache.h. Callers pass the
 * texture rather than the renderer so that nothing outside this file holds an
 * SDL_Renderer: the Wii U backend has no internal locking of any kind, so a
 * stray draw from a worker thread would corrupt the GX2 command buffer rather
 * than merely tear.
 */
void cobalt_draw_texture(cobalt_render *r, SDL_Texture *texture,
                         const SDL_Rect *dst);

/*
 * Create a texture on this context's renderer. Main thread only, for the same
 * reason. Returns NULL if `r` has no renderer.
 */
SDL_Texture *cobalt_render_upload(cobalt_render *r, SDL_Surface *surface);

/* --- images --- */

/*
 * The avatar cache this context draws from, or NULL if images are off.
 *
 * It hangs off the render context rather than being threaded through every
 * drawing call because that is where it actually belongs: a texture is owned by
 * one renderer and cannot be handed to the other, so a per-surface cache is the
 * only arrangement that is correct. Ownership stays with the caller —
 * cobalt_render_destroy() does not free it.
 */
typedef struct cobalt_imagecache cobalt_imagecache;

void cobalt_render_set_images(cobalt_render *r, cobalt_imagecache *cache);
cobalt_imagecache *cobalt_render_images(cobalt_render *r);

/*
 * The post-thumbnail cache this context draws from, or NULL if images are
 * off. Deliberately separate from the avatar cache above rather than a second
 * user of it: cobalt_imagecache bakes both the fit (circle vs. contain) and
 * the decode size into the cache instance itself, and a post image wants a
 * larger, aspect-preserving decode where an avatar wants a small masked
 * circle. Same ownership rules as cobalt_render_set_images().
 */
void cobalt_render_set_thumbs(cobalt_render *r, cobalt_imagecache *cache);
cobalt_imagecache *cobalt_render_thumbs(cobalt_render *r);

/*
 * The full-size viewer cache this context draws from, or NULL if images are
 * off. Separate from the thumbnail cache for the same reason it is separate
 * from the avatar cache: the decode size is baked into the cache instance,
 * and a viewer image is decoded at the surface's full height (720 on the TV,
 * 480 on the GamePad) where a card thumbnail is capped at 320. Two entries
 * and one loader rather than twenty-four and three, because a person looks
 * at one image at a time — see COBALT_IMAGECACHE_VIEWER_ENTRIES.
 */
void cobalt_render_set_viewer(cobalt_render *r, cobalt_imagecache *cache);
cobalt_imagecache *cobalt_render_viewer(cobalt_render *r);

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

/* As cobalt_draw_text_wrapped, but skips the first `first_line` wrapped lines,
 * for scrolling through a long post. */
int cobalt_draw_text_wrapped_from(cobalt_render *r, cobalt_font_id font, const char *utf8,
                                  int x, int y, int max_width, int first_line,
                                  int max_lines, SDL_Color colour);

/* A coloured byte range of the text being drawn (rich text facets). */
typedef struct {
   int start;
   int end;
   SDL_Color colour;
   bool underline;
} cobalt_text_span;

/* As cobalt_draw_text_wrapped_from, colouring the byte ranges in `spans`. */
int cobalt_draw_text_wrapped_spans(cobalt_render *r, cobalt_font_id font,
                                   const char *utf8, int x, int y, int max_width,
                                   int first_line, int max_lines, SDL_Color colour,
                                   const cobalt_text_span *spans, int span_count);

/* How many lines `utf8` wraps to at `max_width` (no truncation). */
int cobalt_text_wrapped_lines(cobalt_render *r, cobalt_font_id font,
                              const char *utf8, int max_width);

/* Bottom edge of the blue title band, and where screen content may start. */
int cobalt_header_height(cobalt_render *r);
int cobalt_content_top(cobalt_render *r);

void cobalt_text_size(cobalt_render *r, cobalt_font_id font, const char *utf8,
                      int *out_w, int *out_h);

int cobalt_font_line_height(cobalt_render *r, cobalt_font_id font);

/* Drop cached text textures. Call when switching screens to bound memory. */
void cobalt_render_flush_text_cache(cobalt_render *r);

#ifdef __cplusplus
}
#endif
