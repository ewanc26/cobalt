#include "app/chat_thread.h"
#include "atproto/session.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

/* A bubble never takes more lines than this, so one long message cannot push
 * the rest of the conversation off the screen. The text is cut with an
 * ellipsis by the wrapper, as the post cards do. */
#define BUBBLE_LINES_MAX 6

void
cobalt_chat_thread_view_init(cobalt_chat_thread_view *view)
{
   if (view) {
      memset(view, 0, sizeof(*view));
   }
}

void
cobalt_chat_thread_view_open(cobalt_chat_thread_view *view, const char *title,
                             const char *convo_id)
{
   if (!view) {
      return;
   }
   snprintf(view->title, sizeof(view->title), "%s", title ? title : "");
   snprintf(view->convo_id, sizeof(view->convo_id), "%s", convo_id ? convo_id : "");
   view->offset = 0;
}

cobalt_chat_thread_action
cobalt_chat_thread_view_update(cobalt_chat_thread_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_CHAT_THREAD_BACK;
   }

   const cobalt_chat_thread *thread = cobalt_session_chat_thread();
   const bool busy = cobalt_session_busy();

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !busy) {
      COBALT_LOGI("chat thread: refresh requested");
      cobalt_session_begin_chat_messages(view->convo_id);
      view->offset = 0;
      return COBALT_CHAT_THREAD_STAY;
   }

   /* Up goes back in time, one message at a time, until the oldest held. */
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && thread->count > 0 &&
       view->offset < thread->count - 1) {
      view->offset++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && view->offset > 0) {
      view->offset--;
   }

   return COBALT_CHAT_THREAD_STAY;
}

/* --- drawing --- */

static int
bubble_lines(cobalt_render *r, const cobalt_chat_message *msg, int inner_w)
{
   int lines = cobalt_text_wrapped_lines(r, COBALT_FONT_BODY, msg->text, inner_w);
   if (lines < 1) {
      lines = 1;
   }
   return lines > BUBBLE_LINES_MAX ? BUBBLE_LINES_MAX : lines;
}

void
cobalt_chat_thread_view_draw(cobalt_chat_thread_view *view, cobalt_render *r,
                             cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }
   (void) surface;

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_chat_thread *thread = cobalt_session_chat_thread();
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Messages", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[128];
      snprintf(subtitle, sizeof(subtitle), "%s",
               cobalt_session_busy() ? "Loading..." : view->title);
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = cobalt_content_top(r);
   const int bottom = m->height - m->pad_edge - 28;

   if (thread->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "No messages to show.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      cobalt_draw_hints(r, "B: back   +: refresh");
      return;
   }

   /* Draw from the newest held message upwards, skipping `offset` of them, until
    * the next bubble would cross the top. */
   const int newest = thread->count - 1 - view->offset;
   const int content_w = m->width - 2 * m->pad_edge;
   const int bubble_w = content_w * 3 / 4;
   const int inner_w = bubble_w - 2 * m->pad_tile;
   int y = bottom;

   for (int i = newest; i >= 0; i--) {
      const cobalt_chat_message *msg = &thread->items[i];
      const int lines = bubble_lines(r, msg, inner_w);
      const int h = m->pad_tile + caption_h + lines * body_h + m->pad_tile;

      if (y - h < top) {
         break;
      }
      y -= h;

      SDL_Rect rect = { msg->mine ? m->width - m->pad_edge - bubble_w : m->pad_edge,
                        y, bubble_w, h };
      /* Alignment says who sent it; the focus highlight is left for selection. */
      cobalt_draw_tile(r, &rect, 0.0f);

      const int text_x = rect.x + m->pad_tile;
      if (msg->age[0]) {
         int age_w = 0;
         cobalt_text_size(r, COBALT_FONT_CAPTION, msg->age, &age_w, NULL);
         cobalt_draw_text(r, COBALT_FONT_CAPTION, msg->age,
                          rect.x + rect.w - m->pad_tile - age_w, rect.y + m->pad_tile,
                          COBALT_COLOUR_TEXT_DIM);
      }
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, msg->text, text_x,
                               rect.y + m->pad_tile + caption_h, inner_w, lines,
                               COBALT_COLOUR_TEXT);

      y -= m->gap;
   }

   cobalt_draw_hints(r, cobalt_session_busy() ? "Working..."
                                          : "B: back   Up/Down: older   +: refresh");
}
