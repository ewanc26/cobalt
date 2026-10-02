/*
 * Live host simulator: the real app and renderer in two SDL windows sized like
 * the Wii U outputs (TV 1280x720, GamePad 854x480), so UI work can be done on a
 * Mac without Cemu. See `make sim`.
 *
 *   keys: arrows = D-pad, Return/Space = A, Esc/Backspace = B, Tab = +,
 *         X = X, Y = Y, F5 = save BMPs of both screens.
 *         Left-click in the GamePad window = touch.
 *   A real game controller also works (SDL maps it like the Wii U Pro pad).
 *
 * Runs against the shared mock PDS by default; --live uses the real network
 * and the app's own sign-in screen. State lives in the out dir (arg after
 * flags) so nothing touches a real account.
 */
#include "app/app.h"
#include "atproto/session.h"
#include "fixtures.h"
#include "input/input.h"
#include "ui/render.h"
#include <SDL_ttf.h>

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void cobalt_test_set_root(const char *path);

static cobalt_render *g_tv, *g_drc;
static cobalt_input g_in;
static SDL_Window *g_drc_win;

static bool
key_button(SDL_Keycode k, cobalt_button *out)
{
   switch (k) {
      case SDLK_UP:        *out = COBALT_BTN_UP;      return true;
      case SDLK_DOWN:      *out = COBALT_BTN_DOWN;    return true;
      case SDLK_LEFT:      *out = COBALT_BTN_LEFT;    return true;
      case SDLK_RIGHT:     *out = COBALT_BTN_RIGHT;   return true;
      case SDLK_RETURN:
      case SDLK_SPACE:     *out = COBALT_BTN_CONFIRM; return true;
      case SDLK_ESCAPE:
      case SDLK_BACKSPACE: *out = COBALT_BTN_BACK;    return true;
      case SDLK_TAB:       *out = COBALT_BTN_MENU;    return true;
      case SDLK_x:         *out = COBALT_BTN_ALT_X;   return true;
      case SDLK_y:         *out = COBALT_BTN_ALT_Y;   return true;
      default: return false;
   }
}

static void
press_button(cobalt_button b, bool down, uint32_t now)
{
   if (down && !g_in.held[b]) {
      g_in.held[b] = true;
      g_in.pressed[b] = true;
      g_in.held_since[b] = now;
      g_in.next_repeat[b] = now + 400;
   } else if (!down) {
      g_in.held[b] = false;
      g_in.next_repeat[b] = 0;
   }
}

static void
touch(const SDL_Event *e, bool down, bool up)
{
   int w = 0, h = 0;
   SDL_GetWindowSize(g_drc_win, &w, &h);
   if (w <= 0 || h <= 0) return;
   int x = (down || up) ? e->button.x : e->motion.x;
   int y = (down || up) ? e->button.y : e->motion.y;
   g_in.touch_x = x * COBALT_DRC_WIDTH / w;
   g_in.touch_y = y * COBALT_DRC_HEIGHT / h;
   if (down) { g_in.touch_down = true; g_in.touch_began = true; }
   if (up) { g_in.touch_down = false; g_in.touch_ended = true; }
}

static void
shoot(const char *dir)
{
   cobalt_render *rs[2] = { g_tv, g_drc };
   const char *names[2] = { "tv", "drc" };
   for (int i = 0; i < 2; i++) {
      SDL_Renderer *rd = cobalt_render_sdl_renderer(rs[i]);
      int w, h;
      SDL_GetRendererOutputSize(rd, &w, &h);
      SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
      if (SDL_RenderReadPixels(rd, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
         char path[512];
         snprintf(path, sizeof path, "%s/sim-%s.bmp", dir, names[i]);
         SDL_SaveBMP(s, path);
         printf("saved %s\n", path);
      }
      SDL_FreeSurface(s);
   }
}

int
main(int argc, char **argv)
{
   bool live = false;
   const char *dir = "build/sim", *font = "romfs/font.ttf";
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--live")) live = true;
      else if (!strcmp(argv[i], "--font") && i + 1 < argc) font = argv[++i];
      else dir = argv[i];
   }
   mkdir("build", 0777);
   mkdir(dir, 0777);
   cobalt_test_set_root(dir);

   SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0 ||
       TTF_Init() != 0) {
      fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
      return 1;
   }
   (void) cobalt_session_init();
   g_tv = cobalt_render_create(COBALT_SURFACE_TV, font, false);
   g_drc = cobalt_render_create(COBALT_SURFACE_DRC, font, false);
   if (!g_tv || !g_drc) return 1;
   {
      SDL_Window *tvw = SDL_RenderGetWindow(cobalt_render_sdl_renderer(g_tv));
      g_drc_win = SDL_RenderGetWindow(cobalt_render_sdl_renderer(g_drc));
      SDL_SetWindowPosition(tvw, 20, 40);
      SDL_SetWindowPosition(g_drc_win, 20 + COBALT_TV_WIDTH + 20, 40);
   }
   cobalt_app *app = cobalt_app_create();
   cobalt_input_init(&g_in);

   wf_mock_pds *pds = NULL;
   if (!live) {
      int port = 0;
      if (wf_mock_pds_start(&pds, &port) != WF_OK) return 1;
      wf_mock_pds_register(pds, "com.atproto.server.createSession",
         "{\"did\":\"did:plc:abc\",\"handle\":\"alice.test\","
         "\"accessJwt\":\"a.b.c\",\"refreshJwt\":\"d.e.f\",\"active\":true}");
      register_fixtures(pds);
      char svc[64];
      snprintf(svc, sizeof svc, "http://127.0.0.1:%d", port);
      (void) cobalt_session_begin_login(svc, "alice.test", "app-pass");
   }

   printf("Cobalt sim: arrows, Return=A, Esc=B, Tab=+, X, Y, click GamePad to touch, F5 screenshots\n");
   bool quit = false;
   while (!quit) {
      const uint32_t now = SDL_GetTicks();
      cobalt_input_begin_frame(&g_in, now);
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
         cobalt_button b;
         switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_WINDOWEVENT:
               if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
               break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
               if (e.key.repeat) break;
               if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F5) shoot(dir);
               else if (key_button(e.key.keysym.sym, &b)) press_button(b, e.type == SDL_KEYDOWN, now);
               break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
               if (e.button.windowID == SDL_GetWindowID(g_drc_win) && e.button.button == SDL_BUTTON_LEFT)
                  touch(&e, e.type == SDL_MOUSEBUTTONDOWN, e.type == SDL_MOUSEBUTTONUP);
               break;
            case SDL_MOUSEMOTION:
               if (e.motion.windowID == SDL_GetWindowID(g_drc_win) && g_in.touch_down)
                  touch(&e, false, false);
               break;
            default:
               cobalt_input_handle_event(&g_in, &e);
               break;
         }
      }
      if (g_in.quit_requested) quit = true;
      cobalt_input_end_frame(&g_in, now);

      cobalt_app_update(app, &g_in, now);
      cobalt_render_begin(g_tv);
      cobalt_app_draw(app, g_tv, COBALT_SURFACE_TV);
      cobalt_render_end(g_tv);
      cobalt_render_begin(g_drc);
      cobalt_app_draw(app, g_drc, COBALT_SURFACE_DRC);
      cobalt_render_end(g_drc);
      SDL_Delay(8);
   }

   if (pds) wf_mock_pds_free(pds);
   cobalt_app_destroy(app);
   cobalt_render_destroy(g_tv);
   cobalt_render_destroy(g_drc);
   SDL_Quit();
   return 0;
}
