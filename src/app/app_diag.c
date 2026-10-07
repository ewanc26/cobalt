/* The diagnostics screen. */

#include "app/app_internal.h"

void
cobalt_app_draw_diagnostics(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_app_draw_header(r, "Diagnostics");

   /*
    * This screen exists because there is no emulator (AGENTS.md §10). It is
    * the fastest way to confirm, on the console itself, that asset paths
    * resolved, the network came up, the TLS trust store is present and the
    * session layer can run — the things that block everything downstream.
    */
   const cobalt_net_status net = cobalt_net_get_status();
   const char *ca = cobalt_session_ca_path();
   const char *blocker = cobalt_session_blocker();

   char lines[20][160];
   int count = 0;

   snprintf(lines[count++], sizeof(lines[0]), "Cobalt: %s",
            cobalt_build_describe());
   snprintf(lines[count++], sizeof(lines[0]), "Surface: %s (%dx%d) frame %u",
            surface == COBALT_SURFACE_DRC ? "GamePad" : "TV", m->width, m->height,
            (unsigned) app->frames);
   snprintf(lines[count++], sizeof(lines[0]), "Content: %s",
            cobalt_content_root() ? cobalt_content_root() : "NOT FOUND");
   snprintf(lines[count++], sizeof(lines[0]), "Data: %s",
            cobalt_data_root() ? cobalt_data_root() : "NOT FOUND");
   snprintf(lines[count++], sizeof(lines[0]), "Network: %s (%s)",
            cobalt_net_status_string(net), cobalt_net_local_address());
   snprintf(lines[count++], sizeof(lines[0]), "Trust store: %s",
            ca ? ca : "MISSING - run `make cacert`");
   snprintf(lines[count++], sizeof(lines[0]), "%s / %s",
            app->sdl_version, app->curl_version);
   snprintf(lines[count++], sizeof(lines[0]), "ATProto SDK: %s",
            cobalt_atproto_sdk_version());
   snprintf(lines[count++], sizeof(lines[0]), "SDK status: %s",
            cobalt_atproto_status_string());
   snprintf(lines[count++], sizeof(lines[0]), "Sign-in: %s",
            blocker ? blocker : "available");
   snprintf(lines[count++], sizeof(lines[0]), "Net worker: %s",
            cobalt_session_threaded() ? "background thread"
                                      : "SYNCHRONOUS - requests stall the frame");

   /*
    * Avatars fail quietly by design — a card falls back to its initial — so
    * without a counter here there is no way to tell "nobody has set one" from
    * "every fetch is failing", which are very different problems.
    */
   cobalt_imagecache *images = cobalt_render_images(r);
   if (!images) {
      snprintf(lines[count++], sizeof(lines[0]), "Avatars: off (%s)",
               cobalt_imagecache_supported() ? "cache unavailable"
                                             : "no SDL2_image");
   } else {
      int ready = 0, loading = 0, failed = 0;
      cobalt_imagecache_stats(images, &ready, &loading, &failed);
      snprintf(lines[count++], sizeof(lines[0]),
               "Avatars: %d ready, %d loading, %d failed", ready, loading,
               failed);
   }

   /* Same reasoning, same cache implementation, different instance — see
    * ui/render.h's cobalt_render_set_thumbs(). Reported separately because
    * the two caches can legitimately disagree: post images are far more
    * likely to be large or slow than an avatar. */
   cobalt_imagecache *thumbs = cobalt_render_thumbs(r);
   if (!thumbs) {
      snprintf(lines[count++], sizeof(lines[0]), "Thumbnails: off (%s)",
               cobalt_imagecache_supported() ? "cache unavailable"
                                             : "no SDL2_image");
   } else {
      int ready = 0, loading = 0, failed = 0;
      cobalt_imagecache_stats(thumbs, &ready, &loading, &failed);
      snprintf(lines[count++], sizeof(lines[0]),
               "Thumbnails: %d ready, %d loading, %d failed", ready, loading,
               failed);
   }

   switch (cobalt_session_state()) {
      case COBALT_AUTH_SIGNED_IN:
         snprintf(lines[count++], sizeof(lines[0]), "Session: %s at %s",
                  cobalt_session_handle(), cobalt_session_service());
         break;
      case COBALT_AUTH_WORKING:
         snprintf(lines[count++], sizeof(lines[0]), "Session: request in flight");
         break;
      case COBALT_AUTH_SIGNED_OUT:
      default:
         snprintf(lines[count++], sizeof(lines[0]), "Session: signed out%s",
                  cobalt_session_has_saved() ? " (credentials stored)" : "");
         break;
   }

   snprintf(lines[count++], sizeof(lines[0]), "Font: %s",
            cobalt_render_has_font(r) ? "loaded" : "MISSING");

   /* The GamePad panel is 480px tall and this list is long, so it drops to the
    * caption scale there rather than scrolling. The TV keeps body size. */
   const cobalt_font_id font = (surface == COBALT_SURFACE_DRC) ? COBALT_FONT_CAPTION
                                                               : COBALT_FONT_BODY;

   const int top = cobalt_content_top(r);
   SDL_Rect panel = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                      m->height - top - m->pad_edge - 40 };
   cobalt_draw_tile(r, &panel, 0.0f);

   /* Squeeze the pitch so every line fits the panel instead of spilling. */
   int line_h = cobalt_font_line_height(r, font) + m->line_gap;
   const int fit = (panel.h - 2 * m->pad_tile) / (count > 0 ? count : 1);
   if (fit < line_h) {
      line_h = fit;
   }
   for (int i = 0; i < count; i++) {
      bool bad = (strstr(lines[i], "NOT FOUND") != NULL) ||
                 (strstr(lines[i], "MISSING") != NULL) ||
                 (strstr(lines[i], "SYNCHRONOUS") != NULL) ||
                 (blocker != NULL && strncmp(lines[i], "Sign-in:", 8) == 0) ||
                 (net != COBALT_NET_UP && strncmp(lines[i], "Network:", 8) == 0);

      cobalt_draw_text_wrapped(r, font, lines[i],
                               panel.x + m->pad_tile, panel.y + m->pad_tile + i * line_h,
                               panel.w - 2 * m->pad_tile, 1,
                               bad ? COBALT_COLOUR_ERROR : COBALT_COLOUR_TEXT);
   }

}
