#include "app/queue_view.h"

#include <stdio.h>

void
cobalt_queue_view_open(cobalt_queue_view *view)
{
   view->selected = 0;
}

cobalt_queue_action
cobalt_queue_view_update(cobalt_queue_view *view, const cobalt_post_queue *queue,
                         const cobalt_input *in, unsigned *index)
{
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_QUEUE_BACK;
   }
   if (queue->count == 0) {
      return COBALT_QUEUE_STAY;
   }

   if (view->selected >= queue->count) {
      view->selected = queue->count - 1;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && view->selected > 0) {
      view->selected--;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected + 1 < queue->count) {
      view->selected++;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      *index = view->selected;
      return COBALT_QUEUE_SEND;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
      *index = view->selected;
      return COBALT_QUEUE_DISCARD;
   }
   return COBALT_QUEUE_STAY;
}

void
cobalt_queue_view_draw(const cobalt_queue_view *view, const cobalt_post_queue *queue,
                       cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const int bottom = m->height - m->pad_edge - 28;
   const int width = m->width - 2 * m->pad_edge;

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Queued posts", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   if (queue->count == 0) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "Nothing is queued. On a new post, choose Save for "
                               "later to keep it here until you send it.",
                               m->pad_edge, top + m->pad_tile, width, 3,
                               COBALT_COLOUR_TEXT);
      cobalt_draw_hints(r, "B: back");
      return;
   }

   int y = top;
   for (unsigned i = 0; i < queue->count; i++) {
      const cobalt_queued_post *p = cobalt_post_queue_get(queue, i);
      const int text_h = 2 * cobalt_font_line_height(r, COBALT_FONT_BODY);
      const int h = text_h + 2 * m->pad_tile;

      if (y + h > bottom) {
         break;
      }
      SDL_Rect rect = { m->pad_edge, y, width, h };
      const bool focus = (i == view->selected);
      cobalt_draw_tile(r, &rect, focus ? 1.0f : 0.0f);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, p->text,
                               rect.x + m->pad_tile, rect.y + m->pad_tile,
                               width - 2 * m->pad_tile, 2, COBALT_COLOUR_TEXT);
      y += h + m->gap;
   }

   cobalt_draw_hints(r, "A: send   X: discard   Up/Down: choose   B: back");
}
