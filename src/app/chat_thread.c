#include "app/chat_thread.h"
#include "atproto/session.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

/* A bubble never takes more lines than this, so one long message cannot push
 * the rest of the conversation off the screen. The wrapper cuts the text with an
 * ellipsis, as the post cards do. */
#define BUBBLE_LINES_MAX 6

void
cobalt_chat_thread_view_init(cobalt_chat_thread_view *view)
{
   if (view) {
      memset(view, 0, sizeof(*view));
      cobalt_listnav_init(&view->join_nav);
   }
}

void
cobalt_chat_thread_view_open(cobalt_chat_thread_view *view, const char *title,
                             const char *convo_id, bool group)
{
   if (!view) {
      return;
   }
   const bool same = strcmp(view->convo_id, convo_id ? convo_id : "") == 0;

   snprintf(view->title, sizeof(view->title), "%s", title ? title : "");
   snprintf(view->convo_id, sizeof(view->convo_id), "%s", convo_id ? convo_id : "");
   view->group = group;
   view->offset = 0;
   view->replying = false;
   /* Opening a conversation shows the messages, never whatever sub-mode it was
    * left on. */
   view->mode = COBALT_CHAT_THREAD_MODE_MESSAGES;
   cobalt_listnav_rewind(&view->join_nav);
   if (!same) {
      view->reply[0] = '\0';
   }
}

void
cobalt_chat_thread_view_sent(cobalt_chat_thread_view *view)
{
   if (view) {
      view->reply[0] = '\0';
   }
}

/* --- group join requests --- */

void
cobalt_chat_thread_decision_began(cobalt_chat_thread_view *view, bool began)
{
   if (view && began) {
      view->join_decision.pending = true;
   }
}

cobalt_chat_thread_decision_outcome
cobalt_chat_thread_decision_finished(cobalt_chat_thread_view *view, bool ok)
{
   if (!view || !view->join_decision.pending) {
      return COBALT_CHAT_THREAD_DECISION_NONE;
   }
   view->join_decision.pending = false;
   if (!ok) {
      return COBALT_CHAT_THREAD_DECISION_FAILED;
   }
   return view->join_decision.approve ? COBALT_CHAT_THREAD_DECISION_APPROVED
                                      : COBALT_CHAT_THREAD_DECISION_REJECTED;
}

/* Fill the decision from a row and report which action it is. The row is copied,
 * because the worker may remove it while the job runs. */
static cobalt_chat_thread_action
press_join_decision(cobalt_chat_thread_view *view,
                    const cobalt_chat_join_requests *requests, int index, bool approve)
{
   const cobalt_chat_join_request *row = &requests->items[index];
   snprintf(view->join_decision.did, sizeof(view->join_decision.did), "%s", row->did);
   snprintf(view->join_decision.name, sizeof(view->join_decision.name), "%s", row->name);
   view->join_decision.approve = approve;
   return approve ? COBALT_CHAT_THREAD_JOIN_APPROVE : COBALT_CHAT_THREAD_JOIN_REJECT;
}

/* --- input --- */

static cobalt_chat_thread_action
update_join_requests(cobalt_chat_thread_view *view, const cobalt_input *in)
{
   const cobalt_chat_join_requests *requests = cobalt_session_chat_join_requests();
   const bool working = cobalt_session_busy() || view->join_decision.pending;

   /* B returns to the conversation, not out of it. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->mode = COBALT_CHAT_THREAD_MODE_MESSAGES;
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !working) {
      COBALT_LOGI("chat thread: join requests refresh requested");
      if (cobalt_session_begin_chat_join_requests(view->convo_id, false)) {
         cobalt_listnav_rewind(&view->join_nav);
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   if (requests->count == 0) {
      return COBALT_CHAT_THREAD_STAY;
   }

   cobalt_listnav_move(&view->join_nav, in, requests->count);
   cobalt_listnav_follow(&view->join_nav);

   const int sel = (int) view->join_nav.selected;
   if (!working && sel < requests->count) {
      if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
         return press_join_decision(view, requests, sel, true);
      }
      if (cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
         return press_join_decision(view, requests, sel, false);
      }
   }

   if (!working && cobalt_chat_join_requests_can_page(requests) &&
       sel >= requests->count - 1) {
      cobalt_session_begin_chat_join_requests(view->convo_id, true);
   }

   return COBALT_CHAT_THREAD_STAY;
}

cobalt_chat_thread_action
cobalt_chat_thread_view_update(cobalt_chat_thread_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_CHAT_THREAD_STAY;
   }

   /* While the keyboard is up it owns the input; B closes it, as it does on the
    * other screens that type. */
   if (view->replying) {
      switch (cobalt_keyboard_update(&view->kb, in)) {
         case COBALT_KB_ACCEPTED:
         case COBALT_KB_CANCELLED:
            view->replying = false;
            break;
         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   if (view->mode == COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS) {
      return update_join_requests(view, in);
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_CHAT_THREAD_BACK;
   }

   const cobalt_chat_thread *thread = cobalt_session_chat_thread();
   const bool busy = cobalt_session_busy();

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM) && !busy) {
      view->replying = true;
      cobalt_keyboard_open(&view->kb, view->reply, sizeof(view->reply), false);
      return COBALT_CHAT_THREAD_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y) && !busy) {
      if (cobalt_chat_reply_valid(view->reply)) {
         COBALT_LOGI("chat thread: sending a reply");
         /* A refused send keeps the draft; the person can press Y again. */
         cobalt_session_begin_chat_send(view->convo_id, view->reply);
      }
      return COBALT_CHAT_THREAD_STAY;
   }

   /* X opens a group's join requests. A one-to-one conversation has none to
    * show, so its X stays inert rather than offering a list that can only be
    * refused. */
   if (cobalt_input_pressed(in, COBALT_BTN_ALT_X) && !busy && view->group) {
      COBALT_LOGI("chat thread: opening join requests");
      view->mode = COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS;
      cobalt_listnav_rewind(&view->join_nav);
      cobalt_session_begin_chat_join_requests(view->convo_id, false);
      return COBALT_CHAT_THREAD_STAY;
   }

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

static void
draw_bubbles(cobalt_chat_thread_view *view, cobalt_render *r, const cobalt_metrics *m,
              cobalt_surface_id surface)
{
   const cobalt_chat_thread *thread = cobalt_session_chat_thread();
   const int top = cobalt_content_top(r);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   /* The reply line and keyboard take the bottom while the person is writing;
    * otherwise the footer hints do. */
   const int kb_h = cobalt_keyboard_height(surface);
   const int draft_y = m->height - kb_h - body_h - m->gap;
   /* Otherwise the bubbles stop a gap above the footer's top edge, which is
    * drawn at height - pad_edge/2 - one pill tall (render.c). */
   const int footer_top = m->height - m->pad_edge / 2 - cobalt_pill_height(r);
   const int bottom = view->replying ? draft_y - m->gap : footer_top - m->gap;

   if (thread->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "No messages to show.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      return;
   }

   /* Draw from the newest held message upwards, skipping `offset` of them,
    * until the next bubble would cross the top. */
   const int newest = thread->count - 1 - view->offset;
   const int content_w = m->width - 2 * m->pad_edge;
   const int bubble_w = content_w * 3 / 4;
   const int inner_w = bubble_w - 2 * m->pad_tile;
   int y = bottom;

   for (int i = newest; i >= 0; i--) {
      const cobalt_chat_message *msg = &thread->items[i];
      int lines = bubble_lines(r, msg, inner_w);
      int h = m->pad_tile + caption_h + lines * body_h + m->pad_tile;

      if (y - h < top) {
         /* Only the newest message is worth cutting to fit: with the keyboard up
          * there may be room for one line, and that line should still show. */
         if (i != newest) {
            break;
         }
         const int fit = (y - top - 2 * m->pad_tile - caption_h) / body_h;
         if (fit < 1) {
            break;
         }
         lines = fit;
         h = m->pad_tile + caption_h + lines * body_h + m->pad_tile;
      }
      y -= h;

      /* Alignment says who sent it; the focus highlight is left for selection. */
      SDL_Rect rect = { msg->mine ? m->width - m->pad_edge - bubble_w : m->pad_edge,
                        y, bubble_w, h };
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
}

/* One join request: the requester, and how long ago they asked. No avatar or
 * last message, because a request is not a conversation. */
static void
draw_join_row(cobalt_render *r, const cobalt_chat_join_request *item, const SDL_Rect *rect,
              bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   const int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, item->name, left, y,
                                       COBALT_COLOUR_TEXT);

   if (item->handle[0]) {
      int tag_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, item->handle, &tag_w, NULL);
      const int tag_x = left + name_w + m->pad_tile / 2;
      if (tag_x + tag_w < right) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, item->handle, tag_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }

   if (item->age[0]) {
      int age_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, item->age, &age_w, NULL);
      const int age_x = right - age_w;
      if (age_x > left) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, item->age, age_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
}

static void
draw_join_requests(cobalt_chat_thread_view *view, cobalt_render *r,
                   cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_chat_join_requests *requests = cobalt_session_chat_join_requests();
   const bool busy = cobalt_session_busy();

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Join requests", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[96];
      if (busy) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else {
         cobalt_chat_requests_count_label(requests->count, subtitle, sizeof(subtitle));
      }
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = cobalt_content_top(r);
   const int bottom = m->height - m->pad_edge - 28;
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_listnav_draw_begin(&view->join_nav, touchable);

   if (requests->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY, busy ? "Loading..." : "No join requests.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->join_nav, touchable);
      cobalt_draw_hints(r, busy ? "Working..." : "B: back   +: refresh");
      return;
   }

   const int row_h = m->pad_tile + cobalt_font_line_height(r, COBALT_FONT_BODY) +
                     m->pad_tile;
   int y = top;
   int last_fitted = view->join_nav.scroll;

   for (int i = view->join_nav.scroll; i < requests->count; i++) {
      const cobalt_chat_join_request *item = &requests->items[i];

      if (y + row_h > bottom && i > view->join_nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, row_h };
      draw_join_row(r, item, &rect, i == view->join_nav.selected, m);
      cobalt_listnav_draw_add(&view->join_nav, touchable, &rect, i);

      last_fitted = i;
      y += row_h + m->gap / 2;
   }

   cobalt_listnav_draw_end(&view->join_nav, touchable, last_fitted);

   cobalt_draw_hints(r, busy ? "Working..."
                             : "A: approve   X: reject   B: back   +: refresh");
}

void
cobalt_chat_thread_view_draw(cobalt_chat_thread_view *view, cobalt_render *r,
                             cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);

   if (view->mode == COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS) {
      draw_join_requests(view, r, surface);
      return;
   }

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

   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int kb_h = cobalt_keyboard_height(surface);

   draw_bubbles(view, r, m, surface);

   if (view->replying) {
      char shown[COBALT_CHAT_REPLY_MAX];
      const int draft_y = m->height - kb_h - body_h - m->gap;
      cobalt_keyboard_display_text(&view->kb, shown, sizeof(shown));
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, shown, m->pad_edge, draft_y,
                               m->width - 2 * m->pad_edge, 1, COBALT_COLOUR_TEXT);
      const SDL_Rect keys = { 0, m->height - kb_h, m->width, kb_h };
      cobalt_keyboard_draw(&view->kb, r, surface, &keys);
      return;
   }

   if (cobalt_session_busy()) {
      cobalt_draw_hints(r, "Working...");
   } else if (view->reply[0]) {
      cobalt_draw_hints(r, "A: edit reply   Y: send   B: back");
   } else if (view->group) {
      cobalt_draw_hints(r, "A: reply   X: join requests   B: back   Up/Down: older   "
                           "+: refresh");
   } else {
      cobalt_draw_hints(r, "A: reply   B: back   Up/Down: older   +: refresh");
   }
}
