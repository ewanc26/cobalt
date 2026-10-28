/*
 * Host frame benchmark: the real app and renderer on SDL's dummy video driver,
 * driven against Wolfram's mock PDS, timing each phase the way src/main.c does
 * on a console (update, TV draw, GamePad draw) and counting the text work
 * behind them. See `make bench`.
 *
 * This measures the work a frame does, not the frame rate a Wii U would show:
 * the host rasteriser and the console's GPU are different halves of the cost.
 * It exists to compare a before against an after on one machine. Anything it
 * says still has to be confirmed on hardware (#24).
 *
 *   tests/bench_main [data-dir] [font-path] [frames]
 */
#include "app/app.h"
#include "app/app_internal.h"
#include "atproto/session.h"
#include "input/input.h"
#include "ui/render.h"
#include "util/paths.h"
#include "fixtures.h"
#include <SDL_ttf.h>

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in host_stubs.c, next to the stubbed paths/log it drives. */
void cobalt_test_set_root(const char *path);

static cobalt_render *g_tv, *g_drc;
static cobalt_app *g_app;
static cobalt_input g_in;

/* What one scenario cost across its frames. */
typedef struct {
   const char *name;
   uint32_t frames;
   double update_ms, tv_ms, drc_ms, worst_ms;
   uint64_t measures, wrap_passes, wrap_requests, measure_us;
} bench_run;

static void
frame(cobalt_button press, bench_run *b)
{
   const uint32_t now = SDL_GetTicks();
   cobalt_input_begin_frame(&g_in, now);
   if (press != COBALT_BTN_COUNT) g_in.pressed[press] = true;
   cobalt_input_end_frame(&g_in, now);

   if (b) {
      cobalt_render_reset_text_work(g_tv);
      cobalt_render_reset_text_work(g_drc);
   }

   const uint32_t t_update = SDL_GetTicks();
   cobalt_app_update(g_app, &g_in, now);

   const uint32_t t_tv = SDL_GetTicks();
   cobalt_render_begin(g_tv);
   cobalt_app_draw(g_app, g_tv, COBALT_SURFACE_TV);
   cobalt_render_end(g_tv);

   const uint32_t t_drc = SDL_GetTicks();
   cobalt_render_begin(g_drc);
   cobalt_app_draw(g_app, g_drc, COBALT_SURFACE_DRC);
   cobalt_render_end(g_drc);

   const uint32_t t_end = SDL_GetTicks();
   if (!b) return;

   uint64_t m_tv = 0, w_tv = 0, q_tv = 0, u_tv = 0;
   uint64_t m_drc = 0, w_drc = 0, q_drc = 0, u_drc = 0;
   cobalt_render_text_work(g_tv, &m_tv, &w_tv, &q_tv, &u_tv);
   cobalt_render_text_work(g_drc, &m_drc, &w_drc, &q_drc, &u_drc);

   b->frames++;
   b->update_ms += (double) (t_tv - t_update);
   b->tv_ms += (double) (t_drc - t_tv);
   b->drc_ms += (double) (t_end - t_drc);
   b->measures += m_tv + m_drc;
   b->wrap_passes += w_tv + w_drc;
   b->wrap_requests += q_tv + q_drc;
   b->measure_us += u_tv + u_drc;
   if (t_end - now > b->worst_ms) b->worst_ms = (double) (t_end - now);
}

static void
settle(int n)
{
   for (int i = 0; i < n; i++) { frame(COBALT_BTN_COUNT, NULL); SDL_Delay(16); }
}

/* Back is a no-op on home, so spamming it reaches home from anywhere. */
static void
go_home(void)
{
   for (int i = 0; i < 8; i++) { frame(COBALT_BTN_BACK, NULL); settle(4); }
}

/* Home selection is linear and wraps; the app remembers it across screens. */
static void
open_home_item(int idx)
{
   go_home();
   settle(4);
   const int count = COBALT_HOME_MENU_COUNT;
   const int steps = (idx - cobalt_app_home_selection(g_app) + count) % count;
   for (int n = 0; n < steps; n++) { frame(COBALT_BTN_RIGHT, NULL); settle(2); }
   if (cobalt_app_home_selection(g_app) != idx) {
      printf("bench: nav wanted %d, got %d\n", idx, cobalt_app_home_selection(g_app));
      return;
   }
   settle(3);
   frame(COBALT_BTN_CONFIRM, NULL);
}

static void
report(const bench_run *b)
{
   const double f = b->frames ? (double) b->frames : 1.0;
   printf("%-14s %4u frames | ms/frame  update %6.3f  tv %7.3f  drc %6.3f | worst %4.0f"
          " | TTF_Size %6.1f %8.0f us  wraps %5.1f/%5.1f asked\n",
          b->name, b->frames, b->update_ms / f, b->tv_ms / f, b->drc_ms / f,
          b->worst_ms, (double) b->measures / f, (double) b->measure_us / f,
          (double) b->wrap_passes / f, (double) b->wrap_requests / f);
}

/*
 * Runs `frames` measured frames on whatever screen is up, after a short warm-up
 * that is not measured: the first frames on a screen fill the caches a steady
 * state then hits, and reporting them would describe the arrival rather than the
 * screen.
 */
static void
measure(const char *name, int frames, cobalt_button press)
{
   for (int i = 0; i < 12; i++) {
      frame(press, NULL);
      SDL_Delay(2);
   }

   bench_run b;
   memset(&b, 0, sizeof b);
   b.name = name;
   for (int i = 0; i < frames; i++) {
      frame(press, &b);
      SDL_Delay(2);
   }
   report(&b);
}

int
main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : "build/bench";
   const char *font = argc > 2 ? argv[2] : "../romfs/font.ttf";
   const int frames = argc > 3 ? atoi(argv[3]) : 300;

   SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
   SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
      printf("SDL init failed: %s\n", SDL_GetError());
      return 1;
   }
   if (TTF_Init() != 0) { printf("TTF_Init failed\n"); return 1; }
   cobalt_test_set_root(dir);
   (void) cobalt_session_init();

   g_tv = cobalt_render_create(COBALT_SURFACE_TV, font, false);
   g_drc = cobalt_render_create(COBALT_SURFACE_DRC, font, false);
   if (!g_tv || !g_drc) { printf("renderer creation failed (%s)\n", font); return 1; }
   if (!cobalt_render_has_font(g_tv)) { printf("bench: no font at %s\n", font); return 1; }
   g_app = cobalt_app_create();
   cobalt_input_init(&g_in);

   wf_mock_pds *pds = NULL;
   int port = 0;
   if (wf_mock_pds_start(&pds, &port) != WF_OK) { printf("bench: mock PDS failed\n"); return 1; }
   register_fixtures(pds);

   char svc[64];
   snprintf(svc, sizeof svc, "http://127.0.0.1:%d", port);
   if (!cobalt_session_begin_login(svc, "ewancroft.test", "app-pass")) {
      printf("bench: login refused\n");
      return 1;
   }
   const uint32_t deadline = SDL_GetTicks() + 10000;
   while (cobalt_session_state() != COBALT_AUTH_SIGNED_IN && SDL_GetTicks() < deadline) {
      cobalt_job_result jr;
      (void) cobalt_session_poll(&jr);
      SDL_Delay(5);
   }
   if (cobalt_session_state() != COBALT_AUTH_SIGNED_IN) {
      printf("bench: sign-in did not finish against the mock PDS (state %d)\n",
             (int) cobalt_session_state());
      return 1;
   }

   settle(10);
   open_home_item(0);
   settle(60);
   if (cobalt_app_screen(g_app) != COBALT_SCREEN_TIMELINE) {
      printf("bench: did not reach the timeline (screen %d)\n", (int) cobalt_app_screen(g_app));
      return 1;
   }
   const cobalt_feed *feed = cobalt_session_feed();
   printf("cobalt bench: %d posts, %d frames/scenario, %dx%d TV, %dx%d GamePad\n",
          feed->count, frames, 1280, 720, 854, 480);

   /* The timeline: the dense scrolling list on both surfaces, and on the TV the
    * detail column beside it — the screen that wraps the most text. */
   measure("timeline", frames, COBALT_BTN_COUNT);
   measure("timeline down", frames, COBALT_BTN_DOWN);
   measure("timeline up", frames, COBALT_BTN_UP);

   /* A thread: one long post body wrapped over and over as it scrolls. */
   open_home_item(0);
   settle(3);
   frame(COBALT_BTN_CONFIRM, NULL);
   settle(60);
   printf("bench: screen %d after opening a thread\n", (int) cobalt_app_screen(g_app));
   measure("thread", frames, COBALT_BTN_COUNT);
   measure("thread scroll", frames, COBALT_BTN_DOWN);

   /* Notifications: a second list with its own card shapes. */
   open_home_item(1);
   settle(60);
   printf("bench: screen %d after opening the next home entry\n", (int) cobalt_app_screen(g_app));
   measure("second screen", frames, COBALT_BTN_DOWN);

   wf_mock_pds_free(pds);
   cobalt_app_destroy(g_app);
   cobalt_render_destroy(g_tv);
   cobalt_render_destroy(g_drc);
   SDL_Quit();
   return 0;
}
