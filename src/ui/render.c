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
   /* Same sizes, from a wider-coverage face; NULL when none is available. */
   TTF_Font *fallbacks[COBALT_FONT_COUNT];

   SDL_Texture *gradient;
   SDL_Texture *band;
   int band_h;
   SDL_Texture *corner;
   int corner_radius;

   text_entry cache[TEXT_CACHE_SIZE];
   uint32_t clock;

   /* Borrowed, not owned — see cobalt_render_set_images(). */
   cobalt_imagecache *images;
   /* Borrowed, not owned — see cobalt_render_set_thumbs(). */
   cobalt_imagecache *thumbs;
   /* Borrowed, not owned — see cobalt_render_set_viewer(). */
   cobalt_imagecache *viewer;

   bool warned_long_string;
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

void
cobalt_fill_rect(cobalt_render *r, const SDL_Rect *rect, SDL_Color colour)
{
   if (!r || !rect) {
      return;
   }
   set_draw_colour(r, colour);
   SDL_RenderFillRect(r->renderer, rect);
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
   cobalt_fill_rounded_rect(r, &pill, pill_h / 2, body);
   int tx = x + padx;
   if (has_key) {
      SDL_Rect chip = { x + 4, y + 4, chip_w, pill_h - 8 };
      cobalt_fill_rounded_rect(r, &chip, chip.h / 2, COBALT_COLOUR_ACCENT);
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
   cobalt_fill_rounded_rect(r, &shadow_rect, radius, shadow);

   /* Focused tiles lift slightly and brighten — the Wii U menu's tactile feel
    * (AGENTS.md §5) comes mostly from this rather than from any animation. */
   SDL_Rect body = *rect;
   body.y -= (int) (2.0f * focus);

   if (focus > 0.0f) {
      /* A rounded ring just outside the body: SDL_RenderDrawRect would draw
       * square corners around a rounded tile. */
      SDL_Color edge = COBALT_COLOUR_ACCENT;
      edge.a = (Uint8) (220 * focus);
      const SDL_Rect ring = { body.x - 2, body.y - 2, body.w + 4, body.h + 4 };
      cobalt_fill_rounded_rect(r, &ring, radius + 2, edge);
   }

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

static int
measure_text(cobalt_render *r, cobalt_font_id id, const char *utf8, int *out_w, int *out_h)
{
   const uint32_t t0 = SDL_GetTicks();
   const int rc = measure_text_impl(r, id, utf8, out_w, out_h);
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

   const int line_height = TTF_FontLineSkip(font) + r->m->line_gap;
   char line[TEXT_KEY_MAX];

   const char *cursor = utf8;
   int drawn_lines = 0;
   int used_height = 0;

   while (*cursor && drawn_lines < first_line + max_lines) {
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
         if (measure_text(r, font_id, line, &w, NULL) == 0 && w > max_width) {
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

      bool is_last_allowed = (drawn_lines == first_line + max_lines - 1);
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
            if (measure_text(r, font_id, candidate, &w, NULL) == 0 && w <= max_width) {
               memcpy(line, candidate, strlen(candidate) + 1);
               break;
            }
            trim = utf8_prev(line, trim);
         }
      }

      if (drawn_lines >= first_line) {
         if (draw) {
            if (spans && span_count > 0) {
               draw_line_spans(r, font_id, font, line, (int) (cursor - utf8), x,
                               y + used_height, colour, spans, span_count);
            } else {
               cobalt_draw_text(r, font_id, line, x, y + used_height, colour);
            }
         }
         used_height += line_height;
      }
      drawn_lines++;
      if (total_lines) {
         *total_lines = drawn_lines;
      }

      cursor = tail;
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
