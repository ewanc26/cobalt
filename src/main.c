/*
 * Cobalt — a native AT Protocol / Bluesky client for the Wii U (Aroma).
 *
 * Entry point and frame loop only; everything else lives under app/, ui/,
 * input/, net/ and util/.
 *
 * ProcUI ownership
 * ----------------
 * Cobalt does NOT call ProcUIInit/ProcUIProcessMessages/ProcUIShutdown. The
 * SDL2 Wii U video driver already does: WIIU_VideoInit calls ProcUIInitEx and
 * registers the acquire/release/save callbacks, WIIU_PumpEvents calls
 * ProcUIProcessMessages and translates the result into SDL_QUIT and the
 * SDL_APP_* lifecycle events, and WIIU_VideoQuit calls ProcUIShutdown.
 *
 * Driving ProcUI from here as well would mean two consumers racing for the
 * same message queue — each ProcUIProcessMessages call consumes messages, so
 * whichever loop ran first would eat the foreground-release notifications the
 * other needed. That is a fast route to an app that will not return to the
 * Wii U Menu. AGENTS.md §9's "handle every foreground/background transition"
 * requirement is therefore met by handling the SDL_APP_* events below.
 */

#include "app/app.h"
#include "atproto/atproto.h"
#include "atproto/session.h"
#include "input/input.h"
#include "net/net.h"
#include "ui/imagecache.h"
#include "ui/postcard.h"
#include "ui/render.h"
#include "ui/theme.h"
#include "util/log.h"
#include "util/paths.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>

#define FONT_FILE "font.ttf"

/* Frame budget. The Wii U presents at 60Hz; this only bounds the loop when
 * vsync is not gating it (for example while backgrounded). */
#define FRAME_DELAY_MS 16

typedef struct {
   cobalt_render *tv;
   cobalt_render *drc;
   cobalt_app *app;
   cobalt_input input;

   /* One per surface: a texture belongs to the renderer that made it, and the
    * two surfaces have separate renderers. */
   cobalt_imagecache *tv_images;
   cobalt_imagecache *drc_images;

   bool sdl_up;
   bool ttf_up;
   bool net_up;
   bool images_up;
   bool foreground;
   bool running;
} cobalt_context;

static void
shutdown_all(cobalt_context *ctx)
{
   /* Reverse creation order. The Wii U does not forgive leaked GPU resources
    * (AGENTS.md §9), so this runs on every exit path including failures. */
   if (ctx->app) {
      cobalt_app_destroy(ctx->app);
      ctx->app = NULL;
   }

   /* Before cobalt_atproto_shutdown(): the worker thread owns a wf_session, so
    * it has to be joined and its session freed while Wolfram's platform layer
    * is still up. */
   cobalt_session_shutdown();

   cobalt_atproto_shutdown();

   cobalt_input_shutdown();

   if (ctx->net_up) {
      cobalt_net_shutdown();
      ctx->net_up = false;
   }

   /*
    * Before the renderers: destroying a cache joins its loader threads and then
    * frees textures, and both have to finish while the renderer that owns those
    * textures is still alive.
    */
   if (ctx->drc_images) {
      cobalt_imagecache_destroy(ctx->drc_images);
      ctx->drc_images = NULL;
   }
   if (ctx->tv_images) {
      cobalt_imagecache_destroy(ctx->tv_images);
      ctx->tv_images = NULL;
   }
   if (ctx->images_up) {
      cobalt_images_shutdown();
      ctx->images_up = false;
   }

   if (ctx->drc) {
      cobalt_render_destroy(ctx->drc);
      ctx->drc = NULL;
   }
   if (ctx->tv) {
      cobalt_render_destroy(ctx->tv);
      ctx->tv = NULL;
   }

   if (ctx->ttf_up) {
      TTF_Quit();
      ctx->ttf_up = false;
   }
   if (ctx->sdl_up) {
      /* Takes ProcUI down with it — see the note at the top of this file. */
      SDL_Quit();
      ctx->sdl_up = false;
   }

   COBALT_LOGI("cobalt shutting down");
   cobalt_log_shutdown();
   cobalt_paths_shutdown();
}

static bool
startup(cobalt_context *ctx)
{
   /* Paths first: logging wants a file sink on the SD card. */
   bool have_content = cobalt_paths_init();

   cobalt_log_init();
   cobalt_paths_log();

   if (!have_content) {
      COBALT_LOGE("bundled content not found — check the WUHB was built with "
                  "romfs/ as its content directory");
      /* Not fatal: the app still boots and the diagnostics screen will say so,
       * which is more useful on hardware than a silent failure to start. */
   }

   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
      COBALT_LOGE("SDL_Init failed: %s", SDL_GetError());
      return false;
   }
   ctx->sdl_up = true;

   if (TTF_Init() < 0) {
      COBALT_LOGE("TTF_Init failed: %s", TTF_GetError());
      return false;
   }
   ctx->ttf_up = true;

   char font_path[COBALT_PATH_MAX];
   if (!cobalt_content_path(font_path, sizeof(font_path), FONT_FILE)) {
      /* Give TTF_OpenFont something to fail on with a legible message rather
       * than passing it an uninitialised buffer. */
      snprintf(font_path, sizeof(font_path), "%s", FONT_FILE);
      COBALT_LOGE("could not resolve a path for %s", FONT_FILE);
   }
   COBALT_LOGI("font path: %s", font_path);

   /* TV is created with prevent_swap so the GamePad, presented second,
    * performs the single scanbuffer swap for the frame. */
   ctx->tv = cobalt_render_create(COBALT_SURFACE_TV, font_path, true);
   if (!ctx->tv) {
      return false;
   }

   ctx->drc = cobalt_render_create(COBALT_SURFACE_DRC, font_path, false);
   if (!ctx->drc) {
      return false;
   }

   ctx->net_up = cobalt_net_init();

   /* Not fatal: the diagnostics screen reports the SDK status either way. */
   cobalt_atproto_init();

   /* Also not fatal. It returns false when signing in could not work — no
    * Wolfram, or no bundled trust store — and the app still boots so the
    * diagnostics screen can say which. */
   cobalt_session_init();

   /*
    * Avatars. Every step is optional and every failure degrades to the lettered
    * placeholder rather than stopping the app: no SDL2_image, no trust store, a
    * loader thread that will not start — the timeline still reads.
    *
    * After cobalt_session_init(), which is what resolves the trust store.
    */
   ctx->images_up = cobalt_images_init(cobalt_session_ca_path());
   if (ctx->images_up) {
      ctx->tv_images = cobalt_imagecache_create(COBALT_AVATAR_TEXTURE_MAX,
                                                COBALT_IMAGE_FIT_CIRCLE);
      ctx->drc_images = cobalt_imagecache_create(COBALT_AVATAR_TEXTURE_MAX,
                                                 COBALT_IMAGE_FIT_CIRCLE);
      cobalt_render_set_images(ctx->tv, ctx->tv_images);
      cobalt_render_set_images(ctx->drc, ctx->drc_images);
   } else {
      COBALT_LOGW("avatars unavailable — cards will show initials");
   }

   cobalt_input_init(&ctx->input);

   ctx->app = cobalt_app_create();
   if (!ctx->app) {
      return false;
   }

   ctx->foreground = true;
   ctx->running = true;
   return true;
}

static void
pump_events(cobalt_context *ctx)
{
   SDL_Event event;
   while (SDL_PollEvent(&event)) {
      switch (event.type) {
         case SDL_QUIT:
            /* Sent by WIIU_PumpEvents when ProcUI reports it is exiting, e.g.
             * the user closed the app from the HOME menu overlay. */
            COBALT_LOGI("SDL_QUIT received — returning to the Wii U Menu");
            ctx->running = false;
            break;

         case SDL_APP_WILLENTERBACKGROUND:
            /* The foreground is about to be released; stop drawing. Touching
             * GX2 after this point is what leaves a console hung. */
            COBALT_LOGI("entering background");
            ctx->foreground = false;
            break;

         case SDL_APP_DIDENTERFOREGROUND:
            COBALT_LOGI("returned to foreground");
            ctx->foreground = true;
            /* Textures do not survive a foreground release, so anything cached
             * against the old GPU context has to go. */
            cobalt_render_flush_text_cache(ctx->tv);
            cobalt_render_flush_text_cache(ctx->drc);
            cobalt_imagecache_flush(ctx->tv_images);
            cobalt_imagecache_flush(ctx->drc_images);
            break;

         case SDL_APP_TERMINATING:
            COBALT_LOGI("SDL_APP_TERMINATING received");
            ctx->running = false;
            break;

         default:
            cobalt_input_handle_event(&ctx->input, &event);
            break;
      }
   }
}

int
main(int argc, char **argv)
{
   (void) argc;
   (void) argv;

   cobalt_context ctx;
   SDL_memset(&ctx, 0, sizeof(ctx));

   if (!startup(&ctx)) {
      COBALT_LOGE("startup failed — exiting");
      shutdown_all(&ctx);
      return EXIT_FAILURE;
   }

   COBALT_LOGI("cobalt running");

   while (ctx.running) {
      const uint32_t now = SDL_GetTicks();

      cobalt_input_begin_frame(&ctx.input, now);
      pump_events(&ctx);
      cobalt_input_end_frame(&ctx.input, now);

      if (!ctx.running) {
         break;
      }

      if (!ctx.foreground) {
         /* Backgrounded: keep pumping so ProcUI messages continue to flow
          * through SDL, but draw nothing and do not spin the CPU. */
         SDL_Delay(FRAME_DELAY_MS);
         continue;
      }

      cobalt_app_update(ctx.app, &ctx.input, now);

      if (cobalt_app_should_quit(ctx.app)) {
         COBALT_LOGI("app requested exit");
         break;
      }

      /*
       * Upload anything the loaders finished, before drawing rather than after:
       * an avatar that arrived during the last frame should appear on this one,
       * not the next.
       */
      cobalt_imagecache_pump(ctx.tv_images, ctx.tv);
      cobalt_imagecache_pump(ctx.drc_images, ctx.drc);

      /* TV first (no swap), GamePad second (swaps both). */
      cobalt_render_begin(ctx.tv);
      cobalt_app_draw(ctx.app, ctx.tv, COBALT_SURFACE_TV);
      cobalt_render_end(ctx.tv);

      cobalt_render_begin(ctx.drc);
      cobalt_app_draw(ctx.app, ctx.drc, COBALT_SURFACE_DRC);
      cobalt_render_end(ctx.drc);
   }

   shutdown_all(&ctx);
   return EXIT_SUCCESS;
}
