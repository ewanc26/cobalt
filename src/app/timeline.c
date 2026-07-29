#include "app/timeline.h"
#include "atproto/session.h"
#include "ui/postcard.h"
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

   /* A refresh can return fewer posts than were on screen. Every current path
    * that shrinks the feed also rewinds the view, but that is a property of
    * the call graph rather than an invariant, so clamp here too. */
   cobalt_list_clamp(&view->selected, &view->scroll, feed->count);

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
            /* A tap selects; opening the thread is a second, deliberate press.
             * A single tap that navigated would make scrolling by touch on a
             * dense list feel like a minefield. */
            view->selected = view->hit_index[i];
            break;
         }
      }
   }

   /*
    * Left and right are free on a vertical list, so they carry the two
    * interactions. Putting them on the D-pad rather than behind a menu is what
    * makes them usable one-handed with the GamePad resting on a lap, and the
    * footer names them so they are discoverable.
    */
   if (!busy && view->selected < feed->count) {
      const cobalt_post *post = &feed->posts[view->selected];
      if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
         cobalt_session_begin_like(post->uri, post->cid);
      } else if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
         cobalt_session_begin_repost(post->uri, post->cid);
      } else if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
         COBALT_LOGI("timeline: opening thread %s", post->uri);
         cobalt_session_begin_thread(post->uri);
         return COBALT_TIMELINE_OPEN_THREAD;
      } else if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
         /* The handle is stored with its leading @, which getProfile will not
          * accept as an actor. */
         const char *actor = post->handle[0] == '@' ? post->handle + 1
                                                    : post->handle;
         if (actor[0]) {
            COBALT_LOGI("timeline: opening profile %s", actor);
            cobalt_session_begin_profile(actor);
            return COBALT_TIMELINE_OPEN_PROFILE;
         }
      } else if (cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
         return COBALT_TIMELINE_COMPOSE;
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
   if (!busy && cobalt_feed_can_page(feed) && view->selected >= feed->count - 1) {
      COBALT_LOGI("timeline: reached the end, fetching the next page");
      cobalt_session_begin_timeline(true);
   }

   return COBALT_TIMELINE_STAY;
}

/* --- drawing --- */

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
      const int h = cobalt_postcard_height(r, post, TEXT_LINES);

      /* Stop before drawing a card that would run off the bottom. Always draw
       * at least one, so a card taller than the viewport is still readable
       * rather than the screen going blank. */
      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      cobalt_postcard_draw(r, post, &rect, i == view->selected,
                           TEXT_LINES, 0);

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
                           ? "Working..."
                           : "A: thread  Y: profile  X: post  Left: like  Right: repost  +: refresh";
   cobalt_draw_text(r, COBALT_FONT_CAPTION, footer, m->pad_edge,
                    m->height - m->pad_edge - 20, hint);
}
