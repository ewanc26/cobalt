#include "app/messages.h"
#include "atproto/session.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_messages_view_init(cobalt_messages_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   cobalt_listnav_init(&view->nav);
}

void
cobalt_messages_view_rewind(cobalt_messages_view *view)
{
   if (view) {
      cobalt_listnav_rewind(&view->nav);
   }
}

cobalt_messages_action
cobalt_messages_view_update(cobalt_messages_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_MESSAGES_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_MESSAGES_BACK;
   }

   const cobalt_chat_inbox *inbox = cobalt_session_chat_inbox();
   const bool busy = cobalt_session_busy();

   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !busy) {
      COBALT_LOGI("messages: refresh requested");
      if (cobalt_session_begin_chat_inbox(false)) {
         cobalt_messages_view_rewind(view);
      }
      return COBALT_MESSAGES_STAY;
   }

   if (inbox->count == 0) {
      return COBALT_MESSAGES_STAY;
   }

   cobalt_listnav_move(&view->nav, in, inbox->count);
   cobalt_listnav_follow(&view->nav);

   if (!busy && cobalt_chat_inbox_can_page(inbox) &&
       view->nav.selected >= inbox->count - 1) {
      cobalt_session_begin_chat_inbox(true);
   }

   return COBALT_MESSAGES_STAY;
}

/* --- drawing --- */

static int
row_height(cobalt_render *r, const cobalt_chat_convo *item, const cobalt_metrics *m)
{
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   int h = m->pad_tile + body_h;
   if (item->preview[0]) {
      h += caption_h + m->line_gap;
   }
   h += m->pad_tile;
   return h > 0 ? h : m->font_body * 3;
}

static void
draw_row(cobalt_render *r, const cobalt_chat_convo *item, const SDL_Rect *rect,
         bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   int y = rect->y + m->pad_tile;

   /* Unread conversations get the same accent dot as unread notifications. */
   if (item->unread > 0) {
      const int d = 10;
      SDL_Rect dot = { rect->x + rect->w - d - 14, rect->y + rect->h - d - 12, d, d };
      cobalt_fill_rounded_rect(r, &dot, d / 2, COBALT_COLOUR_ACCENT);
   }

   const int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, item->name, left, y,
                                       COBALT_COLOUR_TEXT);

   /* The handle, or a plain "Group", sits after the name and is dropped when a
    * long name has taken the row. */
   const char *tag = item->group ? "Group" : item->handle;
   if (tag[0]) {
      int tag_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, tag, &tag_w, NULL);
      const int tag_x = left + name_w + m->pad_tile / 2;
      if (tag_x + tag_w < right) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, tag, tag_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
   y += body_h;

   if (item->preview[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, item->preview, left, y,
                               right - left, 1, COBALT_COLOUR_TEXT);
   }
}

void
cobalt_messages_view_draw(cobalt_messages_view *view, cobalt_render *r,
                          cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_chat_inbox *inbox = cobalt_session_chat_inbox();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Messages", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[96];
      if (cobalt_session_busy()) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else if (inbox->unread > 0) {
         snprintf(subtitle, sizeof(subtitle), "%d unread", inbox->unread);
      } else {
         snprintf(subtitle, sizeof(subtitle), "%s", cobalt_session_handle());
      }
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = cobalt_content_top(r);
   const int bottom = m->height - m->pad_edge - 28;

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (inbox->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge, m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "No messages yet.",
                       card.x + m->pad_tile, card.y + m->pad_tile, COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->nav, touchable);
      cobalt_draw_hints(r, "B: back   +: refresh");
      return;
   }

   int y = top;
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < inbox->count; i++) {
      const cobalt_chat_convo *item = &inbox->items[i];
      const int h = row_height(r, item, m);

      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_row(r, item, &rect, i == view->nav.selected, m);
      cobalt_listnav_draw_add(&view->nav, touchable, &rect, i);

      last_fitted = i;
      y += h + m->gap / 2;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   cobalt_draw_hints(r, cobalt_session_busy() ? "Working..." : "B: back   +: refresh");
}
