#include "app/timeline.h"
#include "atproto/session.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

/* Lines of post text a card shows before truncating. Two is what fits on the
 * GamePad without dropping to a font size that is uncomfortable at arm's
 * length; the full post belongs in a thread view, which is not built yet. */
#define TEXT_LINES 2

void
cobalt_timeline_init(cobalt_timeline *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_timeline_rewind(cobalt_timeline *view)
{
   if (!view) {
      return;
   }
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;
}

/* --- input --- */

cobalt_timeline_action
cobalt_timeline_update(cobalt_timeline *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_TIMELINE_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_TIMELINE_BACK;
   }

   const cobalt_feed *feed = cobalt_session_feed();
   const bool busy = cobalt_session_busy();

   /* Start refreshes even with an empty feed — that is exactly the state where
    * someone wants one. */
   if (cobalt_input_pressed(in, COBALT_BTN_MENU) && !busy) {
      COBALT_LOGI("timeline: refresh requested");
      if (cobalt_session_begin_timeline(false)) {
         cobalt_timeline_rewind(view);
      }
      return COBALT_TIMELINE_STAY;
   }

   if (feed->count == 0) {
      return COBALT_TIMELINE_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      if (view->selected < feed->count - 1) {
         view->selected++;
      }
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      if (view->selected > 0) {
         view->selected--;
      }
   }

   if (view->hit_valid && in->touch_ended) {
      for (int i = 0; i < view->hit_count; i++) {
         if (cobalt_input_tapped(in, &view->hit[i])) {
            view->selected = view->hit_index[i];
            break;
         }
      }
   }

   /* Keep the selection in view. Scrolling back is exact; scrolling forward
    * uses what the last frame actually fitted, since card heights vary and are
    * only known after a draw. */
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

   /*
    * Reaching the last loaded post fetches the next page. Doing it on arrival
    * rather than behind a "load more" button is the right trade on a console:
    * the request is already asynchronous, and a D-pad is a slow way to reach a
    * button that exists only to say "yes, continue".
    */
   if (!busy && feed->has_more && view->selected >= feed->count - 1) {
      COBALT_LOGI("timeline: reached the end, fetching the next page");
      cobalt_session_begin_timeline(true);
   }

   return COBALT_TIMELINE_STAY;
}

/* --- drawing --- */

static int
card_height(cobalt_render *r, const cobalt_post *post, const cobalt_metrics *m)
{
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   int h = m->pad_tile;                    /* top padding */
   if (post->reposted_by[0]) {
      h += caption_h;
   }
   h += body_h;                            /* author row */
   h += TEXT_LINES * (body_h + m->line_gap);
   if (post->meta[0] || post->embed_note[0]) {
      h += caption_h;
   }
   h += m->pad_tile;                       /* bottom padding */

   /* A degenerate value here would make the scroll maths divide the screen
    * into an unbounded number of cards; the font failing to load is already
    * reported on the diagnostics screen. */
   return h > 0 ? h : m->font_body * 4;
}

static void
draw_card(cobalt_render *r, const cobalt_post *post, const SDL_Rect *rect,
          bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int left = rect->x + m->pad_tile;
   const int width = rect->w - 2 * m->pad_tile;
   int y = rect->y + m->pad_tile;

   if (post->reposted_by[0]) {
      char banner[COBALT_POST_NAME_MAX + 16];
      snprintf(banner, sizeof(banner), "Reposted by %s", post->reposted_by);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, banner, left, y,
                       COBALT_COLOUR_TEXT_DIM);
      y += caption_h;
   }

   /* Author row. The age is right-aligned, so it is measured and placed from
    * the right edge rather than flowed after the handle, whose width varies
    * wildly with the display name. */
   int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, post->author, left, y,
                                 COBALT_COLOUR_TEXT);

   int age_w = 0;
   if (post->age[0]) {
      cobalt_text_size(r, COBALT_FONT_CAPTION, post->age, &age_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, post->age,
                       rect->x + rect->w - m->pad_tile - age_w,
                       y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
   }

   /* The handle fills whatever is left between the name and the age. It is
    * dropped rather than overlapped when there is no room — on a narrow
    * GamePad card a long display name legitimately uses the whole row. */
   const int handle_x = left + name_w + m->pad_tile / 2;
   const int handle_room = (rect->x + rect->w - m->pad_tile - age_w - m->pad_tile)
                           - handle_x;
   if (handle_room > 0) {
      int handle_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, post->handle, &handle_w, NULL);
      if (handle_w <= handle_room) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, post->handle, handle_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
   y += body_h;

   /* An image-only post has no text at all, which is legitimate — the embed
    * note below is then the only thing describing it. */
   if (post->text[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, post->text, left, y, width,
                               TEXT_LINES, COBALT_COLOUR_TEXT);
   }
   y += TEXT_LINES * (body_h + m->line_gap);

   if (post->meta[0] || post->embed_note[0]) {
      char footer[COBALT_POST_META_MAX + 40];
      if (post->meta[0] && post->embed_note[0]) {
         snprintf(footer, sizeof(footer), "%s   %s", post->meta, post->embed_note);
      } else {
         snprintf(footer, sizeof(footer), "%s",
                  post->meta[0] ? post->meta : post->embed_note);
      }
      cobalt_draw_text(r, COBALT_FONT_CAPTION, footer, left, y,
                       COBALT_COLOUR_TEXT_DIM);
   }
}

static void
draw_empty(cobalt_render *r, const cobalt_metrics *m, int top)
{
   const char *headline = cobalt_session_busy() ? "Loading your timeline..."
                                                : "Nothing here yet";
   const char *detail = cobalt_session_busy()
                           ? ""
                           : "Press + to refresh. If this stays empty, follow "
                             "some accounts and they will show up here.";

   SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                     m->font_body * 5 };
   cobalt_draw_tile(r, &card, 0.0f);

   cobalt_draw_text(r, COBALT_FONT_HEADING, headline, card.x + m->pad_tile,
                    card.y + m->pad_tile, COBALT_COLOUR_TEXT);
   if (detail[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, detail,
                               card.x + m->pad_tile,
                               card.y + m->pad_tile +
                                  cobalt_font_line_height(r, COBALT_FONT_HEADING),
                               card.w - 2 * m->pad_tile, 2, COBALT_COLOUR_TEXT_DIM);
   }
}

void
cobalt_timeline_draw(cobalt_timeline *view, cobalt_render *r,
                     cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_feed *feed = cobalt_session_feed();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   /* Header. */
   cobalt_draw_text(r, COBALT_FONT_TITLE, "Timeline", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[96];
      if (cobalt_session_busy()) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else if (feed->count > 0) {
         snprintf(subtitle, sizeof(subtitle), "%d of %d", view->selected + 1,
                  feed->count);
      } else {
         snprintf(subtitle, sizeof(subtitle), "%s", cobalt_session_handle());
      }
      SDL_Color dim = { 0xD8, 0xE6, 0xF4, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int bottom = m->height - m->pad_edge - 28;

   if (feed->count == 0) {
      draw_empty(r, m, top);
      if (touchable) {
         view->hit_count = 0;
         view->last_visible = -1;
      }
      return;
   }

   if (touchable) {
      view->hit_count = 0;
   }

   int y = top;
   int last_fitted = view->scroll;

   for (int i = view->scroll; i < feed->count; i++) {
      const cobalt_post *post = &feed->posts[i];
      const int h = card_height(r, post, m);

      /* Stop before drawing a card that would run off the bottom. Always draw
       * at least one, so a card taller than the viewport is still readable
       * rather than the screen going blank. */
      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_card(r, post, &rect, i == view->selected, m);

      if (touchable && view->hit_count < COBALT_FEED_MAX_POSTS) {
         view->hit[view->hit_count] = rect;
         view->hit_index[view->hit_count] = i;
         view->hit_count++;
      }

      last_fitted = i;
      y += h + m->gap;
   }

   if (touchable) {
      view->hit_valid = true;
      view->last_visible = last_fitted;
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   const char *footer = cobalt_session_busy()
                           ? "Loading more..."
                           : "D-pad: move    +: refresh    B: back";
   cobalt_draw_text(r, COBALT_FONT_CAPTION, footer, m->pad_edge,
                    m->height - m->pad_edge - 20, hint);
}
