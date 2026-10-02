/*
 * Host render snapshot: drives the real app and renderer on SDL's dummy video
 * driver with the software renderer and writes the TV and GamePad frames as
 * BMPs. Checks the frames are drawn, not that they are pretty. See `make snapshot`.
 */
#include "app/app.h"
#include "input/input.h"
#include "ui/render.h"
#include <SDL_ttf.h>

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
   printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int
distinct_colours(SDL_Surface *s)
{
   int n = 0;
   uint32_t seen[64];
   const uint8_t *p = s->pixels;
   for (int y = 0; y < s->h; y += 3)
      for (int x = 0; x < s->w; x += 3) {
         uint32_t v = *(const uint32_t *) (p + y * s->pitch + x * 4);
         int k = 0;
         while (k < n && seen[k] != v) k++;
         if (k == n && n < 64) seen[n++] = v;
      }
   return n;
}

int
main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : ".";
   const char *font = argc > 2 ? argv[2] : "../romfs/font.ttf";
   SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
   SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
   CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
   if (TTF_Init() != 0) { printf("TTF_Init failed\n"); return 1; }

   cobalt_render *tv = cobalt_render_create(COBALT_SURFACE_TV, font, false);
   cobalt_render *drc = cobalt_render_create(COBALT_SURFACE_DRC, font, false);
   CHECK(tv && drc);
   if (!tv || !drc) return 1;
   CHECK(cobalt_render_has_font(tv));

   cobalt_app *app = cobalt_app_create();
   CHECK(app != NULL);
   cobalt_input in;
   cobalt_input_init(&in);

   for (int f = 0; f < 5; f++) {
      uint32_t now = SDL_GetTicks() + f * 16;
      cobalt_input_begin_frame(&in, now);
      cobalt_input_end_frame(&in, now);
      cobalt_app_update(app, &in, now);
      cobalt_render_begin(tv);
      cobalt_app_draw(app, tv, COBALT_SURFACE_TV);
      cobalt_render_end(tv);
      cobalt_render_begin(drc);
      cobalt_app_draw(app, drc, COBALT_SURFACE_DRC);
      cobalt_render_end(drc);
   }

   cobalt_render *rs[2] = { tv, drc };
   const char *names[2] = { "tv", "drc" };
   for (int i = 0; i < 2; i++) {
      SDL_Renderer *rd = cobalt_render_sdl_renderer(rs[i]);
      CHECK(rd != NULL);
      if (!rd) continue;
      int w, h;
      SDL_GetRendererOutputSize(rd, &w, &h);
      SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
      CHECK(SDL_RenderReadPixels(rd, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0);
      int dc = distinct_colours(s);
      printf("%s: %dx%d distinct colours (sampled) %d\n", names[i], w, h, dc);
      CHECK(dc > 4);
      char path[512];
      snprintf(path, sizeof path, "%s/%s.bmp", dir, names[i]);
      SDL_SaveBMP(s, path);
      SDL_FreeSurface(s);
   }

   cobalt_app_destroy(app);
   cobalt_render_destroy(tv);
   cobalt_render_destroy(drc);
   SDL_Quit();
   printf("snapshot: %d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
