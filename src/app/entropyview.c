#include "app/entropyview.h"

#include "ui/theme.h"
#include "util/entropy.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_entropy_view_init(cobalt_entropy_view *v, const char *seed_path, const void *extra,
                         size_t extra_len)
{
   memset(v, 0, sizeof(*v));
   snprintf(v->path, sizeof(v->path), "%s", seed_path ? seed_path : "");
   cobalt_gather_init(&v->gather, extra, extra_len);
}

cobalt_entropy_view_action
cobalt_entropy_view_update(cobalt_entropy_view *v, const cobalt_input *in, uint32_t tick)
{
   if (v->state == COBALT_ENTROPY_VIEW_SAVED) {
      return cobalt_input_pressed(in, COBALT_BTN_CONFIRM) ? COBALT_ENTROPY_VIEW_QUIT
                                                          : COBALT_ENTROPY_VIEW_STAY;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_ENTROPY_VIEW_SKIP;
   }
   if (v->state == COBALT_ENTROPY_VIEW_FAILED) {
      if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
         /* Start again with a fresh collector. */
         cobalt_entropy_view_init(v, v->path, NULL, 0);
      }
      return COBALT_ENTROPY_VIEW_STAY;
   }

   if (in->touch_down) {
      cobalt_gather_add(&v->gather, in->touch_x, in->touch_y, tick);
   }
   if (cobalt_gather_done(&v->gather)) {
      unsigned char seed[COBALT_ENTROPY_SEED_SIZE];
      if (cobalt_gather_finish(&v->gather, seed) && v->path[0] &&
          cobalt_entropy_seed_save(v->path, seed)) {
         /* No byte of the seed is logged; only the fact. */
         COBALT_LOGI("entropy: a seed was made from touch input and saved");
         v->state = COBALT_ENTROPY_VIEW_SAVED;
      } else {
         COBALT_LOGE("entropy: could not save the seed made from touch input");
         v->state = COBALT_ENTROPY_VIEW_FAILED;
      }
      memset(seed, 0, sizeof(seed));
   }
   return COBALT_ENTROPY_VIEW_STAY;
}

void
cobalt_entropy_view_draw(cobalt_entropy_view *v, cobalt_render *r, cobalt_surface_id surface,
                         int top)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   SDL_Rect panel = {m->pad_edge, top, m->width - 2 * m->pad_edge,
                     m->height - top - m->pad_edge - 40};
   const int x = panel.x + m->pad_tile;
   const int w = panel.w - 2 * m->pad_tile;
   int y = panel.y + m->pad_tile;
   const int lh = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->line_gap;

   cobalt_draw_tile(r, &panel, 0.0f);

   if (v->state == COBALT_ENTROPY_VIEW_SAVED) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "Done. Your seed is saved. Quit Cobalt and start it again from the "
                               "Wii U Menu; sign-in works after that.",
                               x, y, w, 5, COBALT_COLOUR_TEXT);
      cobalt_draw_hints(r, "A: quit");
      return;
   }
   if (v->state == COBALT_ENTROPY_VIEW_FAILED) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "Could not save the seed to the SD card. Check it is in and not "
                               "write-protected, then try again.",
                               x, y, w, 4, COBALT_COLOUR_ERROR);
      cobalt_draw_hints(r, "A: try again  B: skip");
      return;
   }

   if (surface == COBALT_SURFACE_TV) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "First run. Pick up the GamePad and scribble on its screen.", x, y,
                               w, 3, COBALT_COLOUR_TEXT);
   } else {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "First run: Cobalt has no random seed of its own on this SD card, "
                               "and without one it will not go online. Scribble across this "
                               "screen, all over it, until the bar fills.",
                               x, y, w, 5, COBALT_COLOUR_TEXT);
   }
   y += lh * 5;
   const int pct = cobalt_gather_percent(&v->gather);
   SDL_Rect bar = {x, y, w, 18};
   cobalt_draw_tile(r, &bar, 0.0f);
   SDL_Rect fill = {x, y, w * pct / 100, 18};
   if (fill.w > 0) {
      cobalt_draw_tile(r, &fill, 1.0f);
   }
   cobalt_draw_hints(r, "Draw on the GamePad  B: skip (no network)");
}
