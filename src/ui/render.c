#include "ui/render.h"
#include "input/input.h"
#include "util/log.h"

#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cache sized for a full timeline screen's worth of distinct strings plus the
 * chrome around it. Overflow evicts least-recently-used rather than failing. */
#define TEXT_CACHE_SIZE 128

/* Strings longer than this bypass the cache. Wrapped body lines are well under
 * it; the cap exists so the cache is a flat array with no per-entry malloc. */
#define TEXT_KEY_MAX 160

/* Room for a whole timeline screen's worth of laid-out strings, for the same
 * reason and with the same least-recently-used eviction. */
#define WRAP_CACHE_SIZE 12

/* Distinct pill widths on screen at once, with room for the next screen's. */
#define SHAPE_CACHE_SIZE 8

/* The longest string a layout entry keeps. COBALT_POST_TEXT_MAX is the longest
 * text Cobalt draws; a longer one (a long compose draft) is laid out and drawn
 * exactly as before, just not remembered. */
#define WRAP_TEXT_MAX 1024

/* Lines one entry can hold. The narrowest column any post text is laid out in
 * needs well under this for WRAP_TEXT_MAX bytes; a string that beats it is
 * drawn uncached rather than remembered with lines missing. */
#define WRAP_LINES_MAX 64

/* Vertical resolution of the baked background gradient. Stretched with linear
 * filtering, so a coarse ramp is indistinguishable from a per-scanline one. */
#define GRADIENT_STEPS 64

#define ELLIPSIS "..."

typedef struct {
   char text[TEXT_KEY_MAX];
   uint32_t hash;
   uint32_t colour;
   cobalt_font_id font;
   SDL_Texture *tex;
   int w;
   int h;
   uint32_t last_used;
   bool in_use;
} text_entry;

/*
 * Where one wrapped line starts and ends, as offsets into the string it was
 * laid out from. `next` is where the following line begins: past the spaces a
 * break landed on, and past a newline if the text's own line ended there. It is
 * always greater than `start`, so a walk over the text terminates.
 */
typedef struct {
   int start;
   int len;
   int next;
} wrap_line;

/*
 * The line breaks of one string at one width, kept between frames.
 *
 * The same string is wrapped more than once per frame: a card counts its lines
 * to size itself and then wraps the same text again to draw it, and the TV
 * timeline wraps the selected post a second time for the detail column. The
 * breaks depend only on the text, the font and the width, so they are worked
 * out once and kept — and the walk is the most expensive thing the renderer
 * does (see cobalt_render_text_work).
 *
 * The text itself is held, not just a hash of it: a stale entry would draw the
 * wrong words, and a hash is not allowed to decide that. The hash only skips
 * the comparison on an obvious miss.
 */
typedef struct {
   bool used;
   cobalt_font_id font;
   int max_width;
   uint32_t hash;
   uint32_t last_used;
   int line_count;
   char text[WRAP_TEXT_MAX];
   wrap_line lines[WRAP_LINES_MAX];
} wrap_entry;

/*
 * A rounded rect's coverage at one size, white with the same antialiased alpha
 * the corner tiles carry. Tinted and blitted it is one command where the
 * primitives are three fills and four copies — which is what the footer's pills
 * are worth: five prompts, sixteen commands each, every frame.
 */
typedef struct {
   bool used;
   int w, h, radius;
   uint32_t last_used;
   SDL_Texture *tex;
} shape_entry;

struct cobalt_render {
   cobalt_surface_id surface;
   const cobalt_metrics *m;
   /* See cobalt_render_set_role(). */
   cobalt_surface_role role;

   SDL_Window *window;
   SDL_Renderer *renderer;
   TTF_Font *fonts[COBALT_FONT_COUNT];
   /* Same sizes, from a wider-coverage face; NULL when none is available. */
   TTF_Font *fallbacks[COBALT_FONT_COUNT];

   SDL_Texture *gradient;
   SDL_Texture *band;
   int band_h;
   SDL_Texture *corner;
   int corner_radius;

   text_entry cache[TEXT_CACHE_SIZE];
   uint32_t clock;

   wrap_entry wraps[WRAP_CACHE_SIZE];
   uint32_t wrap_clock;

   shape_entry shapes[SHAPE_CACHE_SIZE];
   uint32_t shape_clock;

   /* Borrowed, not owned — see cobalt_render_set_images(). */
   cobalt_imagecache *images;
   /* Borrowed, not owned — see cobalt_render_set_thumbs(). */
   cobalt_imagecache *thumbs;
   /* Borrowed, not owned — see cobalt_render_set_viewer(). */
   cobalt_imagecache *viewer;

   bool warned_long_string;

   /* Bench counters, see cobalt_render_text_work. */
   uint64_t text_measures;
   uint64_t text_wrap_passes;
   uint64_t text_wrap_requests;
   /* Host builds only: nanoseconds spent in TTF_SizeUTF8. See measure_text. */
   uint64_t text_measure_ns;
};

/* --- helpers --- */
static void open_fallback_fonts(cobalt_render *r, const char *font_path);
static TTF_Font *open_font_cached(const char *path, int pt);

static uint32_t
pack_colour(SDL_Color c)
{
   return ((uint32_t) c.r << 24) | ((uint32_t) c.g << 16) |
          ((uint32_t) c.b << 8) | (uint32_t) c.a;
}

static uint32_t
hash_string(const char *s)
{
   /* FNV-1a; only used to skip obviously-different cache entries cheaply. */
   uint32_t h = 2166136261u;
   while (*s) {
      h ^= (unsigned char) *s++;
      h *= 16777619u;
   }
   return h;
}

static void
set_draw_colour(cobalt_render *r, SDL_Color c)
{
   SDL_SetRenderDrawColor(r->renderer, c.r, c.g, c.b, c.a);
}

static int
font_size_for(const cobalt_metrics *m, cobalt_font_id id)
{
   switch (id) {
      case COBALT_FONT_TITLE:   return m->font_title;
      case COBALT_FONT_HEADING: return m->font_heading;
      case COBALT_FONT_CAPTION: return m->font_caption;
      case COBALT_FONT_ICON:
      case COBALT_FONT_ICON_FILL: return m->font_caption + 2;
      case COBALT_FONT_BODY:
      default:                  return m->font_body;
   }
}

/* --- baked textures --- */

static SDL_Texture *
build_gradient(SDL_Renderer *renderer)
{
   SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC, 1, GRADIENT_STEPS);
   if (!tex) {
      COBALT_LOGE("gradient texture creation failed: %s", SDL_GetError());
      return NULL;
   }

   uint32_t pixels[GRADIENT_STEPS];
   for (int i = 0; i < GRADIENT_STEPS; i++) {
      float t = (float) i / (float) (GRADIENT_STEPS - 1);
      uint8_t rr = (uint8_t) (COBALT_COLOUR_BG_TOP.r +
                              t * (COBALT_COLOUR_BG_BOTTOM.r - COBALT_COLOUR_BG_TOP.r));
      uint8_t gg = (uint8_t) (COBALT_COLOUR_BG_TOP.g +
                              t * (COBALT_COLOUR_BG_BOTTOM.g - COBALT_COLOUR_BG_TOP.g));
      uint8_t bb = (uint8_t) (COBALT_COLOUR_BG_TOP.b +
                              t * (COBALT_COLOUR_BG_BOTTOM.b - COBALT_COLOUR_BG_TOP.b));
      pixels[i] = 0xFF000000u | ((uint32_t) rr << 16) | ((uint32_t) gg << 8) | bb;
   }

   SDL_UpdateTexture(tex, NULL, pixels, (int) sizeof(uint32_t));
   /* Linear filtering is what turns 64 steps into a smooth ramp. */
   SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
   /* Every texel is opaque and it is the first thing drawn each frame, so it
    * can be copied rather than blended — the widest copy of the frame. */
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
   return tex;
}

/*
 * A quarter disc, white, with antialiased edge coverage in the alpha channel.
 * Blitted four times with flips to round off a rectangle, and colour-modded to
 * whatever the caller is filling with — so one texture serves every rounded
 * shape at this surface's radius.
 */
static SDL_Texture *
build_corner(SDL_Renderer *renderer, int radius)
{
   if (radius <= 0) {
      return NULL;
   }

   SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC, radius, radius);
   if (!tex) {
      COBALT_LOGE("corner texture creation failed: %s", SDL_GetError());
      return NULL;
   }

   uint32_t *pixels = (uint32_t *) malloc((size_t) radius * (size_t) radius * sizeof(uint32_t));
   if (!pixels) {
      SDL_DestroyTexture(tex);
      return NULL;
   }

   const float rf = (float) radius;
   for (int y = 0; y < radius; y++) {
      for (int x = 0; x < radius; x++) {
         float dx = rf - ((float) x + 0.5f);
         float dy = rf - ((float) y + 0.5f);
         float dist = sqrtf(dx * dx + dy * dy);
         float cover = rf - dist + 0.5f;
         if (cover < 0.0f) cover = 0.0f;
         if (cover > 1.0f) cover = 1.0f;
         uint8_t a = (uint8_t) (cover * 255.0f + 0.5f);
         pixels[y * radius + x] = ((uint32_t) a << 24) | 0x00FFFFFFu;
      }
   }

   SDL_UpdateTexture(tex, NULL, pixels, radius * (int) sizeof(uint32_t));
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   free(pixels);
   return tex;
}

/*
 * A whole rounded rect's coverage as a texture: white, with the same alpha the
 * corner tiles carry. Tinted and blitted it draws what the three fills and four
 * corner copies draw, in one command instead of seven.
 *
 * The distance to the nearest corner's centre is what decides a pixel: along
 * the straight middle of the rect it is zero, so one formula covers the middle
 * and the caps alike, and it is the formula build_corner uses, so the two
 * shapes agree to the pixel.
 */
static SDL_Texture *
build_shape_mask(SDL_Renderer *renderer, int w, int h, int radius)
{
   SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC, w, h);
   if (!tex) {
      COBALT_LOGE("shape texture creation failed: %s", SDL_GetError());
      return NULL;
   }

   uint32_t *pixels = (uint32_t *) malloc((size_t) w * (size_t) h * sizeof(uint32_t));
   if (!pixels) {
      SDL_DestroyTexture(tex);
      return NULL;
   }

   const float rf = (float) radius;
   const float wf = (float) w;
   const float hf = (float) h;
   for (int y = 0; y < h; y++) {
      const float py = (float) y + 0.5f;
      float cy = py < rf ? rf : (py > hf - rf ? hf - rf : py);
      for (int x = 0; x < w; x++) {
         const float px = (float) x + 0.5f;
         const float cx = px < rf ? rf : (px > wf - rf ? wf - rf : px);
         const float dx = px - cx;
         const float dy = py - cy;
         const float dist = sqrtf(dx * dx + dy * dy);
         float cover = rf - dist + 0.5f;
         if (cover < 0.0f) cover = 0.0f;
         if (cover > 1.0f) cover = 1.0f;
         const uint8_t a = (uint8_t) (cover * 255.0f + 0.5f);
         pixels[y * w + x] = ((uint32_t) a << 24) | 0x00FFFFFFu;
      }
   }

   SDL_UpdateTexture(tex, NULL, pixels, w * (int) sizeof(uint32_t));
   SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
   free(pixels);
   return tex;
}

/* --- lifecycle --- */

cobalt_render *
cobalt_render_create(cobalt_surface_id surface, const char *font_path, bool prevent_swap)
{
   cobalt_render *r = (cobalt_render *) calloc(1, sizeof(cobalt_render));
   if (!r) {
      COBALT_LOGE("out of memory allocating render context");
      return NULL;
   }

   r->surface = surface;
   r->m = cobalt_metrics_for(surface);

   uint32_t flags = (surface == COBALT_SURFACE_DRC) ? SDL_WINDOW_WIIU_GAMEPAD_ONLY
                                                    : SDL_WINDOW_WIIU_TV_ONLY;
   if (prevent_swap) {
      flags |= SDL_WINDOW_WIIU_PREVENT_SWAP;
   }

   const char *title = (surface == COBALT_SURFACE_DRC) ? "Cobalt (GamePad)" : "Cobalt (TV)";
   r->window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                r->m->width, r->m->height, flags);
   if (!r->window) {
      COBALT_LOGE("window creation failed for surface %d: %s", (int) surface, SDL_GetError());
      cobalt_render_destroy(r);
      return NULL;
   }

   r->renderer = SDL_CreateRenderer(r->window, -1, SDL_RENDERER_ACCELERATED);
   if (!r->renderer) {
      COBALT_LOGE("renderer creation failed for surface %d: %s", (int) surface, SDL_GetError());
      cobalt_render_destroy(r);
      return NULL;
   }

   SDL_SetRenderDrawBlendMode(r->renderer, SDL_BLENDMODE_BLEND);

   {
      int ww = 0, wh = 0, ow = 0, oh = 0;
      SDL_GetWindowSize(r->window, &ww, &wh);
      SDL_GetRendererOutputSize(r->renderer, &ow, &oh);
      SDL_DisplayMode dm;
      const bool have_dm = SDL_GetDesktopDisplayMode(0, &dm) == 0;
      COBALT_LOGI("surface %d: window %dx%d, renderer %dx%d, display %dx%d (layout %dx%d)",
                  (int) surface, ww, wh, ow, oh, have_dm ? dm.w : 0,
                  have_dm ? dm.h : 0, r->m->width, r->m->height);
   }

   for (int i = 0; i < COBALT_FONT_COUNT; i++) {
      char path[512];
      snprintf(path, sizeof path, "%s", font_path);
      if (i == COBALT_FONT_ICON || i == COBALT_FONT_ICON_FILL) {
         const char *slash = strrchr(font_path, '/');
         snprintf(path, sizeof path, "%.*s%s", slash ? (int) (slash - font_path + 1) : 0,
                  font_path, i == COBALT_FONT_ICON ? "icons.ttf" : "icons-fill.ttf");
      }
      r->fonts[i] = open_font_cached(path, font_size_for(r->m, (cobalt_font_id) i));
      if (!r->fonts[i]) {
         /* Not fatal — see the header. Shapes still draw, text is skipped. */
         COBALT_LOGE("font %s @%dpt failed to open: %s", path,
                     font_size_for(r->m, (cobalt_font_id) i), TTF_GetError());
      }
   }

   open_fallback_fonts(r, font_path);

   r->gradient = build_gradient(r->renderer);
   r->corner_radius = r->m->tile_radius;
   r->corner = build_corner(r->renderer, r->corner_radius);

   COBALT_LOGI("surface %d up: %dx%d prevent_swap=%d fonts=%s",
               (int) surface, r->m->width, r->m->height, (int) prevent_swap,
               cobalt_render_has_font(r) ? "ok" : "MISSING");
   return r;
}

void
cobalt_render_destroy(cobalt_render *r)
{
   if (!r) {
      return;
   }

   cobalt_render_flush_text_cache(r);

   if (r->corner)   SDL_DestroyTexture(r->corner);
   for (int i = 0; i < SHAPE_CACHE_SIZE; i++) {
      if (r->shapes[i].tex) SDL_DestroyTexture(r->shapes[i].tex);
      r->shapes[i].used = false;
      r->shapes[i].tex = NULL;
   }
   if (r->gradient) SDL_DestroyTexture(r->gradient);
   if (r->band) SDL_DestroyTexture(r->band);

   for (int i = 0; i < COBALT_FONT_COUNT; i++) {
      if (r->fonts[i]) {
         TTF_CloseFont(r->fonts[i]);
      }
      if (r->fallbacks[i]) {
         TTF_CloseFont(r->fallbacks[i]);
      }
   }

   if (r->renderer) SDL_DestroyRenderer(r->renderer);
   if (r->window)   SDL_DestroyWindow(r->window);

   free(r);
}

const cobalt_metrics *
cobalt_render_metrics(const cobalt_render *r)
{
   return r ? r->m : cobalt_metrics_for(COBALT_SURFACE_TV);
}

bool
cobalt_render_has_font(const cobalt_render *r)
{
   return r && r->fonts[COBALT_FONT_BODY] != NULL;
}

void
cobalt_render_set_role(cobalt_render *r, cobalt_surface_role role)
{
   if (r) {
      r->role = role;
   }
}

cobalt_surface_role
cobalt_render_role(const cobalt_render *r)
{
   return r ? r->role : COBALT_ROLE_FULL;
}

bool
cobalt_render_remote(const cobalt_render *r)
{
   return cobalt_render_role(r) == COBALT_ROLE_REMOTE;
}

bool
cobalt_render_reader(const cobalt_render *r)
{
   return cobalt_render_role(r) == COBALT_ROLE_READER;
}

#ifdef COBALT_E2E_HOST
SDL_Renderer *
cobalt_render_sdl_renderer(cobalt_render *r)
{
   return r->renderer;
}
#endif

int
cobalt_header_height(cobalt_render *r)
{
   if (!r || !r->fonts[COBALT_FONT_TITLE] || !r->fonts[COBALT_FONT_CAPTION]) {
      return 0;
   }
   const cobalt_metrics *m = r->m;
   return m->pad_edge + TTF_FontLineSkip(r->fonts[COBALT_FONT_TITLE]) - m->line_gap +
          TTF_FontLineSkip(r->fonts[COBALT_FONT_CAPTION]) + m->gap / 2 +
          (m->width < 1000 ? 10 : 0);
}

int
cobalt_content_top(cobalt_render *r)
{
   return cobalt_header_height(r) + r->m->gap;
}

/* One-pixel-wide ramp baked on first use, so the band is one draw call, not a
 * line per row (a GX2 draw each, on both screens, every frame). */
static SDL_Texture *
build_band(cobalt_render *r, int h)
{
   SDL_Texture *tex = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC, 1, h);
   if (!tex) {
      return NULL;
   }
   uint32_t *px = SDL_malloc(sizeof(uint32_t) * (size_t) h);
   if (!px) {
      SDL_DestroyTexture(tex);
      return NULL;
   }
   for (int y = 0; y < h; y++) {
      float t = (float) y / (float) h;
      uint8_t rr = (uint8_t) (COBALT_COLOUR_BAND_TOP.r + t * (COBALT_COLOUR_BAND_BOTTOM.r - COBALT_COLOUR_BAND_TOP.r));
      uint8_t gg = (uint8_t) (COBALT_COLOUR_BAND_TOP.g + t * (COBALT_COLOUR_BAND_BOTTOM.g - COBALT_COLOUR_BAND_TOP.g));
      uint8_t bb = (uint8_t) (COBALT_COLOUR_BAND_TOP.b + t * (COBALT_COLOUR_BAND_BOTTOM.b - COBALT_COLOUR_BAND_TOP.b));
      px[y] = 0xFF000000u | ((uint32_t) rr << 16) | ((uint32_t) gg << 8) | bb;
   }
   SDL_UpdateTexture(tex, NULL, px, (int) sizeof(uint32_t));
   SDL_free(px);
   return tex;
}

/* Miiverse-style blue header behind every screen's title and subtitle. */
static void
draw_header_band(cobalt_render *r)
{
   const cobalt_metrics *m = r->m;
   const int h = cobalt_header_height(r) ? cobalt_header_height(r) : m->pad_edge * 3;
   if (!r->band || r->band_h != h) {
      if (r->band) SDL_DestroyTexture(r->band);
      r->band = build_band(r, h);
      r->band_h = h;
   }
   if (r->band) {
      SDL_Rect dst = { 0, 0, m->width, h };
      SDL_RenderCopy(r->renderer, r->band, NULL, &dst);
   }
   SDL_Color edge = { 0x2F, 0x6D, 0x0B, 0xFF };
   set_draw_colour(r, edge);
   SDL_RenderDrawLine(r->renderer, 0, h, m->width, h);
   SDL_Color shade = { 0x00, 0x00, 0x00, 0x14 };
   set_draw_colour(r, shade);
   SDL_RenderDrawLine(r->renderer, 0, h + 1, m->width, h + 1);
}

void
cobalt_render_begin(cobalt_render *r)
{
   if (!r) {
      return;
   }

   r->clock++;

   if (r->gradient) {
      SDL_Rect dst = { 0, 0, r->m->width, r->m->height };
      SDL_RenderCopy(r->renderer, r->gradient, NULL, &dst);
      draw_header_band(r);
   } else {
      set_draw_colour(r, COBALT_COLOUR_BG_BOTTOM);
      SDL_RenderClear(r->renderer);
   }
}

void
cobalt_render_end(cobalt_render *r)
{
   if (r) {
      const uint32_t t0 = SDL_GetTicks();
      SDL_RenderPresent(r->renderer);
      const uint32_t dt = SDL_GetTicks() - t0;
      if (dt >= 100) {
         COBALT_LOGW("render: slow present %u ms", (unsigned) dt);
      }
   }
}

/* --- primitives --- */

/*
 * Fill a rectangle that the colour makes opaque.
 *
 * The renderer blends every fill by default, which is a per-pixel read and mix
 * even where there is nothing underneath to mix with — a card's body, a band
 * under the header. An opaque fill has nothing to blend, and SDL has a straight
 * fill for it, so ask for that and put the renderer back afterwards. A
 * translucent colour takes the blended path as before.
 */
static void
fill_opaque(cobalt_render *r, const SDL_Rect *rect, SDL_Color colour)
{
   if (colour.a == 0xFF) {
      SDL_SetRenderDrawBlendMode(r->renderer, SDL_BLENDMODE_NONE);
      SDL_RenderFillRect(r->renderer, rect);
      SDL_SetRenderDrawBlendMode(r->renderer, SDL_BLENDMODE_BLEND);
      return;
   }
   SDL_RenderFillRect(r->renderer, rect);
}

void
cobalt_fill_rect(cobalt_render *r, const SDL_Rect *rect, SDL_Color colour)
{
   if (!r || !rect) {
      return;
   }
   set_draw_colour(r, colour);
   fill_opaque(r, rect, colour);
}

/* Blit a cached mask, tinted. */
static void
blit_shape(cobalt_render *r, SDL_Texture *tex, const SDL_Rect *dst, SDL_Color colour)
{
   SDL_SetTextureColorMod(tex, colour.r, colour.g, colour.b);
   SDL_SetTextureAlphaMod(tex, colour.a);
   SDL_RenderCopy(r->renderer, tex, NULL, dst);
}

/*
 * A rounded rect, drawn as one tinted mask of its own shape at this size.
 *
 * Worth it for the shapes that recur every frame at a handful of sizes — the
 * footer's pills, which are a stadium body and an accent chip each — and not
 * for a card, whose size and radius vary with the layout. The masks are a few
 * kilobytes apiece and are evicted least-recently-used like everything else
 * here, so a screen full of prompts cannot grow the cache without bound.
 */
static void
draw_rounded_shape(cobalt_render *r, const SDL_Rect *rect, int radius, SDL_Color colour)
{
   if (!r || !rect || rect->w <= 0 || rect->h <= 0) {
      return;
   }

   const int max_radius = ((rect->w < rect->h) ? rect->w : rect->h) / 2;
   if (radius > max_radius) radius = max_radius;
   if (radius <= 0) {
      cobalt_fill_rect(r, rect, colour);
      return;
   }

   shape_entry *spare = NULL;
   shape_entry *oldest = NULL;
   for (int i = 0; i < SHAPE_CACHE_SIZE; i++) {
      shape_entry *e = &r->shapes[i];
      if (!e->used) {
         if (!spare) spare = e;
         continue;
      }
      if (e->w == rect->w && e->h == rect->h && e->radius == radius) {
         e->last_used = ++r->shape_clock;
         blit_shape(r, e->tex, rect, colour);
         return;
      }
      if (!oldest || e->last_used < oldest->last_used) {
         oldest = e;
      }
   }

   shape_entry *e = spare ? spare : oldest;
   if (e->tex) {
      SDL_DestroyTexture(e->tex);
      e->tex = NULL;
   }
   e->tex = build_shape_mask(r->renderer, rect->w, rect->h, radius);
   if (!e->tex) {
      /* Out of memory or no renderer: the primitives still draw it. */
      e->used = false;
      cobalt_fill_rounded_rect(r, rect, radius, colour);
      return;
   }
   e->used = true;
   e->w = rect->w;
   e->h = rect->h;
   e->radius = radius;
   e->last_used = ++r->shape_clock;
   blit_shape(r, e->tex, rect, colour);
}

int
cobalt_pill_height(cobalt_render *r)
{
   return cobalt_font_line_height(r, COBALT_FONT_CAPTION) + 12;
}

int
cobalt_pill_width(cobalt_render *r, const char *key, const char *label)
{
   const int pill_h = cobalt_pill_height(r);
   const int padx = pill_h / 3 + 2;
   int kw = 0, lw = 0;
   if (key && key[0]) cobalt_text_size(r, COBALT_FONT_CAPTION, key, &kw, NULL);
   if (label && label[0]) cobalt_text_size(r, COBALT_FONT_CAPTION, label, &lw, NULL);
   if (key && key[0]) {
      return padx + (kw + pill_h / 3 + 4) + 6 + lw + padx - 2;
   }
   return padx + lw + padx;
}

/* The one control-prompt pill: every pill on every screen, header and footer,
 * goes through here so they cannot drift apart. */
void
cobalt_draw_pill(cobalt_render *r, const char *key, const char *label, int x, int y)
{
   const int line = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int pill_h = line + 12;
   const int padx = pill_h / 3 + 2;
   const SDL_Color white = { 0xFF, 0xFF, 0xFF, 0xFF };
   const SDL_Color body = { 0xFF, 0xFF, 0xFF, 0xE6 };
   const bool has_key = key && key[0];
   int kw = 0;
   if (has_key) cobalt_text_size(r, COBALT_FONT_CAPTION, key, &kw, NULL);
   const int chip_w = has_key ? kw + pill_h / 3 + 4 : 0;
   SDL_Rect pill = { x, y, cobalt_pill_width(r, key, label), pill_h };
   draw_rounded_shape(r, &pill, pill_h / 2, body);
   int tx = x + padx;
   if (has_key) {
      SDL_Rect chip = { x + 4, y + 4, chip_w, pill_h - 8 };
      draw_rounded_shape(r, &chip, chip.h / 2, COBALT_COLOUR_ACCENT);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, key, chip.x + (chip.w - kw) / 2,
                       y + (pill_h - line) / 2, white);
      tx = chip.x + chip.w + 6;
   }
   cobalt_draw_text(r, COBALT_FONT_CAPTION, label, tx, y + (pill_h - line) / 2,
                    COBALT_COLOUR_TEXT);
}

typedef struct {
   char key[32];
   char label[64];
   int kw, lw, w;
   /* Index into the tappable-pill arrays, or -1 when this segment names no
    * button. Segments and pills are *not* one-to-one — a spec may mix a bare
    * word ("Working...") with keyed prompts — so the draw loops store rects
    * through this rather than through the segment index. */
   int pill;
} hint_seg;

/* Where the last draw_hints put each pill, so a tap can be routed back to the
 * button the pill names. Only the GamePad's are tappable: the TV has no touch.
 * [0] is the header band (under the Back pill), the rest the footer. */
#define COBALT_HINT_MAX 16
static SDL_Rect s_hint_hit[COBALT_HINT_MAX];
static int s_hint_count;
static char s_hint_keys[COBALT_HINT_MAX][8];
static bool s_hint_valid;

/* Map a hint pill's key text to the button it names. COBALT_BTN_COUNT when
 * it names none (a bare word like "Working..." has no button). */
static cobalt_button
hint_key_button(const char *key)
{
   static const struct { const char *key; cobalt_button btn; } table[] = {
      { "A", COBALT_BTN_CONFIRM }, { "B", COBALT_BTN_BACK },
      { "X", COBALT_BTN_ALT_X }, { "Y", COBALT_BTN_ALT_Y },
      { "+", COBALT_BTN_MENU }, { "Left", COBALT_BTN_LEFT },
      { "Right", COBALT_BTN_RIGHT }, { "Up", COBALT_BTN_UP },
      { "Down", COBALT_BTN_DOWN },
   };
   for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
      if (strcmp(key, table[i].key) == 0) {
         return table[i].btn;
      }
   }
   return COBALT_BTN_COUNT;
}

void
cobalt_draw_hints(cobalt_render *r, const char *spec)
{
   if (!r || !spec) {
      return;
   }
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int line = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int pill_h = line + 12;
   const int padx = pill_h / 3 + 2;
   const int gap = 16;
   const int right = m->width - m->pad_edge;

   s_hint_count = 0;
   s_hint_valid = (r->surface == COBALT_SURFACE_DRC);

   hint_seg segs[16];
   int count = 0;
   const char *p = spec;
   while (*p && count < 16) {
      while (*p == ' ') p++;
      if (!*p) break;
      const char *end = p;
      while (*end && !(end[0] == ' ' && end[1] == ' ')) end++;

      char seg[96];
      size_t n = (size_t) (end - p);
      if (n >= sizeof seg) n = sizeof seg - 1;
      memcpy(seg, p, n);
      seg[n] = '\0';
      p = end;

      hint_seg *h = &segs[count++];
      memset(h, 0, sizeof *h);
      char *colon = strstr(seg, ": ");
      if (colon) {
         *colon = '\0';
         snprintf(h->key, sizeof h->key, "%s", seg);
         snprintf(h->label, sizeof h->label, "%s", colon + 2);
         cobalt_text_size(r, COBALT_FONT_CAPTION, h->key, &h->kw, NULL);
      } else {
         snprintf(h->label, sizeof h->label, "%s", seg);
      }
      /* Only a pill naming a single, mappable button is tappable. A key that
       * is a range or a compound ("Up/Down", "A/Left/Right") has no one
       * button to be, and half-applying it would be worse than ignoring the
       * tap. */
      h->pill = -1;
      if (s_hint_valid && h->key[0] &&
          hint_key_button(h->key) != COBALT_BTN_COUNT &&
          s_hint_count < COBALT_HINT_MAX) {
         snprintf(s_hint_keys[s_hint_count], sizeof s_hint_keys[0], "%s",
                  h->key);
         h->pill = s_hint_count++;
      }
      cobalt_text_size(r, COBALT_FONT_CAPTION, h->label, &h->lw, NULL);
      const int chip_w = h->key[0] ? h->kw + pill_h / 3 + 4 : 0;
      h->w = padx + (h->key[0] ? chip_w + 6 : 0) + h->lw + padx -
             (h->key[0] ? 2 : 0);
   }

   /* The GamePad is short: with more than three prompts the first half moves
    * into the header band (right-aligned, under the Back pill) so the bottom
    * edge is not a crowded strip. */
   int header_n = 0;
   if (m->width < 1000 && count > 3) {
      header_n = count / 2;
      /* Keep clear of the title and subtitle on the left. */
      const int left_limit = 300;
      for (;;) {
         int total = 0;
         for (int i = 0; i < header_n; i++) total += segs[i].w + (i ? gap : 0);
         if (header_n == 0 || right - total >= left_limit) break;
         header_n--;
      }
   }

   if (header_n > 0) {
      int total = 0;
      for (int i = 0; i < header_n; i++) total += segs[i].w + (i ? gap : 0);
      int x = right - total;
      const int y = m->pad_edge + pill_h + 6;
      for (int i = 0; i < header_n; i++) {
         cobalt_draw_pill(r, segs[i].key, segs[i].label, x, y);
         if (segs[i].pill >= 0) {
            s_hint_hit[segs[i].pill] = (SDL_Rect){ x, y, segs[i].w, pill_h };
         }
         x += segs[i].w + gap;
      }
   }

   int x = m->pad_edge;
   int y = m->height - m->pad_edge / 2 - pill_h;
   for (int i = header_n; i < count; i++) {
      if (x + segs[i].w > right && x > m->pad_edge) {
         x = m->pad_edge;
         y -= pill_h + 6;
      }
      cobalt_draw_pill(r, segs[i].key, segs[i].label, x, y);
      if (segs[i].pill >= 0) {
         s_hint_hit[segs[i].pill] = (SDL_Rect){ x, y, segs[i].w, pill_h };
      }
      x += segs[i].w + gap;
   }
}

/* Turn a GamePad tap that landed on a hint pill into the button press it
 * names, so the on-screen prompts are themselves buttons. Returns the number
 * of pills hit, filling `out` with their buttons. */
int
cobalt_hints_tapped(const cobalt_input *in, cobalt_button *out, int max)
{
   if (!in || !out || max <= 0 || !in->touch_ended || !s_hint_valid) {
      return 0;
   }
   int n = 0;
   for (int i = 0; i < s_hint_count; i++) {
      if (cobalt_input_tapped(in, &s_hint_hit[i])) {
         const cobalt_button btn = hint_key_button(s_hint_keys[i]);
         if (btn != COBALT_BTN_COUNT && n < max) {
            out[n++] = btn;
         }
      }
   }
   return n;
}

int
cobalt_hints_pill(int i, SDL_Rect *rect, char *key, size_t key_size)
{
   if (!s_hint_valid || i < 0 || i >= s_hint_count) {
      return 0;
   }
   if (rect) {
      *rect = s_hint_hit[i];
   }
   if (key && key_size > 0) {
      snprintf(key, key_size, "%s", s_hint_keys[i]);
   }
   return 1;
}

void
cobalt_fill_rounded_rect(cobalt_render *r, const SDL_Rect *rect, int radius,
                         SDL_Color colour)
{
   if (!r || !rect || rect->w <= 0 || rect->h <= 0) {
      return;
   }

   /* Radius cannot exceed half of either side, or the corners overlap. */
   int max_radius = ((rect->w < rect->h) ? rect->w : rect->h) / 2;
   if (radius > max_radius) radius = max_radius;

   if (radius <= 0 || !r->corner) {
      cobalt_fill_rect(r, rect, colour);
      return;
   }

   set_draw_colour(r, colour);

   /* Body: a tall centre band plus two side bands, leaving the corners bare. */
   SDL_Rect middle = { rect->x, rect->y + radius, rect->w, rect->h - 2 * radius };
   SDL_Rect top    = { rect->x + radius, rect->y, rect->w - 2 * radius, radius };
   SDL_Rect bottom = { rect->x + radius, rect->y + rect->h - radius,
                       rect->w - 2 * radius, radius };
   if (middle.h > 0) fill_opaque(r, &middle, colour);
   if (top.w > 0)    fill_opaque(r, &top, colour);
   if (bottom.w > 0) fill_opaque(r, &bottom, colour);

   SDL_SetTextureColorMod(r->corner, colour.r, colour.g, colour.b);
   SDL_SetTextureAlphaMod(r->corner, colour.a);

   const struct { int x, y; SDL_RendererFlip flip; } corners[] = {
      { rect->x,                     rect->y,                     SDL_FLIP_NONE },
      { rect->x + rect->w - radius,  rect->y,                     SDL_FLIP_HORIZONTAL },
      { rect->x,                     rect->y + rect->h - radius,  SDL_FLIP_VERTICAL },
      { rect->x + rect->w - radius,  rect->y + rect->h - radius,
        (SDL_RendererFlip) (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL) },
   };

   for (int i = 0; i < 4; i++) {
      SDL_Rect dst = { corners[i].x, corners[i].y, radius, radius };
      SDL_RenderCopyEx(r->renderer, r->corner, NULL, &dst, 0.0, NULL, corners[i].flip);
   }
}

/* `top` composited over `bottom` when `bottom` is opaque, so a translucent
 * overlay on a known solid can be drawn as a solid of its own. */
static SDL_Color
blend_over(SDL_Color top, SDL_Color bottom)
{
   const int a = top.a;
   const int ia = 255 - a;
   SDL_Color out = {
      (Uint8) ((top.r * a + bottom.r * ia + 127) / 255),
      (Uint8) ((top.g * a + bottom.g * ia + 127) / 255),
      (Uint8) ((top.b * a + bottom.b * ia + 127) / 255),
      0xFF,
   };
   return out;
}

/*
 * The border of a rounded rect: the bands and corner tiles a fill would draw,
 * with the middle left out. `rect` is the outside edge and `thickness` how far
 * the border reaches in. The interior is assumed to be painted over afterwards
 * by something opaque — a tile body, in the one place this is used.
 */
static void
fill_rounded_ring(cobalt_render *r, const SDL_Rect *rect, int radius, int thickness,
                  SDL_Color colour)
{
   if (!r || !rect || rect->w <= 0 || rect->h <= 0) {
      return;
   }

   const int max_radius = ((rect->w < rect->h) ? rect->w : rect->h) / 2;
   if (radius > max_radius) radius = max_radius;
   if (radius <= 0 || thickness <= 0 || !r->corner) {
      cobalt_fill_rounded_rect(r, rect, radius, colour);
      return;
   }
   if (thickness > max_radius) thickness = max_radius;

   set_draw_colour(r, colour);

   SDL_Rect top    = { rect->x + radius, rect->y, rect->w - 2 * radius, thickness };
   SDL_Rect bottom = { rect->x + radius, rect->y + rect->h - thickness,
                       rect->w - 2 * radius, thickness };
   SDL_Rect left   = { rect->x, rect->y + radius, thickness, rect->h - 2 * radius };
   SDL_Rect right  = { rect->x + rect->w - thickness, rect->y + radius, thickness,
                       rect->h - 2 * radius };
   if (top.w > 0)    fill_opaque(r, &top, colour);
   if (bottom.w > 0) fill_opaque(r, &bottom, colour);
   if (left.h > 0)   fill_opaque(r, &left, colour);
   if (right.h > 0)  fill_opaque(r, &right, colour);

   SDL_SetTextureColorMod(r->corner, colour.r, colour.g, colour.b);
   SDL_SetTextureAlphaMod(r->corner, colour.a);

   const struct { int x, y; SDL_RendererFlip flip; } corners[] = {
      { rect->x,                    rect->y,                    SDL_FLIP_NONE },
      { rect->x + rect->w - radius, rect->y,                    SDL_FLIP_HORIZONTAL },
      { rect->x,                    rect->y + rect->h - radius, SDL_FLIP_VERTICAL },
      { rect->x + rect->w - radius, rect->y + rect->h - radius,
        (SDL_RendererFlip) (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL) },
   };

   for (int i = 0; i < 4; i++) {
      SDL_Rect dst = { corners[i].x, corners[i].y, radius, radius };
      SDL_RenderCopyEx(r->renderer, r->corner, NULL, &dst, 0.0, NULL, corners[i].flip);
   }
}

/*
 * A tile's drop shadow, drawn only where the tile does not cover it.
 *
 * The body is opaque and sits over the shadow (the shadow is offset down and to
 * the right of it), so all that can be seen of it is a column down the right, a
 * band along the bottom, and whatever the body's rounded corners cut away. The
 * whole offset rect is no longer filled: on a card the size of a timeline post
 * that blended a card's area of pixels per frame to paint an edge a few pixels
 * wide. The corner tiles still go down in full — under an opaque body they cost
 * only their radius squared, and they are what rounds the shadow itself.
 */
static void
fill_tile_shadow(cobalt_render *r, const SDL_Rect *shadow, const SDL_Rect *body,
                 int radius, SDL_Color colour)
{
   if (!r || !shadow || !body || shadow->w <= 0 || shadow->h <= 0) {
      return;
   }

   const int right  = shadow->x + shadow->w - (body->x + body->w);
   const int bottom = shadow->y + shadow->h - (body->y + body->h);
   if (right <= 0 && bottom <= 0) {
      return; /* The body hides every pixel of it. */
   }

   const int max_radius = ((shadow->w < shadow->h) ? shadow->w : shadow->h) / 2;
   if (radius > max_radius) radius = max_radius;
   if (radius <= 0 || !r->corner) {
      cobalt_fill_rounded_rect(r, shadow, radius, colour);
      return;
   }

   set_draw_colour(r, colour);

   /* The straight parts of the shadow's edges the body does not reach. */
   if (right > 0) {
      SDL_Rect band = { shadow->x + shadow->w - right, shadow->y + radius, right,
                        shadow->h - 2 * radius };
      if (band.h > 0) SDL_RenderFillRect(r->renderer, &band);
   }
   if (bottom > 0) {
      SDL_Rect band = { shadow->x + radius, shadow->y + shadow->h - bottom,
                        shadow->w - 2 * radius, bottom };
      if (band.w > 0) SDL_RenderFillRect(r->renderer, &band);
   }

   SDL_SetTextureColorMod(r->corner, colour.r, colour.g, colour.b);
   SDL_SetTextureAlphaMod(r->corner, colour.a);

   const struct { int x, y; SDL_RendererFlip flip; } corners[] = {
      { shadow->x,                    shadow->y,                    SDL_FLIP_NONE },
      { shadow->x + shadow->w - radius, shadow->y,                   SDL_FLIP_HORIZONTAL },
      { shadow->x,                    shadow->y + shadow->h - radius, SDL_FLIP_VERTICAL },
      { shadow->x + shadow->w - radius, shadow->y + shadow->h - radius,
        (SDL_RendererFlip) (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL) },
   };

   for (int i = 0; i < 4; i++) {
      SDL_Rect dst = { corners[i].x, corners[i].y, radius, radius };
      SDL_RenderCopyEx(r->renderer, r->corner, NULL, &dst, 0.0, NULL, corners[i].flip);
   }
}

void
cobalt_draw_tile(cobalt_render *r, const SDL_Rect *rect, float focus)
{
   if (!r || !rect) {
      return;
   }

   if (focus < 0.0f) focus = 0.0f;
   if (focus > 1.0f) focus = 1.0f;

   const int radius = r->m->tile_radius;

   /* Drop shadow: a single offset rounded rect rather than a real blur. At
    * these sizes the difference is not visible on a TV and it costs one draw. */
   /* Focus tints the shadow with the accent, as Miiverse does for a post you
    * have given a Yeah, rather than only darkening it. */
   const SDL_Color ink = { 0x30, 0x40, 0x50, 0xFF };
   const SDL_Color acc = COBALT_COLOUR_ACCENT;
   SDL_Color shadow = {
      (Uint8) (ink.r + (acc.r - ink.r) * focus),
      (Uint8) (ink.g + (acc.g - ink.g) * focus),
      (Uint8) (ink.b + (acc.b - ink.b) * focus),
      (Uint8) (26 + 44 * focus),
   };
   SDL_Rect shadow_rect = { rect->x + 2, rect->y + 3 + (int) (2 * focus),
                            rect->w, rect->h };

   /* Focused tiles lift slightly and brighten — the Wii U menu's tactile feel
    * (AGENTS.md §5) comes mostly from this rather than from any animation. */
   SDL_Rect body = *rect;
   body.y -= (int) (2.0f * focus);

   fill_tile_shadow(r, &shadow_rect, &body, radius, shadow);

   if (focus > 0.0f) {
      /* A rounded ring just outside the body: SDL_RenderDrawRect would draw
       * square corners around a rounded tile. Only its 2px border survives the
       * body being drawn over it, so the middle is not filled — at this size
       * that was the whole card's area of accent blended for a frame edge. */
      SDL_Color edge = COBALT_COLOUR_ACCENT;
      edge.a = (Uint8) (220 * focus);
      const SDL_Rect ring = { body.x - 2, body.y - 2, body.w + 4, body.h + 4 };
      fill_rounded_ring(r, &ring, radius + 2, 2, edge);
   }

   SDL_Color base = focus > 0.0f ? COBALT_COLOUR_TILE : COBALT_COLOUR_TILE;
   cobalt_fill_rounded_rect(r, &body, radius, base);

   /* Top sheen: a translucent band across the upper third, giving the glassy
    * highlight the flat-card look lacks. It lies wholly within the opaque body,
    * so it is composited with it first and drawn as one solid rounded rect;
    * where the result is the body's own colour — a white gloss on a white tile,
    * which is what the theme sets — there is nothing to draw and it is skipped
    * rather than blended across the card's top third every frame. */
   SDL_Rect sheen = { body.x + radius / 2, body.y + 1,
                      body.w - radius, body.h / 3 };
   if (sheen.w > 0 && sheen.h > 0) {
      SDL_Color gloss = { 0xFF, 0xFF, 0xFF, (Uint8) (60 + 40 * focus) };
      const SDL_Color lit = blend_over(gloss, base);
      if (lit.r != base.r || lit.g != base.g || lit.b != base.b) {
         cobalt_fill_rounded_rect(r, &sheen, radius / 2, lit);
      }
   }

}

SDL_Rect
cobalt_typing_area(cobalt_render *r)
{
   SDL_Rect area = { 0, 0, 0, 0 };
   if (!r) {
      return area;
   }
   const cobalt_metrics *m = r->m;
   const int top = cobalt_content_top(r);
   /* Down to the footer's top edge, which is one pill plus the half margin
    * cobalt_draw_hints() puts under it. */
   const int bottom = m->height - m->pad_edge / 2 - cobalt_pill_height(r) - m->gap;
   area.x = m->pad_edge;
   area.y = top;
   area.w = m->width - 2 * m->pad_edge;
   area.h = bottom - top;
   return area;
}

void
cobalt_draw_typing_panel(cobalt_render *r, const SDL_Rect *area,
                         const char *caption, const char *text)
{
   if (!r || !area || area->w <= 0 || area->h <= 0) {
      return;
   }

   const cobalt_metrics *m = r->m;
   cobalt_draw_tile(r, area, 0.0f);

   const int left = area->x + m->pad_tile;
   const int width = area->w - 2 * m->pad_tile;
   int y = area->y + m->pad_tile;
   if (width <= 0) {
      return;
   }

   if (caption && caption[0]) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, caption, left, y,
                       COBALT_COLOUR_TEXT_DIM);
      y += cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   }

   const int line = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->line_gap;
   /* Lines that fit under the caption, counted from the body's own metrics so
    * a panel with no room draws nothing rather than spilling off its tile. */
   const int lines = line > 0 ? (area->y + area->h - m->pad_tile - y) / line : 0;
   if (lines < 1) {
      return;
   }

   if (text && text[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, text, left, y, width, lines,
                               COBALT_COLOUR_TEXT);
   } else {
      cobalt_draw_text(r, COBALT_FONT_BODY, "Nothing typed yet", left, y,
                       COBALT_COLOUR_TEXT_DIM);
   }
}

void
cobalt_draw_texture(cobalt_render *r, SDL_Texture *texture, const SDL_Rect *dst)
{
   if (!r || !r->renderer || !texture || !dst) {
      return;
   }
   SDL_RenderCopy(r->renderer, texture, NULL, dst);
}

SDL_Texture *
cobalt_render_upload(cobalt_render *r, SDL_Surface *surface)
{
   if (!r || !r->renderer || !surface) {
      return NULL;
   }

   SDL_Texture *texture = SDL_CreateTextureFromSurface(r->renderer, surface);
   if (!texture) {
      COBALT_LOGW("texture upload failed: %s", SDL_GetError());
      return NULL;
   }
   SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
   return texture;
}

void
cobalt_render_set_images(cobalt_render *r, cobalt_imagecache *cache)
{
   if (r) {
      r->images = cache;
   }
}

cobalt_imagecache *
cobalt_render_images(cobalt_render *r)
{
   return r ? r->images : NULL;
}

void
cobalt_render_set_thumbs(cobalt_render *r, cobalt_imagecache *cache)
{
   if (r) {
      r->thumbs = cache;
   }
}

cobalt_imagecache *
cobalt_render_thumbs(cobalt_render *r)
{
   return r ? r->thumbs : NULL;
}

void
cobalt_render_set_viewer(cobalt_render *r, cobalt_imagecache *cache)
{
   if (r) {
      r->viewer = cache;
   }
}

cobalt_imagecache *
cobalt_render_viewer(cobalt_render *r)
{
   return r ? r->viewer : NULL;
}

/* --- text --- */

static TTF_Font *
font_of(cobalt_render *r, cobalt_font_id id)
{
   if (!r || id < 0 || id >= COBALT_FONT_COUNT) {
      return NULL;
   }
   return r->fonts[id];
}


/*
 * Glyph fallback. One TTF_Font is one face, so a codepoint the bundled font
 * lacks draws as a tofu box. Latin-only Lato means every Japanese, Chinese or
 * Korean post is a row of boxes, so strings are split into runs and each run is
 * drawn from the first face that provides it.
 *
 * The fallback face is an optional bundled fallback.ttf, never a system font.
 */
/*
 * Fonts are read into memory once and every size is opened from that buffer.
 * Left to stream from the file, FreeType seeks and reads the card for each
 * cold glyph, and on the console each of those took 100-700 ms while the
 * worker was also using the card. The buffers are shared by both surfaces and
 * live until exit.
 */
typedef struct {
   char path[512];
   unsigned char *data;
   size_t size;
} font_blob;

static font_blob font_blobs[4];

static TTF_Font *
open_font_cached(const char *path, int pt)
{
   font_blob *blob = NULL;
   for (size_t i = 0; i < sizeof(font_blobs) / sizeof(font_blobs[0]); i++) {
      if (font_blobs[i].data && strcmp(font_blobs[i].path, path) == 0) {
         blob = &font_blobs[i];
         break;
      }
   }
   if (!blob) {
      for (size_t i = 0; i < sizeof(font_blobs) / sizeof(font_blobs[0]); i++) {
         if (!font_blobs[i].data) {
            blob = &font_blobs[i];
            break;
         }
      }
      FILE *f = blob ? fopen(path, "rb") : NULL;
      if (f) {
         fseek(f, 0, SEEK_END);
         long len = ftell(f);
         fseek(f, 0, SEEK_SET);
         unsigned char *data = len > 0 ? (unsigned char *) malloc((size_t) len) : NULL;
         if (data && fread(data, 1, (size_t) len, f) == (size_t) len) {
            blob->data = data;
            blob->size = (size_t) len;
            snprintf(blob->path, sizeof blob->path, "%s", path);
         } else {
            free(data);
            blob = NULL;
         }
         fclose(f);
      } else {
         blob = NULL;
      }
   }
   if (!blob) {
      return TTF_OpenFont(path, pt);
   }
   SDL_RWops *rw = SDL_RWFromConstMem(blob->data, (int) blob->size);
   return rw ? TTF_OpenFontRW(rw, 1, pt) : NULL;
}

static void
open_fallback_fonts(cobalt_render *r, const char *font_path)
{
   /* A second bundled face (e.g. Hangul or Hans) goes next to the primary as
    * fallback.ttf. COBALT_FALLBACK_FONT overrides it for host experiments. */
   char path[512];
   const char *env = getenv("COBALT_FALLBACK_FONT");
   if (env && env[0]) {
      snprintf(path, sizeof path, "%s", env);
   } else {
      const char *slash = strrchr(font_path, '/');
      int dir = slash ? (int) (slash - font_path + 1) : 0;
      snprintf(path, sizeof path, "%.*sfallback.ttf", dir, font_path);
   }
   FILE *f = fopen(path, "rb");
   if (!f) {
      return;
   }
   fclose(f);
   for (int i = 0; i < COBALT_FONT_ICON; i++) {
      r->fallbacks[i] = open_font_cached(path, font_size_for(r->m, (cobalt_font_id) i));
   }
}

static uint32_t
utf8_next(const char **p)
{
   const unsigned char *s = (const unsigned char *) *p;
   uint32_t cp = 0xFFFD;
   int n = 1;
   if (s[0] < 0x80) {
      cp = s[0];
   } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
      cp = ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
      n = 2;
   } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
      cp = ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
      n = 3;
   } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
              (s[3] & 0xC0) == 0x80) {
      cp = ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
      n = 4;
   }
   *p += n;
   return cp;
}

typedef struct {
   TTF_Font *font;
   char text[TEXT_KEY_MAX];
} text_run;

/* Face for one codepoint: primary if it has the glyph, else the fallback. */
static TTF_Font *
face_for(TTF_Font *primary, TTF_Font *fallback, uint32_t cp)
{
   if (!fallback || cp < 0x80 || TTF_GlyphIsProvided32(primary, cp)) {
      return primary;
   }
   return TTF_GlyphIsProvided32(fallback, cp) ? fallback : primary;
}

/* True when the whole string can be drawn from the primary face alone. */
static bool
single_face(TTF_Font *primary, TTF_Font *fallback, const char *utf8)
{
   if (!fallback) {
      return true;
   }
   const char *p = utf8;
   while (*p) {
      if (face_for(primary, fallback, utf8_next(&p)) != primary) {
         return false;
      }
   }
   return true;
}

/* Split into maximal same-face runs. Returns the run count (bounded). */
static int
split_runs(TTF_Font *primary, TTF_Font *fallback, const char *utf8, text_run *runs, int max_runs)
{
   int n = 0;
   const char *p = utf8;
   while (*p && n < max_runs) {
      const char *start = p;
      uint32_t cp = utf8_next(&p);
      TTF_Font *face = face_for(primary, fallback, cp);
      while (*p) {
         const char *save = p;
         if (face_for(primary, fallback, utf8_next(&p)) != face) {
            p = save;
            break;
         }
      }
      size_t len = (size_t) (p - start);
      if (len >= sizeof(runs[n].text)) {
         len = sizeof(runs[n].text) - 1;
      }
      memcpy(runs[n].text, start, len);
      runs[n].text[len] = '\0';
      runs[n].font = face;
      n++;
   }
   return n;
}

#define MAX_RUNS 32

static int
measure_text_impl(cobalt_render *r, cobalt_font_id id, const char *utf8, int *out_w, int *out_h)
{
   TTF_Font *font = r->fonts[id];
   TTF_Font *fb = r->fallbacks[id];
   if (single_face(font, fb, utf8)) {
      return TTF_SizeUTF8(font, utf8, out_w, out_h);
   }
   text_run runs[MAX_RUNS];
   int n = split_runs(font, fb, utf8, runs, MAX_RUNS);
   int w = 0, h = 0;
   for (int i = 0; i < n; i++) {
      int rw = 0, rh = 0;
      if (TTF_SizeUTF8(runs[i].font, runs[i].text, &rw, &rh) != 0) {
         return -1;
      }
      w += rw;
      if (rh > h) h = rh;
   }
   if (out_w) *out_w = w;
   if (out_h) *out_h = h;
   return 0;
}

static SDL_Surface *
render_text_surface_impl(cobalt_render *r, cobalt_font_id id, const char *utf8, SDL_Color colour)
{
   TTF_Font *font = r->fonts[id];
   TTF_Font *fb = r->fallbacks[id];
   if (single_face(font, fb, utf8)) {
      return TTF_RenderUTF8_Blended(font, utf8, colour);
   }

   text_run runs[MAX_RUNS];
   int n = split_runs(font, fb, utf8, runs, MAX_RUNS);
   SDL_Surface *parts[MAX_RUNS] = { 0 };
   int w = 0, h = 0;
   for (int i = 0; i < n; i++) {
      parts[i] = TTF_RenderUTF8_Blended(runs[i].font, runs[i].text, colour);
      if (parts[i]) {
         w += parts[i]->w;
         if (parts[i]->h > h) h = parts[i]->h;
      }
   }

   SDL_Surface *out = NULL;
   if (w > 0 && h > 0) {
      out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
   }
   if (out) {
      SDL_FillRect(out, NULL, 0);
      int x = 0;
      for (int i = 0; i < n; i++) {
         if (!parts[i]) continue;
         SDL_SetSurfaceBlendMode(parts[i], SDL_BLENDMODE_NONE);
         SDL_Rect dst = { x, (h - parts[i]->h) / 2, 0, 0 };
         SDL_BlitSurface(parts[i], NULL, out, &dst);
         x += parts[i]->w;
      }
   }
   for (int i = 0; i < n; i++) {
      if (parts[i]) SDL_FreeSurface(parts[i]);
   }
   return out;
}

/* Hardware-pass diagnostics: a glyph load that stalls on the SD card shows up
 * here by name instead of as an anonymous long frame. */
#define SLOW_MS 100

/*
 * Nanoseconds on a monotonic clock, for the benchmark's own accounting.
 *
 * Host builds only — `make bench` compiles with COBALT_E2E_HOST — so a console
 * frame takes no clock reading here. SDL_GetTicks is far too coarse for a
 * single measurement (it would round every one of them to zero); the large
 * integer arithmetic cannot overflow: SDL counts a few tens of millions per
 * second and this is a 64-bit value.
 */
#ifdef COBALT_E2E_HOST
static uint64_t
now_ns(void)
{
   static uint64_t freq;
   if (freq == 0) {
      freq = SDL_GetPerformanceFrequency();
   }
   return freq ? SDL_GetPerformanceCounter() * 1000000000ull / freq : 0;
}
#endif

static int
measure_text(cobalt_render *r, cobalt_font_id id, const char *utf8, int *out_w, int *out_h)
{
   const uint32_t t0 = SDL_GetTicks();
#ifdef COBALT_E2E_HOST
   const uint64_t ns0 = now_ns();
#endif
   if (r) r->text_measures++;
   const int rc = measure_text_impl(r, id, utf8, out_w, out_h);
#ifdef COBALT_E2E_HOST
   if (r) r->text_measure_ns += now_ns() - ns0;
#endif
   const uint32_t dt = SDL_GetTicks() - t0;
   if (dt >= SLOW_MS) {
      COBALT_LOGW("render: slow measure %u ms (font %d, %.40s)", (unsigned) dt, (int) id, utf8);
   }
   return rc;
}

static SDL_Surface *
render_text_surface(cobalt_render *r, cobalt_font_id id, const char *utf8, SDL_Color colour)
{
   const uint32_t t0 = SDL_GetTicks();
   SDL_Surface *surface = render_text_surface_impl(r, id, utf8, colour);
   const uint32_t dt = SDL_GetTicks() - t0;
   if (dt >= SLOW_MS) {
      COBALT_LOGW("render: slow glyph render %u ms (font %d, %.40s)", (unsigned) dt, (int) id, utf8);
   }
   return surface;
}

/* Render straight to a texture, bypassing the cache. Caller destroys it. */
static SDL_Texture *
render_uncached(cobalt_render *r, cobalt_font_id font_id, const char *utf8, SDL_Color colour,
                int *out_w, int *out_h)
{
   SDL_Surface *surface = render_text_surface(r, font_id, utf8, colour);
   if (!surface) {
      return NULL;
   }

   SDL_Texture *tex = SDL_CreateTextureFromSurface(r->renderer, surface);
   if (out_w) *out_w = surface->w;
   if (out_h) *out_h = surface->h;
   SDL_FreeSurface(surface);
   return tex;
}

/*
 * Look the string up, rendering and caching it on a miss. The returned texture
 * belongs to the cache and must not be destroyed by the caller.
 * Returns NULL when the string cannot be cached (too long) or has no font.
 */
static text_entry *
cache_lookup(cobalt_render *r, cobalt_font_id font_id, const char *utf8, SDL_Color colour)
{
   TTF_Font *font = font_of(r, font_id);
   if (!font || !utf8 || utf8[0] == '\0') {
      return NULL;
   }

   size_t len = strlen(utf8);
   if (len >= TEXT_KEY_MAX) {
      return NULL;
   }

   const uint32_t hash = hash_string(utf8);
   const uint32_t packed = pack_colour(colour);

   text_entry *victim = NULL;
   for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
      text_entry *e = &r->cache[i];

      if (!e->in_use) {
         if (!victim || victim->in_use) {
            victim = e;
         }
         continue;
      }

      if (e->hash == hash && e->font == font_id && e->colour == packed &&
          strcmp(e->text, utf8) == 0) {
         e->last_used = r->clock;
         return e;
      }

      /* Track the least-recently-used entry in case we need to evict. */
      if (!victim || (victim->in_use && e->last_used < victim->last_used)) {
         victim = e;
      }
   }

   if (!victim) {
      return NULL;
   }

   if (victim->in_use && victim->tex) {
      SDL_DestroyTexture(victim->tex);
      victim->tex = NULL;
   }

   int w = 0, h = 0;
   SDL_Texture *tex = render_uncached(r, font_id, utf8, colour, &w, &h);
   if (!tex) {
      victim->in_use = false;
      return NULL;
   }

   memcpy(victim->text, utf8, len + 1);
   victim->hash = hash;
   victim->colour = packed;
   victim->font = font_id;
   victim->tex = tex;
   victim->w = w;
   victim->h = h;
   victim->last_used = r->clock;
   victim->in_use = true;
   return victim;
}

int
cobalt_draw_text(cobalt_render *r, cobalt_font_id font_id, const char *utf8,
                 int x, int y, SDL_Color colour)
{
   if (!r || !utf8 || utf8[0] == '\0') {
      return 0;
   }

   text_entry *e = cache_lookup(r, font_id, utf8, colour);
   if (e) {
      SDL_Rect dst = { x, y, e->w, e->h };
      SDL_RenderCopy(r->renderer, e->tex, NULL, &dst);
      return e->w;
   }

   /* Uncacheable (over-long) string: render it directly this frame. Logged
    * once so a hot path doing this repeatedly is visible in the UDP log. */
   TTF_Font *font = font_of(r, font_id);
   if (!font) {
      return 0;
   }

   if (!r->warned_long_string && strlen(utf8) >= TEXT_KEY_MAX) {
      r->warned_long_string = true;
      COBALT_LOGW("string longer than %d bytes bypassed the text cache; "
                  "wrap before drawing to avoid per-frame allocation", TEXT_KEY_MAX);
   }

   int w = 0, h = 0;
   SDL_Texture *tex = render_uncached(r, font_id, utf8, colour, &w, &h);
   if (!tex) {
      return 0;
   }
   SDL_Rect dst = { x, y, w, h };
   SDL_RenderCopy(r->renderer, tex, NULL, &dst);
   SDL_DestroyTexture(tex);
   return w;
}

int
cobalt_draw_text_centred(cobalt_render *r, cobalt_font_id font_id, const char *utf8,
                         int x, int y, int width, SDL_Color colour)
{
   int w = 0, h = 0;
   cobalt_text_size(r, font_id, utf8, &w, &h);
   (void) h;
   return cobalt_draw_text(r, font_id, utf8, x + (width - w) / 2, y, colour);
}

void
cobalt_text_size(cobalt_render *r, cobalt_font_id font_id, const char *utf8,
                 int *out_w, int *out_h)
{
   if (out_w) *out_w = 0;
   if (out_h) *out_h = 0;

   TTF_Font *font = font_of(r, font_id);
   if (!font || !utf8) {
      return;
   }

   int w = 0, h = 0;
   if (measure_text(r, font_id, utf8, &w, &h) == 0) {
      if (out_w) *out_w = w;
      if (out_h) *out_h = h;
   }
}

int
cobalt_font_line_height(cobalt_render *r, cobalt_font_id font_id)
{
   TTF_Font *font = font_of(r, font_id);
   return font ? TTF_FontLineSkip(font) : 0;
}

/* Byte offset of the start of the UTF-8 sequence preceding `pos`. */
static size_t
utf8_prev(const char *s, size_t pos)
{
   if (pos == 0) {
      return 0;
   }
   size_t i = pos - 1;
   /* Continuation bytes are 10xxxxxx; walk back to the lead byte. */
   while (i > 0 && ((unsigned char) s[i] & 0xC0) == 0x80) {
      i--;
   }
   return i;
}

/* Draw one wrapped line whose first byte is at `base` in the whole text,
 * switching colour at span boundaries. */
static void
draw_line_spans(cobalt_render *r, cobalt_font_id font_id, TTF_Font *font,
                const char *line, int base, int x, int y, SDL_Color colour,
                const cobalt_text_span *spans, int span_count)
{
   const int n = (int) strlen(line);
   int p = 0;
   while (p < n) {
      const cobalt_text_span *in = NULL;
      int next = n;
      for (int i = 0; i < span_count; i++) {
         const int a = spans[i].start - base;
         const int b = spans[i].end - base;
         if (a <= p && p < b) {
            in = &spans[i];
            next = b < n ? b : n;
            break;
         }
         if (a > p && a < next) {
            next = a;
         }
      }
      char seg[TEXT_KEY_MAX];
      int len = next - p;
      if (len >= (int) sizeof(seg)) {
         len = (int) sizeof(seg) - 1;
      }
      memcpy(seg, line + p, (size_t) len);
      seg[len] = '\0';
      const SDL_Color c = in ? in->colour : colour;
      const int w = cobalt_draw_text(r, font_id, seg, x, y, c);
      if (in && in->underline && w > 0) {
         const SDL_Rect ul = { x, y + TTF_FontAscent(font) + 2, w, 1 };
         cobalt_fill_rect(r, &ul, c);
      }
      x += w;
      p = next;
   }
}

/*
 * Works out the line that begins at `offset` in `text`, leaving its bytes in
 * `out` (which holds TEXT_KEY_MAX bytes) and its extent in `ln`. Returns false
 * when there is nothing left to lay out, which includes the degenerate case of
 * a single glyph too wide for the column.
 *
 * This is the only place the wrapping rule lives, so a string laid out into the
 * cache and the same string drawn without one cannot break in different places.
 *
 * The rule is a line of whole words: measure a token at a time — a run of
 * non-spaces plus the spaces after it — and commit it while it fits. A line can
 * only break at a space, so measuring every byte of the line (what this used to
 * do) pays the length of the whole prefix to find one break, once per byte, and
 * so squares with the length of the line. Only the token that does not fit is
 * counted byte by byte, which is that same prefix walk narrowed to the one
 * place the break can be.
 */
static bool
next_wrapped_line(cobalt_render *r, cobalt_font_id font_id, const char *text, int offset,
                  int max_width, char *out, wrap_line *ln)
{
   const char *cursor = text + offset;
   if (!*cursor) {
      return false;
   }

   size_t len = 0;          /* bytes committed to this line */
   size_t last_break = 0;   /* byte offset just past the last space */
   bool overflowed = false;

   while (cursor[len] && cursor[len] != '\n' && len + 1 < TEXT_KEY_MAX) {
      size_t token = len;
      while (cursor[token] && cursor[token] != ' ' && cursor[token] != '\n') {
         token++;
      }
      while (cursor[token] == ' ') {
         token++;
      }
      if (token > TEXT_KEY_MAX - 1) {
         token = TEXT_KEY_MAX - 1;
      }

      memcpy(out, cursor, token);
      out[token] = '\0';

      int w = 0;
      if (measure_text(r, font_id, out, &w, NULL) == 0 && w <= max_width) {
         /* The whole token fits. The spaces in it are now the last place this
          * line could break. Space is never a UTF-8 continuation byte, so this
          * is byte-safe. */
         for (size_t i = len; i < token; i++) {
            if (cursor[i] == ' ') {
               last_break = i + 1;
            }
         }
         len = token;
         continue;
      }

      /* The break is somewhere in this token: find it byte by byte from the
       * length already known to fit, so the line break is the one a prefix walk
       * would have found. */
      bool found = false;
      for (size_t k = len + 1; k <= token; k++) {
         /* Restore the byte the previous pass replaced with a terminator
          * before cutting the string one byte further along: the measurement is
          * of the prefix up to k, not of the first prefix this loop tried. */
         out[k - 1] = cursor[k - 1];
         out[k] = '\0';
         if (cursor[k - 1] == ' ') {
            last_break = k;
         }
         int pw = 0;
         if (measure_text(r, font_id, out, &pw, NULL) == 0 && pw > max_width) {
            len = k;
            overflowed = true;
            found = true;
            break;
         }
      }
      if (!found) {
         /* No prefix of the token measured wider than the column, so the
          * token-level measurement must have failed rather than overflowed
          * (libfreetype could not size something in it). Take the token and move
          * on: a line has to consume something to make progress. */
         len = token;
      }
      break;
   }

   if (overflowed) {
      if (last_break > 0) {
         /* Break at the last space; drop it from the drawn line. */
         len = last_break - 1;
      } else {
         /* One long unbroken run (CJK, a URL). Hard-break at the previous
          * codepoint boundary so a multi-byte sequence is never split. */
         len = utf8_prev(out, len);
         if (len == 0) {
            return false;
         }
      }
   }

   out[len] = '\0';

   const char *tail = cursor + (overflowed && last_break > 0 ? last_break : len);
   while (*tail == ' ') {
      tail++;
   }
   if (*tail == '\n') {
      tail++;
   }

   ln->start = offset;
   ln->len = (int) len;
   ln->next = (int) (tail - text);
   return true;
}

/*
 * Draws one laid-out line at (x, y). `truncated` adds the ellipsis, for the line
 * a draw stops on with text still to come.
 */
static void
draw_wrapped_line(cobalt_render *r, cobalt_font_id font_id, const char *text,
                  const wrap_line *ln, int max_width, bool truncated, int x, int y,
                  SDL_Color colour, const cobalt_text_span *spans, int span_count)
{
   char line[TEXT_KEY_MAX];
   int len = ln->len;
   if (len > TEXT_KEY_MAX - 1) {
      len = TEXT_KEY_MAX - 1;
   }
   memcpy(line, text + ln->start, (size_t) len);
   line[len] = '\0';

   if (truncated) {
      /* Shorten the line until what is left of it and the ellipsis fit. */
      size_t trim = (size_t) len;
      while (trim > 0) {
         char candidate[TEXT_KEY_MAX];
         memcpy(candidate, line, trim);
         candidate[trim] = '\0';
         strncat(candidate, ELLIPSIS, sizeof(candidate) - trim - 1);

         int w = 0;
         if (measure_text(r, font_id, candidate, &w, NULL) == 0 && w <= max_width) {
            memcpy(line, candidate, strlen(candidate) + 1);
            break;
         }
         trim = utf8_prev(line, trim);
      }
   }

   if (spans && span_count > 0) {
      draw_line_spans(r, font_id, font_of(r, font_id), line, ln->start, x, y, colour,
                      spans, span_count);
   } else {
      cobalt_draw_text(r, font_id, line, x, y, colour);
   }
}

/*
 * Lays `utf8` out into `lines`, which must hold `line_cap` of them, and reports
 * how many it took. Returns false when the text needs more lines than that,
 * leaving nothing worth keeping.
 */
static bool
wrap_walk(cobalt_render *r, cobalt_font_id font_id, const char *utf8, int max_width,
          wrap_line *lines, int line_cap, int *out_count)
{
   char line[TEXT_KEY_MAX];
   int offset = 0;
   int count = 0;
   while (utf8[offset]) {
      if (count >= line_cap) {
         return false;
      }
      wrap_line ln;
      if (!next_wrapped_line(r, font_id, utf8, offset, max_width, line, &ln)) {
         break;
      }
      lines[count++] = ln;
      offset = ln.next;
   }
   *out_count = count;
   return true;
}

/*
 * The layout of `utf8` at this width, walking it and keeping the result if it is
 * not there yet. NULL when the string is too long or too tall for an entry, and
 * the caller then lays it out as it draws, exactly as it did before the cache
 * existed.
 */
static const wrap_entry *
wrap_lookup(cobalt_render *r, cobalt_font_id font_id, const char *utf8, int max_width)
{
   const size_t n = strlen(utf8);
   if (n == 0 || n >= WRAP_TEXT_MAX) {
      return NULL;
   }
   const uint32_t hash = hash_string(utf8);

   wrap_entry *spare = NULL;
   wrap_entry *oldest = NULL;
   for (int i = 0; i < WRAP_CACHE_SIZE; i++) {
      wrap_entry *e = &r->wraps[i];
      if (!e->used) {
         if (!spare) spare = e;
         continue;
      }
      if (e->hash == hash && e->font == font_id && e->max_width == max_width &&
          strcmp(e->text, utf8) == 0) {
         e->last_used = ++r->wrap_clock;
         return e;
      }
      if (!oldest || e->last_used < oldest->last_used) {
         oldest = e;
      }
   }

   /* Laid out beside the cache first: a string too tall to keep must not evict
    * a useful entry on its way past. */
   wrap_line lines[WRAP_LINES_MAX];
   int count = 0;
   r->text_wrap_passes++;
   if (!wrap_walk(r, font_id, utf8, max_width, lines, WRAP_LINES_MAX, &count)) {
      return NULL;
   }

   wrap_entry *e = spare ? spare : oldest;
   if (!e) {
      return NULL;
   }
   memcpy(e->text, utf8, n + 1);
   memcpy(e->lines, lines, (size_t) count * sizeof e->lines[0]);
   e->used = true;
   e->font = font_id;
   e->max_width = max_width;
   e->hash = hash;
   e->line_count = count;
   e->last_used = ++r->wrap_clock;
   return e;
}

static int
wrapped_impl(cobalt_render *r, cobalt_font_id font_id, const char *utf8,
             int x, int y, int max_width, int first_line, int max_lines,
             SDL_Color colour, bool draw, int *total_lines,
             const cobalt_text_span *spans, int span_count)
{
   if (total_lines) {
      *total_lines = 0;
   }
   TTF_Font *font = font_of(r, font_id);
   if (!r || !font || !utf8 || max_width <= 0 || max_lines <= 0) {
      return 0;
   }
   r->text_wrap_requests++;

   const int line_height = TTF_FontLineSkip(font) + r->m->line_gap;

   const wrap_entry *entry = wrap_lookup(r, font_id, utf8, max_width);
   if (entry) {
      /* The breaks are known: walk them, skipping to the first drawn line and
       * stopping at the last, and mark the one a cut lands on. */
      const int limit = first_line + max_lines;
      const int walked = entry->line_count < limit ? entry->line_count : limit;
      int used = 0;
      for (int i = first_line; i < walked; i++) {
         const wrap_line *ln = &entry->lines[i];
         if (draw) {
            const bool truncated = (i == walked - 1) && utf8[ln->next] != '\0';
            draw_wrapped_line(r, font_id, utf8, ln, max_width, truncated, x, y + used,
                              colour, spans, span_count);
         }
         used += line_height;
      }
      if (total_lines) {
         *total_lines = walked;
      }
      return used;
   }

   /* Not kept: a compose draft longer than an entry holds, or a text that needs
    * more lines than one can carry. Lay it out and draw it line by line, by the
    * same rule — the only difference is that nothing is remembered. */
   r->text_wrap_passes++;
   char line[TEXT_KEY_MAX];

   int offset = 0;
   int drawn_lines = 0;
   int used_height = 0;

   while (utf8[offset] && drawn_lines < first_line + max_lines) {
      wrap_line ln;
      if (!next_wrapped_line(r, font_id, utf8, offset, max_width, line, &ln)) {
         break;
      }

      /* Ran out of lines with text still to go: mark the truncation. */
      const bool is_last_allowed = (drawn_lines == first_line + max_lines - 1);
      if (drawn_lines >= first_line) {
         if (draw) {
            draw_wrapped_line(r, font_id, utf8, &ln, max_width,
                              is_last_allowed && utf8[ln.next] != '\0', x,
                              y + used_height, colour, spans, span_count);
         }
         used_height += line_height;
      }
      drawn_lines++;
      if (total_lines) {
         *total_lines = drawn_lines;
      }

      offset = ln.next;
   }

   return used_height;
}

int
cobalt_draw_text_wrapped(cobalt_render *r, cobalt_font_id font_id, const char *utf8,
                         int x, int y, int max_width, int max_lines, SDL_Color colour)
{
   return wrapped_impl(r, font_id, utf8, x, y, max_width, 0, max_lines, colour,
                       true, NULL, NULL, 0);
}

int
cobalt_text_wrapped_lines(cobalt_render *r, cobalt_font_id font_id,
                          const char *utf8, int max_width)
{
   int total = 0;
   SDL_Color none = { 0, 0, 0, 0 };
   wrapped_impl(r, font_id, utf8, 0, 0, max_width, 0, 4096, none, false, &total, NULL, 0);
   return total;
}

void
cobalt_render_text_work(cobalt_render *r, uint64_t *measures, uint64_t *wrap_passes,
                        uint64_t *wrap_requests, uint64_t *measure_us)
{
   if (measures) *measures = r ? r->text_measures : 0;
   if (wrap_passes) *wrap_passes = r ? r->text_wrap_passes : 0;
   if (wrap_requests) *wrap_requests = r ? r->text_wrap_requests : 0;
   if (measure_us) *measure_us = r ? r->text_measure_ns / 1000 : 0;
}

void
cobalt_render_reset_text_work(cobalt_render *r)
{
   if (!r) return;
   r->text_measures = 0;
   r->text_wrap_passes = 0;
   r->text_wrap_requests = 0;
   r->text_measure_ns = 0;
}

void
cobalt_render_flush_text_cache(cobalt_render *r)
{
   if (!r) {
      return;
   }
   for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
      if (r->cache[i].tex) {
         SDL_DestroyTexture(r->cache[i].tex);
         r->cache[i].tex = NULL;
      }
      r->cache[i].in_use = false;
   }
}

int
cobalt_draw_text_wrapped_spans(cobalt_render *r, cobalt_font_id font_id,
                               const char *utf8, int x, int y, int max_width,
                               int first_line, int max_lines, SDL_Color colour,
                               const cobalt_text_span *spans, int span_count)
{
   return wrapped_impl(r, font_id, utf8, x, y, max_width, first_line, max_lines,
                       colour, true, NULL, spans, span_count);
}
