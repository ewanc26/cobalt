#include "app/notify.h"
#include "atproto/session.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

/* Lines of quoted text on a row that has any. One, deliberately: this is a
 * list of things that happened, not a feed — reading the reply in full is what
 * opening it is for. */
#define TEXT_LINES 1

void
cobalt_notify_view_init(cobalt_notify_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_notify_view_rewind(cobalt_notify_view *view)
{
   if (!view) {
      return;
   }
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;
}

cobalt_notify_action
cobalt_notify_view_update(cobalt_notify_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_NOTIFY_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_NOTIFY_BACK;
   }

   const cobalt_notifications *list = cobalt_session_notifications();
   const bool busy = cobalt_session_busy();

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !busy) {
      COBALT_LOGI("notify: refresh requested");
      if (cobalt_session_begin_notifications(false)) {
         cobalt_notify_view_rewind(view);
      }
      return COBALT_NOTIFY_STAY;
   }

   if (list->count == 0) {
      return COBALT_NOTIFY_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected < list->count - 1) {
      view->selected++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && view->selected > 0) {
      view->selected--;
   }

   if (view->hit_valid && in->touch_ended) {
      for (int i = 0; i < view->hit_count; i++) {
         if (cobalt_input_tapped(in, &view->hit[i])) {
            view->selected = view->hit_index[i];
            break;
         }
      }
   }

   if (view->selected < view->scroll) {
      view->scroll = view->selected;
   } else if (view->last_visible >= 0 && view->selected > view->last_visible) {
      view->scroll += view->selected - view->last_visible;
   }
   if (view->scroll > view->selected) {
      view->scroll = view->selected;
   }
   if (view->scroll < 0) {
      view->scroll = 0;
   }

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      const cobalt_notification *item = &list->items[view->selected];
      /* A follow has nothing to open. Doing nothing is better than opening
       * something arbitrary, and the row already says what happened. */
      if (item->subject_uri[0]) {
         COBALT_LOGI("notify: opening %s", item->subject_uri);
         cobalt_session_begin_thread(item->subject_uri);
         return COBALT_NOTIFY_OPEN_THREAD;
      }
   }

   if (!busy && list->has_more && view->selected >= list->count - 1) {
      cobalt_session_begin_notifications(true);
   }

   return COBALT_NOTIFY_STAY;
}

/* --- drawing --- */

static int
row_height(cobalt_render *r, const cobalt_notification *item,
           const cobalt_metrics *m)
{
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   int h = m->pad_tile + body_h;
   if (item->text[0]) {
      h += TEXT_LINES * (caption_h + m->line_gap);
   }
   h += m->pad_tile;
   return h > 0 ? h : m->font_body * 3;
}

static void
draw_row(cobalt_render *r, const cobalt_notification *item, const SDL_Rect *rect,
         bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   int y = rect->y + m->pad_tile;

   /* Unread rows get an accent bar rather than a different background: the
    * tile already carries the focus state, and two competing highlights on one
    * row is harder to read than one. */
   if (item->unread) {
      SDL_Rect bar = { rect->x, rect->y, 4, rect->h };
      cobalt_fill_rect(r, &bar, COBALT_COLOUR_ACCENT);
   }

   const int actor_w = cobalt_draw_text(r, COBALT_FONT_BODY, item->actor, left, y,
                                        COBALT_COLOUR_TEXT);

   int age_w = 0;
   if (item->age[0]) {
      cobalt_text_size(r, COBALT_FONT_CAPTION, item->age, &age_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, item->age, right - age_w,
                       y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      age_w += m->pad_tile;
   }

   /* "liked your post" sits right after the name and is dropped rather than
    * overlapped when a long display name has taken the row. */
   const int summary_x = left + actor_w + m->pad_tile / 2;
   const int summary_room = (right - age_w) - summary_x;
   if (summary_room > 0) {
      int summary_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, item->summary, &summary_w, NULL);
      if (summary_w <= summary_room) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, item->summary, summary_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
   y += body_h;

   if (item->text[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, item->text, left, y,
                               right - left, TEXT_LINES, COBALT_COLOUR_TEXT);
   }
}

void
cobalt_notify_view_draw(cobalt_notify_view *view, cobalt_render *r,
                        cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_notifications *list = cobalt_session_notifications();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Notifications", m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[96];
      if (cobalt_session_busy()) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else if (list->unread > 0) {
         snprintf(subtitle, sizeof(subtitle), "%d new", list->unread);
      } else {
         snprintf(subtitle, sizeof(subtitle), "%s", cobalt_session_handle());
      }
      SDL_Color dim = { 0xD8, 0xE6, 0xF4, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge +
                          cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int bottom = m->height - m->pad_edge - 28;

   if (touchable) {
      view->hit_count = 0;
   }

   if (list->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "Nothing new.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      if (touchable) {
         view->last_visible = -1;
      }
      return;
   }

   int y = top;
   int last_fitted = view->scroll;

   for (int i = view->scroll; i < list->count; i++) {
      const cobalt_notification *item = &list->items[i];
      const int h = row_height(r, item, m);

      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_row(r, item, &rect, i == view->selected, m);

      if (touchable && view->hit_count < COBALT_NOTIFICATIONS_MAX) {
         view->hit[view->hit_count] = rect;
         view->hit_index[view->hit_count] = i;
         view->hit_count++;
      }

      last_fitted = i;
      y += h + m->gap / 2;
   }

   if (touchable) {
      view->hit_valid = true;
      view->last_visible = last_fitted;
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy() ? "Working..."
                                          : "A: open   +: refresh   B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}
