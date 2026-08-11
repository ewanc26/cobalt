#include "app/graph.h"
#include "atproto/session.h"
#include "ui/postcard.h"   /* cobalt_avatar_draw */
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

static const cobalt_actor_list *
list_for(cobalt_graph_kind kind)
{
   return kind == COBALT_GRAPH_MUTED ? cobalt_session_muted_list()
                                     : cobalt_session_blocked_list();
}

static const char *
title_for(cobalt_graph_kind kind)
{
   return kind == COBALT_GRAPH_MUTED ? "Muted accounts" : "Blocked accounts";
}

static const char *
empty_message_for(cobalt_graph_kind kind)
{
   return kind == COBALT_GRAPH_MUTED ? "No muted accounts."
                                     : "No blocked accounts.";
}

void
cobalt_graph_view_init(cobalt_graph_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_graph_view_open(cobalt_graph_view *view, cobalt_graph_kind kind)
{
   if (!view) {
      return;
   }
   view->kind = kind;
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;

   /* Only fetch if there is nothing to show — re-opening should not throw
    * away a scroll position, same rule the timeline/notifications entry
    * points already follow. */
   if (list_for(kind)->count == 0) {
      if (kind == COBALT_GRAPH_MUTED) {
         cobalt_session_begin_muted_list(false);
      } else {
         cobalt_session_begin_blocked_list(false);
      }
   }
}

cobalt_graph_view_action
cobalt_graph_view_update(cobalt_graph_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_GRAPH_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_GRAPH_VIEW_BACK;
   }

   const cobalt_actor_list *list = list_for(view->kind);
   const bool busy = cobalt_session_busy();

   if (list->count == 0) {
      return COBALT_GRAPH_VIEW_STAY;
   }

   cobalt_list_clamp(&view->selected, &view->scroll, list->count);

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

   /* A on a row always undoes — every row here is, by definition, already
    * muted or blocked. */
   if (!busy && view->selected < list->count &&
       cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      const cobalt_actor *actor = &list->actors[view->selected];
      if (view->kind == COBALT_GRAPH_MUTED) {
         COBALT_LOGI("graph: unmuting %s", actor->did);
         cobalt_session_begin_unmute_actor(actor->did);
      } else {
         COBALT_LOGI("graph: unblocking %s", actor->did);
         cobalt_session_begin_unblock_actor(actor->record_uri, actor->did);
      }
   }

   if (!busy && cobalt_actor_list_can_page(list) &&
       view->selected >= list->count - 1) {
      if (view->kind == COBALT_GRAPH_MUTED) {
         cobalt_session_begin_muted_list(true);
      } else {
         cobalt_session_begin_blocked_list(true);
      }
   }

   return COBALT_GRAPH_VIEW_STAY;
}

/* --- drawing --- */

/* Matches the post card's and the notification row's, so an account looks
 * the same size everywhere it appears. */
#define GRAPH_AVATAR_SIDE(m) ((m)->font_body * 2)
#define GRAPH_AVATAR_GAP(m)  ((m)->pad_tile / 2)

static int
row_height(const cobalt_metrics *m)
{
   const int h = m->pad_tile + GRAPH_AVATAR_SIDE(m) + m->pad_tile;
   return h > 0 ? h : m->font_body * 3;
}

static void
draw_row(cobalt_render *r, const cobalt_actor *actor, const SDL_Rect *rect,
        bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   cobalt_avatar_draw(r, actor->avatar, actor->display_name, actor->handle,
                      left, y, GRAPH_AVATAR_SIDE(m));
   const int text_left = left + GRAPH_AVATAR_SIDE(m) + GRAPH_AVATAR_GAP(m);
   if (right - text_left <= 0) {
      return;
   }

   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int handle_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int side = GRAPH_AVATAR_SIDE(m);
   const int text_y = y + (side - (name_h + handle_h)) / 2;

   cobalt_draw_text(r, COBALT_FONT_BODY, actor->display_name, text_left,
                    text_y, COBALT_COLOUR_TEXT);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, actor->handle, text_left,
                    text_y + name_h, COBALT_COLOUR_TEXT_DIM);
}

void
cobalt_graph_view_draw(cobalt_graph_view *view, cobalt_render *r,
                       cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_actor_list *list = list_for(view->kind);
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, title_for(view->kind), m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_TILE_FOCUS);

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
                       cobalt_session_busy() ? "Loading..."
                                             : empty_message_for(view->kind),
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      if (touchable) {
         view->last_visible = -1;
      }
      return;
   }

   const int h = row_height(m);
   int y = top;
   int last_fitted = view->scroll;

   for (int i = view->scroll; i < list->count; i++) {
      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_row(r, &list->actors[i], &rect, i == view->selected, m);

      if (touchable && view->hit_count < COBALT_ACTORS_MAX) {
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
   const char *action = view->kind == COBALT_GRAPH_MUTED ? "unmute" : "unblock";
   char hint_text[64];
   snprintf(hint_text, sizeof(hint_text), "A: %s   B: back", action);
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy() ? "Working..." : hint_text,
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}
