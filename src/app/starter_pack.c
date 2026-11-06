#include "app/starter_pack.h"
#include "atproto/session.h"
#include "ui/postcard.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_starter_pack_view_init(cobalt_starter_pack_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   cobalt_listnav_init(&view->nav);
}

void
cobalt_starter_pack_view_open(cobalt_starter_pack_view *view,
                              const char *pack_uri)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   cobalt_listnav_init(&view->nav);
   if (pack_uri) {
      snprintf(view->uri, sizeof(view->uri), "%s", pack_uri);
   }
}

static bool
selected_feed(const cobalt_starter_pack_view *view,
              const cobalt_starter_pack *pack,
              const cobalt_actor_list *members, int *feed_index)
{
   const int index = view->nav.selected - (members ? members->count : 0);

   if (!pack || index < 0 || index >= pack->feed_count) {
      return false;
   }
   if (feed_index) {
      *feed_index = index;
   }
   return true;
}

cobalt_starter_pack_view_action
cobalt_starter_pack_view_update(cobalt_starter_pack_view *view,
                                const cobalt_input *in)
{
   const cobalt_starter_pack *pack;
   const cobalt_actor_list *members;
   int rows;
   int tapped;

   if (!view || !in) {
      return COBALT_STARTER_PACK_VIEW_STAY;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_STARTER_PACK_VIEW_BACK;
   }
   if (cobalt_session_busy()) {
      return COBALT_STARTER_PACK_VIEW_STAY;
   }

   pack = cobalt_session_starter_pack();
   members = cobalt_session_starter_pack_members();
   rows = cobalt_starter_pack_row_count(pack, members);
   if (rows <= 0) {
      return COBALT_STARTER_PACK_VIEW_STAY;
   }

   tapped = cobalt_listnav_move(&view->nav, in, rows);
   cobalt_listnav_follow(&view->nav);

   if (view->nav.selected < members->count &&
       cobalt_actor_list_can_page(members) &&
       view->nav.selected >= members->count - 1) {
      cobalt_session_begin_starter_pack(view->uri, true);
   }

   if (!(tapped >= 0 || cobalt_input_pressed(in, COBALT_BTN_CONFIRM))) {
      return COBALT_STARTER_PACK_VIEW_STAY;
   }

   if (view->nav.selected < members->count) {
      const cobalt_actor *actor = &members->actors[view->nav.selected];
      if (actor->did[0] && cobalt_session_begin_profile(actor->did)) {
         COBALT_LOGI("starter pack: opening profile %s", actor->did);
         return COBALT_STARTER_PACK_VIEW_OPEN_PROFILE;
      }
   } else {
      int feed_index = 0;
      if (selected_feed(view, pack, members, &feed_index) &&
          cobalt_session_begin_feed(pack->feeds[feed_index], false)) {
         snprintf(view->open_feed_uri, sizeof(view->open_feed_uri), "%s",
                  pack->feeds[feed_index]);
         return COBALT_STARTER_PACK_VIEW_OPEN_FEED;
      }
   }
   return COBALT_STARTER_PACK_VIEW_STAY;
}

/* --- drawing --- */

#define ROW_AVATAR_SIDE(m) ((m)->font_body * 2)
#define ROW_AVATAR_GAP(m)  ((m)->pad_tile / 2)

static int
member_row_height(const cobalt_metrics *m)
{
   const int h = m->pad_tile + ROW_AVATAR_SIDE(m) + m->pad_tile;
   return h > 0 ? h : m->font_body * 3;
}

static int
feed_row_height(const cobalt_metrics *m)
{
   return m->pad_tile * 2 + m->font_body * 2;
}

static void
draw_member_row(cobalt_render *r, const cobalt_actor *actor,
                const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int y = rect->y + m->pad_tile;
   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int handle_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int side = ROW_AVATAR_SIDE(m);
   const int text_left = left + side + ROW_AVATAR_GAP(m);
   const int text_y = y + (side - (name_h + handle_h)) / 2;

   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);
   cobalt_avatar_draw(r, actor->avatar, actor->display_name, actor->handle,
                      left, y, side);
   if (right - text_left <= 0) {
      return;
   }

   cobalt_draw_text(r, COBALT_FONT_BODY, actor->display_name, text_left,
                    text_y, COBALT_COLOUR_TEXT);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, actor->handle, text_left,
                    text_y + name_h, COBALT_COLOUR_TEXT_DIM);
}

static void
draw_feed_row(cobalt_render *r, int index, const char *uri, const SDL_Rect *rect,
              bool focused, const cobalt_metrics *m)
{
   char label[32];

   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);
   snprintf(label, sizeof(label), "Included feed %d", index + 1);
   cobalt_draw_text(r, COBALT_FONT_BODY, label, rect->x + m->pad_tile,
                    rect->y + m->pad_tile, COBALT_COLOUR_TEXT);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, uri, rect->x + m->pad_tile,
                    rect->y + m->pad_tile +
                       cobalt_font_line_height(r, COBALT_FONT_BODY),
                    COBALT_COLOUR_TEXT_DIM);
}

static int
draw_pack_header(cobalt_render *r, const cobalt_starter_pack *pack,
                 const cobalt_actor_list *members, int top)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int left = m->pad_edge;
   const int right = m->width - m->pad_edge;
   const int width = right - left;
   const bool reader = cobalt_render_reader(r);
   int y = top;

   cobalt_draw_text(r, COBALT_FONT_TITLE,
                    pack->name[0] ? pack->name : "Starter pack", left, y,
                    COBALT_COLOUR_HEADER_TEXT);
   y += cobalt_font_line_height(r, COBALT_FONT_TITLE);

   if (pack->creator.handle[0]) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, pack->creator.handle, left, y,
                       COBALT_COLOUR_TEXT_DIM);
      y += cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   }
   if (reader && pack->description[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, pack->description, left,
                               y, width, 2, COBALT_COLOUR_TEXT_DIM);
      y += cobalt_font_line_height(r, COBALT_FONT_CAPTION) * 2;
   }

   {
      char counts[96];
      const int total = pack->member_count > 0 ? pack->member_count : members->count;

      snprintf(counts, sizeof(counts), "%d member%s  %d feed%s", total,
               total == 1 ? "" : "s", pack->feed_count,
               pack->feed_count == 1 ? "" : "s");
      cobalt_draw_text(r, COBALT_FONT_CAPTION, counts, left, y,
                       COBALT_COLOUR_TEXT_DIM);
      y += cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   }

   return y + m->gap;
}

void
cobalt_starter_pack_view_draw(cobalt_starter_pack_view *view, cobalt_render *r,
                              cobalt_surface_id surface)
{
   const cobalt_starter_pack *pack;
   const cobalt_actor_list *members;
   const cobalt_metrics *m;
   const bool touchable = (surface == COBALT_SURFACE_DRC);
   int rows;
   int top;
   int bottom;
   int y;
   int last_fitted;

   if (!view || !r) {
      return;
   }
   pack = cobalt_session_starter_pack();
   members = cobalt_session_starter_pack_members();
   m = cobalt_render_metrics(r);
   rows = cobalt_starter_pack_row_count(pack, members);
   top = cobalt_content_top(r);
   bottom = cobalt_content_bottom(r);

   y = draw_pack_header(r, pack, members, top);

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (rows == 0) {
      SDL_Rect card = { m->pad_edge, y, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      const char *message = cobalt_session_busy()
                               ? "Loading starter pack..."
                               : "This starter pack has no available members or feeds.";
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, message,
                               card.x + m->pad_tile, card.y + m->pad_tile,
                               card.w - 2 * m->pad_tile, 2,
                               COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->nav, touchable);
      return;
   }

   last_fitted = view->nav.scroll;
   for (int index = view->nav.scroll; index < rows; index++) {
      const bool member = index < members->count;
      const int h = member ? member_row_height(m) : feed_row_height(m);
      SDL_Rect rect;
      const bool focused = (index == view->nav.selected);

      if (y + h > bottom && index > view->nav.scroll) {
         break;
      }
      rect.x = m->pad_edge;
      rect.y = y;
      rect.w = m->width - 2 * m->pad_edge;
      rect.h = h;

      if (member) {
         draw_member_row(r, &members->actors[index], &rect, focused, m);
      } else {
         const int feed_index = index - members->count;
         draw_feed_row(r, feed_index, pack->feeds[feed_index], &rect, focused, m);
      }
      cobalt_listnav_draw_add(&view->nav, touchable, &rect, index);
      last_fitted = index;
      y += h + m->gap;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);
}
