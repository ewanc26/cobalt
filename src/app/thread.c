#include "app/thread.h"
#include "atproto/session.h"
#include "ui/postcard.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

/*
 * A thread shows more of each post than the timeline does. The timeline is
 * something to skim; a thread is something someone chose to read, and cutting
 * a reply off after two lines is exactly the wrong place to save space.
 */
#define TEXT_LINES 4

void
cobalt_thread_view_init(cobalt_thread_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_thread_view_reset(cobalt_thread_view *view)
{
   if (!view) {
      return;
   }
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;
   view->centred = false;
}

cobalt_thread_action
cobalt_thread_view_update(cobalt_thread_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_THREAD_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_THREAD_VIEW_BACK;
   }

   const cobalt_thread *thread = cobalt_session_thread();
   const bool busy = cobalt_session_busy();

   if (thread->count == 0) {
      return COBALT_THREAD_VIEW_STAY;
   }

   /* Jump to the post the thread was opened on, once. Doing it here rather
    * than at fetch time means it survives the request completing after the
    * screen was already showing. */
   if (!view->centred && !busy) {
      view->selected = thread->focus;
      view->scroll = thread->focus;
      view->centred = true;
   }

   /*
    * A fetch can replace the conversation with a shorter one — re-rooting on a
    * reply does exactly that — leaving the cursor past the end. Nothing else
    * clamps downwards: the scroll maths below only ever raises `scroll` to
    * meet `selected`, so an out-of-range cursor renders an empty list that
    * takes one press of UP per row to escape.
    */
   cobalt_list_clamp(&view->selected, &view->scroll, thread->count);

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
       view->selected < thread->count - 1) {
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

   /* Same bindings as the timeline, deliberately: the card is the same card,
    * so the buttons that act on it should be the same buttons. */
   if (!busy && view->selected < thread->count) {
      const cobalt_post *post = &thread->posts[view->selected];

      /* A blocked or deleted placeholder has no URI to act on. */
      if (post->uri[0] && post->cid[0]) {
         if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
            cobalt_session_begin_like(post->uri, post->cid);
         } else if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
            cobalt_session_begin_repost(post->uri, post->cid);
         } else if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
            if (view->selected == thread->focus) {
               /* A is reply on the post being read, and navigation elsewhere.
                * Replying to the thing on screen is the common case and should
                * not need a different button from the one that acts on it. */
               return COBALT_THREAD_VIEW_REPLY;
            }
            /* Walking into a reply re-roots the thread on it, which is how a
             * long conversation stays navigable in a fixed buffer. */
            COBALT_LOGI("thread: re-rooting on %s", post->uri);
            cobalt_session_begin_thread(post->uri);
            cobalt_thread_view_reset(view);
         } else if (cobalt_input_pressed(in, COBALT_BTN_MENU)) {
            return COBALT_THREAD_VIEW_REPLY;
         }
      }
   }

   return COBALT_THREAD_VIEW_STAY;
}

void
cobalt_thread_view_draw(cobalt_thread_view *view, cobalt_render *r,
                        cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_thread *thread = cobalt_session_thread();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Thread", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);
   {
      char subtitle[96];
      if (cobalt_session_busy()) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else if (thread->count > 0) {
         snprintf(subtitle, sizeof(subtitle), "%d post%s%s", thread->count,
                  thread->count == 1 ? "" : "s",
                  thread->truncated ? " (shortened)" : "");
      } else {
         snprintf(subtitle, sizeof(subtitle), "Nothing to show");
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

   if (thread->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading the conversation..."
                                             : "This conversation is unavailable.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      if (touchable) {
         view->last_visible = -1;
      }
      return;
   }

   int y = top;
   int last_fitted = view->scroll;

   for (int i = view->scroll; i < thread->count; i++) {
      const cobalt_post *post = &thread->posts[i];
      const int h = cobalt_postcard_height(r, post, TEXT_LINES);

      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      cobalt_postcard_draw(r, post, &rect, i == view->selected, TEXT_LINES,
                           thread->depth[i]);

      /* The post the thread was opened on gets an accent edge, so it stays
       * findable after scrolling away from it and back. */
      if (i == thread->focus && i != view->selected) {
         SDL_Color edge = COBALT_COLOUR_ACCENT;
         edge.a = 140;
         SDL_Rect strip = { rect.x, rect.y, 3, rect.h };
         cobalt_fill_rect(r, &strip, edge);
      }

      if (touchable && view->hit_count < COBALT_THREAD_MAX_POSTS) {
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
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy()
                       ? "Working..."
                       : "A: open/reply   +: reply   Left: like   Right: repost   B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}
