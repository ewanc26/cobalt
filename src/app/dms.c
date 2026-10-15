/* The direct messages screen: a list of conversations, with a back pill,
 * a refresh hint, and paging. Opening a row opens the conversation. */

#include "app/dms.h"
#include "atproto/dms.h"
#include "atproto/session.h"
#include "ui/postcard.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_dms_view_init(cobalt_dms_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   cobalt_listnav_init(&view->nav);
}

void
cobalt_dms_view_open(cobalt_dms_view *view)
{
   if (!view) {
      return;
   }
   cobalt_listnav_rewind(&view->nav);
   view->composing = false;
}

cobalt_dms_action
cobalt_dms_view_update(cobalt_dms_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_DMS_ACTION_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_DMS_ACTION_BACK;
   }

   const cobalt_convos *list = cobalt_session_dms_convos();
   const bool busy = cobalt_session_busy();
   if (busy) {
      return COBALT_DMS_ACTION_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_MENU)) {
      COBALT_LOGI("dms: refresh requested");
      if (cobalt_session_begin_dms_convos(false)) {
         cobalt_dms_view_open(view);
      }
      return COBALT_DMS_ACTION_STAY;
   }

   if (list->count == 0) {
      return COBALT_DMS_ACTION_STAY;
   }

   cobalt_listnav_move(&view->nav, in, list->count);
   cobalt_listnav_follow(&view->nav);

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM) &&
       view->nav.selected < list->count) {
      COBALT_LOGI("dms: opening convo %d", view->nav.selected);
      return COBALT_DMS_ACTION_OPEN_CONVO;
   }

   if (!busy && cobalt_convos_can_page(list) &&
       view->nav.selected >= list->count - 1) {
      cobalt_session_begin_dms_convos(true);
   }

   return COBALT_DMS_ACTION_STAY;
}

/* --- drawing --- */

#define CONVO_AVATAR_SIDE(m) ((m)->font_body * 3)
#define CONVO_AVATAR_GAP(m)  ((m)->pad_tile)

static int
row_height(cobalt_render *r, const cobalt_convo *item, const cobalt_metrics *m)
{
   (void) item;
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int h = m->pad_tile + body_h + m->pad_tile;

   const int avatar_min = m->pad_tile + CONVO_AVATAR_SIDE(m) + m->pad_tile;
   return h > avatar_min ? h : (avatar_min > caption_h ? avatar_min : caption_h);
}

static void
draw_row(cobalt_render *r, const cobalt_convo *item, const SDL_Rect *rect,
         bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   int y = rect->y + m->pad_tile;

   if (item->has_unread && item->unread > 0) {
      const int d = 10;
      SDL_Rect dot = { right - d - 14, rect->y + rect->h - d - 12, d, d };
      cobalt_fill_rounded_rect(r, &dot, d / 2, COBALT_COLOUR_ACCENT);
   }

    cobalt_avatar_draw(r, item->avatar, item->name[0] ? item->name : item->handle,
                       item->handle, left, y, CONVO_AVATAR_SIDE(m));
    const int text_left = left + CONVO_AVATAR_SIDE(m) + CONVO_AVATAR_GAP(m);

    int actor_x = text_left;
    cobalt_draw_text(r, COBALT_FONT_BODY, item->name[0] ? item->name : item->handle,
                     actor_x, y, COBALT_COLOUR_TEXT);

    int age_w = 0;
    if (item->age[0]) {
       cobalt_text_size(r, COBALT_FONT_CAPTION, item->age, &age_w, NULL);
       cobalt_draw_text(r, COBALT_FONT_CAPTION, item->age,
                        right - age_w, y + (body_h - caption_h) / 2,
                        COBALT_COLOUR_TEXT_DIM);
    }

    char summary[128];
    if (item->has_unread && item->unread > 0) {
       snprintf(summary, sizeof(summary), "%s (%d unread)",
                item->last_text[0] ? item->last_text : "No messages", item->unread);
    } else {
       snprintf(summary, sizeof(summary), "%s", item->last_text[0] ? item->last_text : "No messages");
    }
    cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, summary, text_left,
                             y + body_h + m->line_gap,
                             right - text_left - age_w - m->pad_tile, 1,
                             COBALT_COLOUR_TEXT_DIM);
}

void
cobalt_dms_view_draw(cobalt_dms_view *view, cobalt_render *r,
                     cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_convos *list = cobalt_session_dms_convos();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Messages", m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[96];
      if (cobalt_session_busy()) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else if (list->count == 0) {
         snprintf(subtitle, sizeof(subtitle), "%s", cobalt_session_handle());
      } else {
         snprintf(subtitle, sizeof(subtitle), "%s", cobalt_session_handle());
      }
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) - m->line_gap,
                       dim);
   }

   const int top = cobalt_content_top(r);
   const int bottom = m->height - m->pad_edge - 28;

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (list->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..." : "No conversations yet.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->nav, touchable);
      return;
   }

   int y = top;
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < list->count; i++) {
      const cobalt_convo *item = &list->items[i];
      const int h = row_height(r, item, m);

      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_row(r, item, &rect, i == view->nav.selected, m);
      last_fitted = i;
      y += h;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);
}
