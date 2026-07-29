#include "ui/render.h"
#include "util/log.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cache sized for a full timeline screen's worth of distinct strings plus the
 * chrome around it. Overflow evicts least-recently-used rather than failing. */
#define TEXT_CACHE_SIZE 128

/* Strings longer than this bypass the cache. Wrapped body lines are well under
 * it; the cap exists so the cache is a flat array with no per-entry malloc. */
#define TEXT_KEY_MAX 160

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

struct cobalt_render {
   cobalt_surface_id surface;
   const cobalt_metrics *m;

   SDL_Window *window;
   SDL_Renderer *renderer;
   TTF_Font *fonts[COBALT_FONT_COUNT];

   SDL_Texture *gradient;
   SDL_Texture *corner;
   int corner_radius;

   text_entry cache[TEXT_CACHE_SIZE];
   uint32_t clock;

   /* Borrowed, not owned — see cobalt_render_set_images(). */
   cobalt_imagecache *images;

   bool warned_long_string;
};

/* --- helpers --- */

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

   for (int i = 0; i < COBALT_FONT_COUNT; i++) {
      r->fonts[i] = TTF_OpenFont(font_path, font_size_for(r->m, (cobalt_font_id) i));
      if (!r->fonts[i]) {
         /* Not fatal — see the header. Shapes still draw, text is skipped. */
         COBALT_LOGE("font %s @%dpt failed to open: %s", font_path,
                     font_size_for(r->m, (cobalt_font_id) i), TTF_GetError());
      }
   }

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
   if (r->gradient) SDL_DestroyTexture(r->gradient);

   for (int i = 0; i < COBALT_FONT_COUNT; i++) {
      if (r->fonts[i]) {
         TTF_CloseFont(r->fonts[i]);
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
cobalt_render_begin(cobalt_render *r)
{
   if (!r) {
      return;
   }

   r->clock++;

   if (r->gradient) {
      SDL_Rect dst = { 0, 0, r->m->width, r->m->height };
      SDL_RenderCopy(r->renderer, r->gradient, NULL, &dst);
   } else {
      set_draw_colour(r, COBALT_COLOUR_BG_BOTTOM);
      SDL_RenderClear(r->renderer);
   }
}

void
cobalt_render_end(cobalt_render *r)
{
   if (r) {
      SDL_RenderPresent(r->renderer);
   }
}

/* --- primitives --- */

void
cobalt_fill_rect(cobalt_render *r, const SDL_Rect *rect, SDL_Color colour)
{
   if (!r || !rect) {
      return;
   }
   set_draw_colour(r, colour);
   SDL_RenderFillRect(r->renderer, rect);
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
   if (middle.h > 0) SDL_RenderFillRect(r->renderer, &middle);
   if (top.w > 0)    SDL_RenderFillRect(r->renderer, &top);
   if (bottom.w > 0) SDL_RenderFillRect(r->renderer, &bottom);

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
   SDL_Color shadow = { 0x00, 0x10, 0x20, (Uint8) (48 + 40 * focus) };
   SDL_Rect shadow_rect = { rect->x + 2, rect->y + 3 + (int) (2 * focus),
                            rect->w, rect->h };
   cobalt_fill_rounded_rect(r, &shadow_rect, radius, shadow);

   /* Focused tiles lift slightly and brighten — the Wii U menu's tactile feel
    * (AGENTS.md §5) comes mostly from this rather than from any animation. */
   SDL_Rect body = *rect;
   body.y -= (int) (2.0f * focus);

   SDL_Color base = focus > 0.0f ? COBALT_COLOUR_TILE_FOCUS : COBALT_COLOUR_TILE;
   cobalt_fill_rounded_rect(r, &body, radius, base);

   /* Top sheen: a translucent band across the upper third, giving the glassy
    * highlight the flat-card look lacks. */
   SDL_Rect sheen = { body.x + radius / 2, body.y + 1,
                      body.w - radius, body.h / 3 };
   if (sheen.w > 0 && sheen.h > 0) {
      SDL_Color gloss = { 0xFF, 0xFF, 0xFF, (Uint8) (60 + 40 * focus) };
      cobalt_fill_rounded_rect(r, &sheen, radius / 2, gloss);
   }

   if (focus > 0.0f) {
      SDL_Color edge = COBALT_COLOUR_ACCENT;
      edge.a = (Uint8) (200 * focus);
      set_draw_colour(r, edge);
      SDL_RenderDrawRect(r->renderer, &body);
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

/* --- text --- */

static TTF_Font *
font_of(cobalt_render *r, cobalt_font_id id)
{
   if (!r || id < 0 || id >= COBALT_FONT_COUNT) {
      return NULL;
   }
   return r->fonts[id];
}

/* Render straight to a texture, bypassing the cache. Caller destroys it. */
static SDL_Texture *
render_uncached(cobalt_render *r, TTF_Font *font, const char *utf8, SDL_Color colour,
                int *out_w, int *out_h)
{
   SDL_Surface *surface = TTF_RenderUTF8_Blended(font, utf8, colour);
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
   SDL_Texture *tex = render_uncached(r, font, utf8, colour, &w, &h);
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
   SDL_Texture *tex = render_uncached(r, font, utf8, colour, &w, &h);
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
   if (TTF_SizeUTF8(font, utf8, &w, &h) == 0) {
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

int
cobalt_draw_text_wrapped(cobalt_render *r, cobalt_font_id font_id, const char *utf8,
                         int x, int y, int max_width, int max_lines, SDL_Color colour)
{
   TTF_Font *font = font_of(r, font_id);
   if (!r || !font || !utf8 || max_width <= 0 || max_lines <= 0) {
      return 0;
   }

   const int line_height = TTF_FontLineSkip(font) + r->m->line_gap;
   char line[TEXT_KEY_MAX];

   const char *cursor = utf8;
   int drawn_lines = 0;
   int used_height = 0;

   while (*cursor && drawn_lines < max_lines) {
      size_t len = 0;          /* bytes committed to this line */
      size_t last_break = 0;   /* byte offset just past the last space */
      bool overflowed = false;

      /* Grow the line one byte at a time, remembering the last point we could
       * break at, and stop as soon as it no longer fits. */
      while (cursor[len] && cursor[len] != '\n' && len + 1 < sizeof(line)) {
         line[len] = cursor[len];
         line[len + 1] = '\0';
         len++;

         /* Space is never a UTF-8 continuation byte, so this is byte-safe. */
         if (cursor[len - 1] == ' ') {
            last_break = len;
         }

         int w = 0;
         if (TTF_SizeUTF8(font, line, &w, NULL) == 0 && w > max_width) {
            overflowed = true;
            break;
         }
      }

      if (overflowed) {
         if (last_break > 0) {
            /* Break at the last space; drop it from the drawn line. */
            len = last_break - 1;
         } else {
            /* One long unbroken run (CJK, a URL). Hard-break at the previous
             * codepoint boundary so a multi-byte sequence is never split. */
            len = utf8_prev(line, len);
            if (len == 0) {
               break; /* Single glyph wider than max_width; nothing sensible left. */
            }
         }
      }

      line[len] = '\0';

      bool is_last_allowed = (drawn_lines == max_lines - 1);
      const char *tail = cursor + (overflowed && last_break > 0 ? last_break : len);
      while (*tail == ' ') {
         tail++;
      }
      if (*tail == '\n') {
         tail++;
      }

      /* Ran out of lines with text still to go: mark the truncation. */
      if (is_last_allowed && *tail != '\0') {
         size_t trim = len;
         while (trim > 0) {
            char candidate[TEXT_KEY_MAX];
            memcpy(candidate, line, trim);
            candidate[trim] = '\0';
            strncat(candidate, ELLIPSIS, sizeof(candidate) - trim - 1);

            int w = 0;
            if (TTF_SizeUTF8(font, candidate, &w, NULL) == 0 && w <= max_width) {
               memcpy(line, candidate, strlen(candidate) + 1);
               break;
            }
            trim = utf8_prev(line, trim);
         }
      }

      cobalt_draw_text(r, font_id, line, x, y + used_height, colour);
      used_height += line_height;
      drawn_lines++;

      cursor = tail;
   }

   return used_height;
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
