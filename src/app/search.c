#include "app/search.h"
#include "atproto/session.h"
#include "ui/postcard.h"   /* cobalt_avatar_draw */
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_search_view_init(cobalt_search_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_search_view_open(cobalt_search_view *view)
{
   if (!view) {
      return;
   }
   view->query[0] = '\0';
   view->browsing = false;
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;
   cobalt_keyboard_open(&view->kb, view->query, sizeof(view->query), false);
}

cobalt_search_view_action
cobalt_search_view_update(cobalt_search_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_SEARCH_VIEW_STAY;
   }

   if (cobalt_session_busy()) {
      return COBALT_SEARCH_VIEW_STAY;
   }

   if (!view->browsing) {
      switch (cobalt_keyboard_update(&view->kb, in)) {
         case COBALT_KB_ACCEPTED:
            if (view->query[0] == '\0') {
               return COBALT_SEARCH_VIEW_STAY;
            }
            COBALT_LOGI("search: querying '%s'", view->query);
            view->browsing = true;
            view->selected = 0;
            view->scroll = 0;
            view->last_visible = -1;
            cobalt_session_begin_search_actors(view->query, false);
            break;

         case COBALT_KB_CANCELLED:
            return COBALT_SEARCH_VIEW_BACK;

         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_SEARCH_VIEW_STAY;
   }

   /* Browsing results. B returns to the query rather than leaving the
    * screen — revising a search should not mean re-opening it from the menu. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->browsing = false;
      return COBALT_SEARCH_VIEW_STAY;
   }

   const cobalt_actor_list *results = cobalt_session_search_results();
   if (results->count == 0) {
      return COBALT_SEARCH_VIEW_STAY;
   }

   cobalt_list_clamp(&view->selected, &view->scroll, results->count);

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected < results->count - 1) {
      view->selected++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && view->selected > 0) {
      view->selected--;
   }

   int tapped_index = -1;
   if (view->hit_valid && in->touch_ended) {
      for (int i = 0; i < view->hit_count; i++) {
         if (cobalt_input_tapped(in, &view->hit[i])) {
            view->selected = view->hit_index[i];
            tapped_index = view->hit_index[i];
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

   if (cobalt_actor_list_can_page(results) &&
       view->selected >= results->count - 1) {
      cobalt_session_begin_search_actors(view->query, true);
   }

   /* A tap opens its row directly; CONFIRM opens whichever row is currently
    * selected — the same split timeline.c uses between tap-to-act and
    * D-pad-plus-button. DID (not handle) is what profile.c's fetch wants,
    * and search results already carry it, unlike timeline rows. */
   bool open_profile = tapped_index >= 0 ||
                        cobalt_input_pressed(in, COBALT_BTN_CONFIRM);
   if (open_profile && view->selected < results->count) {
      const cobalt_actor *actor = &results->actors[view->selected];
      if (actor->did[0]) {
         COBALT_LOGI("search: opening profile %s", actor->did);
         cobalt_session_begin_profile(actor->did);
         return COBALT_SEARCH_VIEW_OPEN_PROFILE;
      }
   }

   return COBALT_SEARCH_VIEW_STAY;
}

/* --- drawing --- */

#define ROW_AVATAR_SIDE(m) ((m)->font_body * 2)
#define ROW_AVATAR_GAP(m)  ((m)->pad_tile / 2)

static int
row_height(const cobalt_metrics *m)
{
   const int h = m->pad_tile + ROW_AVATAR_SIDE(m) + m->pad_tile;
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
                      left, y, ROW_AVATAR_SIDE(m));
   const int text_left = left + ROW_AVATAR_SIDE(m) + ROW_AVATAR_GAP(m);
   if (right - text_left <= 0) {
      return;
   }

   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int handle_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int side = ROW_AVATAR_SIDE(m);
   const int text_y = y + (side - (name_h + handle_h)) / 2;

   cobalt_draw_text(r, COBALT_FONT_BODY, actor->display_name, text_left,
                    text_y, COBALT_COLOUR_TEXT);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, actor->handle, text_left,
                    text_y + name_h, COBALT_COLOUR_TEXT_DIM);
}

static void
draw_editing(cobalt_search_view *view, cobalt_render *r,
            cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);

   SDL_Rect box = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                    m->pad_tile * 2 + cobalt_font_line_height(r, COBALT_FONT_BODY) };
   cobalt_draw_tile(r, &box, 1.0f);

   char display[COBALT_SEARCH_QUERY_MAX + 4];
   cobalt_keyboard_display_text(&view->kb, display, sizeof(display));
   const char *shown = display[0] ? display : "Search for an account...";
   cobalt_draw_text(r, COBALT_FONT_BODY, shown, box.x + m->pad_tile,
                    box.y + m->pad_tile,
                    display[0] ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM);

   SDL_Rect keys = { m->pad_edge, box.y + box.h + m->gap,
                     m->width - 2 * m->pad_edge,
                     cobalt_keyboard_height(surface) };
   cobalt_keyboard_draw(&view->kb, r, surface, &keys);
}

static void
draw_browsing(cobalt_search_view *view, cobalt_render *r,
             cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_actor_list *results = cobalt_session_search_results();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int bottom = m->height - m->pad_edge - 28;

   if (touchable) {
      view->hit_count = 0;
   }

   if (results->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Searching..."
                                             : "No accounts found.",
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

   for (int i = view->scroll; i < results->count; i++) {
      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_row(r, &results->actors[i], &rect, i == view->selected, m);

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
}

void
cobalt_search_view_draw(cobalt_search_view *view, cobalt_render *r,
                        cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_draw_text(r, COBALT_FONT_TITLE, "Search", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   if (view->browsing) {
      draw_browsing(view, r, surface);
      SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION,
                       cobalt_session_busy() ? "Searching..."
                                             : "B: new search",
                       m->pad_edge, m->height - m->pad_edge - 20, hint);
   } else {
      draw_editing(view, r, surface);
   }
}
