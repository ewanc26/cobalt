#include "app/lists.h"
#include "atproto/session.h"
#include "ui/postcard.h"   /* cobalt_avatar_draw */
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_lists_view_init(cobalt_lists_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_lists_view_open(cobalt_lists_view *view)
{
   if (!view) {
      return;
   }
   view->browsing_members = false;
   view->open_uri[0] = '\0';
   view->open_name[0] = '\0';
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;

   if (cobalt_session_lists()->count == 0) {
      cobalt_session_begin_lists(false);
   }
}

/* --- update --- */

static cobalt_lists_view_action
update_list_of_lists(cobalt_lists_view *view, const cobalt_input *in)
{
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_LISTS_VIEW_BACK;
   }

   const cobalt_list_summary_list *lists = cobalt_session_lists();
   const bool busy = cobalt_session_busy();

   if (lists->count == 0) {
      return COBALT_LISTS_VIEW_STAY;
   }

   cobalt_list_clamp(&view->selected, &view->scroll, lists->count);

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected < lists->count - 1) {
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

   if (!busy && cobalt_list_summary_list_can_page(lists) &&
       view->selected >= lists->count - 1) {
      cobalt_session_begin_lists(true);
   }

   bool open = tapped_index >= 0 || cobalt_input_pressed(in, COBALT_BTN_CONFIRM);
   if (!busy && open && view->selected < lists->count) {
      const cobalt_list_summary *list = &lists->lists[view->selected];
      if (list->uri[0]) {
         COBALT_LOGI("lists: opening %s", list->name);
         snprintf(view->open_uri, sizeof(view->open_uri), "%s", list->uri);
         snprintf(view->open_name, sizeof(view->open_name), "%s", list->name);
         view->browsing_members = true;
         view->selected = 0;
         view->scroll = 0;
         view->last_visible = -1;
         cobalt_session_begin_list_members(list->uri, false);
      }
   }

   return COBALT_LISTS_VIEW_STAY;
}

static cobalt_lists_view_action
update_members(cobalt_lists_view *view, const cobalt_input *in)
{
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      view->browsing_members = false;
      view->selected = 0;
      view->scroll = 0;
      view->last_visible = -1;
      return COBALT_LISTS_VIEW_STAY;
   }

   const cobalt_actor_list *members = cobalt_session_list_members();
   const bool busy = cobalt_session_busy();

   if (members->count == 0) {
      return COBALT_LISTS_VIEW_STAY;
   }

   cobalt_list_clamp(&view->selected, &view->scroll, members->count);

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected < members->count - 1) {
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

   if (!busy && cobalt_actor_list_can_page(members) &&
       view->selected >= members->count - 1) {
      cobalt_session_begin_list_members(view->open_uri, true);
   }

   /* Members are read-only (see atproto/lists.h) — no A action here, unlike
    * graph.c's undo or search.c's open-profile. A future "open profile from
    * a list member" needs the same DID-forwarding search.c already does;
    * left out for now since nothing else forwards a DID out of this view. */

   return COBALT_LISTS_VIEW_STAY;
}

cobalt_lists_view_action
cobalt_lists_view_update(cobalt_lists_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_LISTS_VIEW_STAY;
   }
   if (cobalt_session_busy()) {
      return COBALT_LISTS_VIEW_STAY;
   }
   return view->browsing_members ? update_members(view, in)
                                 : update_list_of_lists(view, in);
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
draw_actor_row(cobalt_render *r, const cobalt_actor *actor,
               const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
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
draw_list_row(cobalt_render *r, const cobalt_list_summary *list,
             const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;

   cobalt_avatar_draw(r, list->avatar, list->name, list->name, left, y,
                      ROW_AVATAR_SIDE(m));
   const int text_left = left + ROW_AVATAR_SIDE(m) + ROW_AVATAR_GAP(m);
   if (right - text_left <= 0) {
      return;
   }

   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int side = ROW_AVATAR_SIDE(m);
   const bool has_desc = list->description[0] != '\0';
   const int desc_h = has_desc ? cobalt_font_line_height(r, COBALT_FONT_CAPTION) : 0;
   const int text_y = y + (side - (name_h + desc_h)) / 2;

   cobalt_draw_text(r, COBALT_FONT_BODY, list->name, text_left, text_y,
                    COBALT_COLOUR_TEXT);
   if (has_desc) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, list->description, text_left,
                       text_y + name_h, COBALT_COLOUR_TEXT_DIM);
   }
}

static void
draw_list_of_lists(cobalt_lists_view *view, cobalt_render *r,
                   cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_list_summary_list *lists = cobalt_session_lists();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Your lists", m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_TILE_FOCUS);

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int bottom = m->height - m->pad_edge - 28;

   if (touchable) {
      view->hit_count = 0;
   }

   if (lists->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..."
                                             : "You haven't made any lists yet.",
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

   for (int i = view->scroll; i < lists->count; i++) {
      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_list_row(r, &lists->lists[i], &rect, i == view->selected, m);

      if (touchable && view->hit_count < COBALT_LISTS_MAX) {
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
                                          : "A / touch: open list   B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

static void
draw_members(cobalt_lists_view *view, cobalt_render *r,
            cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_actor_list *members = cobalt_session_list_members();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, view->open_name, m->pad_edge,
                    m->pad_edge, COBALT_COLOUR_TILE_FOCUS);

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int bottom = m->height - m->pad_edge - 28;

   if (touchable) {
      view->hit_count = 0;
   }

   if (members->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading..."
                                             : "This list has no members.",
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

   for (int i = view->scroll; i < members->count; i++) {
      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_actor_row(r, &members->actors[i], &rect, i == view->selected, m);

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
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy() ? "Working..." : "B: back to lists",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

void
cobalt_lists_view_draw(cobalt_lists_view *view, cobalt_render *r,
                       cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }
   if (view->browsing_members) {
      draw_members(view, r, surface);
   } else {
      draw_list_of_lists(view, r, surface);
   }
}
