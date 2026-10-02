/*
 * Host render snapshot: drives the real app and renderer on SDL's dummy video
 * driver with the software renderer and writes the TV and GamePad frames as
 * BMPs. Checks the frames are drawn, not that they are pretty. See `make snapshot`.
 */
#include "app/app.h"
#include "atproto/session.h"
#include "../../wolfram/test/mock_pds.h"
#include "input/input.h"
#include "ui/render.h"
#include "fixtures.h"
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

static cobalt_render *g_tv, *g_drc;
static cobalt_app *g_app;
static cobalt_input g_in;
static const char *g_dir;

static void
frame(cobalt_button press)
{
   uint32_t now = SDL_GetTicks();
   cobalt_input_begin_frame(&g_in, now);
   if (press != COBALT_BTN_COUNT) g_in.pressed[press] = true;
   cobalt_input_end_frame(&g_in, now);
   cobalt_app_update(g_app, &g_in, now);
   cobalt_render_begin(g_tv);
   cobalt_app_draw(g_app, g_tv, COBALT_SURFACE_TV);
   cobalt_render_end(g_tv);
   cobalt_render_begin(g_drc);
   cobalt_app_draw(g_app, g_drc, COBALT_SURFACE_DRC);
   cobalt_render_end(g_drc);
}

static void
settle(int n)
{
   for (int i = 0; i < n; i++) { frame(COBALT_BTN_COUNT); SDL_Delay(16); }
}

static void
shoot(const char *scene)
{
   cobalt_render *rs[2] = { g_tv, g_drc };
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
      printf("%s/%s: %dx%d distinct colours (sampled) %d\n", scene, names[i], w, h, dc);
      CHECK(dc > 4);
      char path[512];
      snprintf(path, sizeof path, "%s/%s-%s.bmp", g_dir, scene, names[i]);
      SDL_SaveBMP(s, path);
      SDL_FreeSurface(s);
   }
}

/* Back is a no-op on home, so spamming it reaches home from anywhere. */
static void
go_home(void)
{
   for (int i = 0; i < 8; i++) {
      frame(COBALT_BTN_BACK);
      settle(4);
   }
}

/* Home selection is linear and wraps; the app remembers it across screens. */
static void
open_home_item(int idx)
{
   go_home();
   {
      /* TV grid: Down jumps a row of 5 and Up comes back; wraps like the list. */
      const int start = cobalt_app_home_selection(g_app);
      frame(COBALT_BTN_DOWN); settle(2);
      CHECK(cobalt_app_home_selection(g_app) == (start + 5) % 10);
      frame(COBALT_BTN_UP); settle(2);
      CHECK(cobalt_app_home_selection(g_app) == start);
   }
   for (int n = (idx - cobalt_app_home_selection(g_app) + 10) % 10; n > 0; n--) {
      frame(COBALT_BTN_RIGHT);
      settle(2);
   }
   if (cobalt_app_home_selection(g_app) != idx) printf("nav: wanted %d got %d\n", idx, cobalt_app_home_selection(g_app));
   CHECK(cobalt_app_home_selection(g_app) == idx);
   settle(3);
   frame(COBALT_BTN_CONFIRM);
}


int
main(int argc, char **argv)
{
   g_dir = argc > 1 ? argv[1] : ".";
   const char *font = argc > 2 ? argv[2] : "../romfs/font.ttf";
   SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
   SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
   CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
   if (TTF_Init() != 0) { printf("TTF_Init failed\n"); return 1; }
   (void) cobalt_session_init();

   g_tv = cobalt_render_create(COBALT_SURFACE_TV, font, false);
   g_drc = cobalt_render_create(COBALT_SURFACE_DRC, font, false);
   CHECK(g_tv && g_drc);
   if (!g_tv || !g_drc) return 1;
   CHECK(cobalt_render_has_font(g_tv));
   g_app = cobalt_app_create();
   CHECK(g_app != NULL);
   cobalt_input_init(&g_in);

   settle(5);
   shoot("home-signedout");

   wf_mock_pds *pds = NULL;
   int port = 0;
   CHECK(wf_mock_pds_start(&pds, &port) == WF_OK);
   register_fixtures(pds);

   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", port);
   CHECK(cobalt_session_begin_login(svc, "alice.test", "app-pass"));
   for (int i = 0; i < 300 && cobalt_session_state() != COBALT_AUTH_SIGNED_IN; i++) {
      cobalt_job_result jr;
      (void) cobalt_session_poll(&jr);
      SDL_Delay(5);
   }
   CHECK(cobalt_session_state() == COBALT_AUTH_SIGNED_IN);
   settle(30);
   shoot("timeline");
   for (int k = 2; k <= 5; k++) {
      frame(COBALT_BTN_DOWN); settle(25);
      char nm[24]; snprintf(nm, sizeof nm, "timeline-%d", k);
      shoot(nm);
   }
   for (int k = 0; k < 5; k++) { frame(COBALT_BTN_UP); settle(3); }
   settle(20);

   frame(COBALT_BTN_CONFIRM);
   settle(30);
   shoot("thread");

   frame(COBALT_BTN_BACK);
   settle(10);
   frame(COBALT_BTN_ALT_Y);
   settle(40);
   shoot("profile");

   frame(COBALT_BTN_BACK);
   settle(10);
   frame(COBALT_BTN_BACK);
   settle(10);
   shoot("home-signedin");

   open_home_item(5);
   settle(40);
   shoot("notifications");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(7);
   settle(10);
   shoot("diagnostics");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(1);
   settle(10);
   shoot("compose");

   /* B is backspace on the keyboard, so leave through its Cancel key: the
    * focus starts on row 2, so two Downs reach the function row and Left
    * wraps from column 0 to Cancel. */
   for (int k = 0; k < 2; k++) {
      frame(COBALT_BTN_DOWN);
      settle(2);
   }
   frame(COBALT_BTN_LEFT);   /* wraps to Emoji, the last key */
   settle(2);
   frame(COBALT_BTN_LEFT);   /* then Cancel */
   settle(2);
   frame(COBALT_BTN_CONFIRM);
   settle(5);
   open_home_item(6);
   settle(10);
   shoot("account");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(2);
   settle(10);
   shoot("search");

   for (int k = 0; k < 2; k++) { frame(COBALT_BTN_DOWN); settle(2); }
   frame(COBALT_BTN_LEFT);
   settle(2);
   frame(COBALT_BTN_LEFT);
   settle(2);
   frame(COBALT_BTN_CONFIRM);
   settle(5);
   open_home_item(3);
   settle(10);
   settle(30);
   CHECK(cobalt_session_saved_feeds()->count == 2);
   CHECK(strcmp(cobalt_session_saved_feeds()->feeds[0].label, "Cat Pics") == 0);
   CHECK(strcmp(cobalt_session_saved_feeds()->feeds[1].label, "poetry") == 0);
   shoot("feeds");

   frame(COBALT_BTN_BACK);
   settle(10);
   open_home_item(4);
   settle(40);
   shoot("lists");

   /* Tapping the header's Back pill on the GamePad leaves the screen. */
   {
      uint32_t now = SDL_GetTicks();
      cobalt_input_begin_frame(&g_in, now);
      g_in.touch_x = 800; g_in.touch_y = 38; g_in.touch_ended = true;
      cobalt_input_end_frame(&g_in, now);
      cobalt_app_update(g_app, &g_in, now);
      g_in.touch_ended = false;
      settle(3);
      CHECK(cobalt_app_screen(g_app) == COBALT_SCREEN_HOME);
   }

   wf_mock_pds_free(pds);
   cobalt_app_destroy(g_app);
   cobalt_render_destroy(g_tv);
   cobalt_render_destroy(g_drc);
   SDL_Quit();
   printf("snapshot: %d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
