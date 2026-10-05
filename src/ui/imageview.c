#include "ui/imageview.h"

#include "ui/imagecache.h"
#include "ui/postcard.h"
#include "ui/theme.h"

#include <stdio.h>

void
cobalt_imageview_open(cobalt_imageview *view, const cobalt_post *post)
{
   if (!view || !post || post->image_count <= 0) {
      return;
   }

   view->image_count = post->image_count;
   if (view->image_count > COBALT_POST_IMAGES_MAX) {
      view->image_count = COBALT_POST_IMAGES_MAX;
   }
   for (int i = 0; i < view->image_count; i++) {
      view->images[i] = post->images[i];
   }
   view->index = 0;
   view->open = true;
   view->hit_valid = false;
}

void
cobalt_imageview_close(cobalt_imageview *view)
{
   if (!view) {
      return;
   }
   view->open = false;
   view->image_count = 0;
   view->hit_valid = false;
}

bool
cobalt_imageview_update(cobalt_imageview *view, const cobalt_input *in)
{
   if (!view || !in || !view->open) {
      return false;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK) ||
       cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      cobalt_imageview_close(view);
      return true;
   }

   if (view->image_count > 1) {
      if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
         view->index = (view->index + 1) % view->image_count;
      } else if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
         view->index = (view->index + view->image_count - 1) % view->image_count;
      }
   }

   if (view->hit_valid && in->touch_ended &&
       cobalt_input_tapped(in, &view->hit_close)) {
      cobalt_imageview_close(view);
   }

   return true;
}

void
cobalt_imageview_draw(cobalt_imageview *view, cobalt_render *r)
{
   if (!view || !r || !view->open || view->image_count <= 0) {
      return;
   }
   if (view->index < 0 || view->index >= view->image_count) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_post_image *img = &view->images[view->index];

   /* The whole surface, edge to edge: this screen exists to show the picture
    * as large as the output allows, and a frame around it would only shrink
    * it. The count and alt text are drawn over the image's own letterboxing
    * where they do not cover the picture. */
   const SDL_Rect full = { 0, 0, m->width, m->height };
   cobalt_fill_rect(r, &full, COBALT_COLOUR_BG_TOP);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int hint_h = 22 + m->pad_edge;
   const int alt_lines = 3;

   /* Alt text sits under the image; reserve room for it before fitting so
    * the two never overlap. A post without alt text gets the whole area. */
   const char *alt = img->alt[0] ? img->alt : NULL;
   const int alt_h = alt ? alt_lines * caption_h + m->pad_edge : 0;

   const SDL_Rect box = { 0, 0, m->width, m->height - alt_h - hint_h };
   cobalt_fill_rect(r, &box, COBALT_COLOUR_BG_TOP);

   cobalt_imagecache *viewer = cobalt_render_viewer(r);
   SDL_Texture *texture = NULL;
   int tex_w = 0, tex_h = 0;
   if (viewer && img->thumb[0]) {
      texture = cobalt_imagecache_get(viewer, r, img->thumb, &tex_w, &tex_h);
   }

   int dst_w = box.w, dst_h = box.h;
   if (texture) {
      cobalt_postcard_contain_fit(box.w, box.h, tex_w, tex_h, &dst_w, &dst_h);
   } else {
      /* Nothing decoded yet. Draw the declared ratio as a frame so the
       * screen reads as "loading" rather than as a blank the app drew for
       * no reason — the same reasoning as the thumbnail placeholder. */
      cobalt_postcard_contain_fit(box.w, box.h, img->aspect_w, img->aspect_h,
                                  &dst_w, &dst_h);
      const SDL_Rect frame = { (m->width - dst_w) / 2, (box.h - dst_h) / 2,
                              dst_w, dst_h };
      cobalt_fill_rounded_rect(r, &frame, 6, COBALT_COLOUR_TILE_EDGE);
   }

   if (texture) {
      const SDL_Rect dst = { (m->width - dst_w) / 2, (box.h - dst_h) / 2,
                            dst_w, dst_h };
      cobalt_draw_texture(r, texture, &dst);
   }

   if (alt) {
      const int alt_y = box.h + m->pad_edge / 2;
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, alt, m->pad_edge, alt_y,
                               m->width - 2 * m->pad_edge, alt_lines,
                               COBALT_COLOUR_TEXT_DIM);
   }

   /* The count, top-right, only when there is more than one to count. */
   if (view->image_count > 1) {
      char count[32];
      snprintf(count, sizeof(count), "%d of %d", view->index + 1,
               view->image_count);
      int w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, count, &w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, count,
                       m->width - m->pad_edge - w, m->pad_edge,
                       COBALT_COLOUR_TEXT_DIM);
   }

   const char *hints = view->image_count > 1
                          ? "B: close  Left/Right: pictures"
                          : "B: close";
   cobalt_draw_hints(r, hints);

   view->hit_close = full;
   view->hit_valid = true;
}
