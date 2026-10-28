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
   cobalt_listnav_init(&view->nav);
}

void
cobalt_thread_view_reset(cobalt_thread_view *view)
{
   if (!view) {
      return;
   }
   cobalt_listnav_rewind(&view->nav);
   view->text_scroll = 0;
   view->detail_scroll = 0;
   view->text_for = 0;
   view->centred = false;
   view->confirm_delete = false;
}

cobalt_thread_action
cobalt_thread_view_update(cobalt_thread_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_THREAD_VIEW_STAY;
   }

   if (view->confirm_delete) {
      if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
         view->confirm_delete = false;
         if (!cobalt_session_busy()) {
            cobalt_session_begin_delete_post(view->delete_uri);
         }
      } else if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
         view->confirm_delete = false;
      }
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
      view->nav.selected = thread->focus;
      view->nav.scroll = thread->focus;
      view->centred = true;
   }

   /*
    * A fetch can replace the conversation with a shorter one — re-rooting on a
    * reply does exactly that — leaving the cursor past the end. Nothing else
    * clamps downwards: the scroll maths below only ever raises `scroll` to
    * meet `selected`, so an out-of-range cursor renders an empty list that
    * takes one press of UP per row to escape.
    */
   cobalt_list_clamp(&view->nav.selected, &view->nav.scroll, thread->count);
   cobalt_input_drag_list(in, &view->nav.selected, thread->count, view->nav.hit_valid ? view->nav.hit : NULL,
                          view->nav.hit_count);

   /* Down and Up read through a long post before leaving it. */
   const int text_max = view->text_total - view->text_window;
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      if (view->text_scroll < text_max) {
         view->text_scroll++;
      } else if (view->nav.selected < thread->count - 1) {
         view->nav.selected++;
      }
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      if (view->text_scroll > 0) {
         view->text_scroll--;
      } else if (view->nav.selected > 0) {
         view->nav.selected--;
      }
   }
   if (view->nav.selected != view->text_for) {
      view->text_for = view->nav.selected;
      view->text_scroll = 0;
      view->detail_scroll = 0;
      view->text_total = 0;
      view->text_window = 0;
   }

   /* L and R move the TV's detail column a line at a time. They never move the
    * selection, and the bound is the one the last TV draw measured. */
   if (cobalt_input_pressed(in, COBALT_BTN_SHOULDER_R) &&
       view->detail_scroll < view->detail_max) {
      view->detail_scroll++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_SHOULDER_L) && view->detail_scroll > 0) {
      view->detail_scroll--;
   }

   if (view->nav.hit_valid && in->touch_ended) {
      for (int i = 0; i < view->nav.hit_count; i++) {
         if (cobalt_input_tapped(in, &view->nav.hit[i])) {
            view->nav.selected = view->nav.hit_index[i];
            view->text_for = view->nav.selected;
            view->text_scroll = 0;
            view->detail_scroll = 0;
            view->text_total = 0;
            view->text_window = 0;
            break;
         }
      }
   }

   cobalt_listnav_follow(&view->nav);

   /* Same bindings as the timeline, deliberately: the card is the same card,
    * so the buttons that act on it should be the same buttons. */
   if (!busy && view->nav.selected < thread->count) {
      const cobalt_post *post = &thread->posts[view->nav.selected];

      /* A blocked or deleted placeholder has no URI to act on. */
      if (post->uri[0] && post->cid[0]) {
         if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
            cobalt_session_begin_like(post->uri, post->cid);
         } else if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
            cobalt_session_begin_repost(post->uri, post->cid);
         } else if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
            if (view->nav.selected == thread->focus) {
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
         } else if (cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
            return COBALT_THREAD_VIEW_QUOTE;
         } else if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
            return COBALT_THREAD_VIEW_MENU;
         }
      }
   }

   return COBALT_THREAD_VIEW_STAY;
}

/* The scrollbar for the selected post's text window, drawn along the right
 * edge of the card it belongs to. Shared by the full-width card and the TV's
 * detail column.
 *
 * Inset from the card edge by a full gap: at the very edge the 3px bar reads
 * as a second border beside the tile's own, and a focused card's ring sits
 * two pixels away from it. Inside the padding it reads as a scrollbar, and
 * the text column still clears it. */
static void
draw_text_scrollbar(cobalt_render *r, const cobalt_metrics *m, const SDL_Rect *rect,
                    int first, int win, int tot)
{
   const int track_h = rect->h - 2 * m->pad_tile;
   if (track_h <= 0 || tot <= 0 || win <= 0) {
      return;
   }
   int thumb_h = track_h * win / tot;
   if (thumb_h < 8) {
      thumb_h = 8;
   }
   const int span = tot - win;
   const int thumb_y = rect->y + m->pad_tile +
                       (span > 0 ? (track_h - thumb_h) * first / span : 0);
   SDL_Color track = COBALT_COLOUR_TEXT_DIM;
   track.a = 70;
   const int bar_x = rect->x + rect->w - m->pad_tile / 2;
   const SDL_Rect tr = { bar_x, rect->y + m->pad_tile, 3, track_h };
   const SDL_Rect th = { bar_x, thumb_y, 3, thumb_h };
   cobalt_fill_rect(r, &tr, track);
   cobalt_fill_rect(r, &th, COBALT_COLOUR_ACCENT);
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
   /* On the TV the thread is a column and the selected post is shown in full
    * beside it, as the timeline does. The GamePad keeps the full-width view. */
   const bool split = (surface == COBALT_SURFACE_TV);
   const int list_w = split ? (m->width - 2 * m->pad_edge - m->gap) * 2 / 5
                            : m->width - 2 * m->pad_edge;
   const int detail_x = m->pad_edge + list_w + m->gap;
   const int detail_w = m->width - m->pad_edge - detail_x;

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Thread", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE);
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
      SDL_Color dim = { 0xFF, 0xFF, 0xFF, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                       m->pad_edge +
                          cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                          m->line_gap,
                       dim);
   }

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (thread->count == 0) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading the conversation..."
                                             : "This conversation is unavailable.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      cobalt_listnav_draw_empty(&view->nav, touchable);
      return;
   }

   /* The selected post shows as much of its text as fits, scrolled by lines;
    * the others keep the fixed truncation. */
   int sel_lines = TEXT_LINES;
   int win = 0;
   int first = 0;
   int tot = 0;
   /* The TV column's "more" line is set again by every TV draw. */
   if (split) {
      view->detail_more = (SDL_Rect){ 0, 0, 0, 0 };
   }
   if (view->nav.selected >= 0 && view->nav.selected < thread->count) {
      const cobalt_post *sp = &thread->posts[view->nav.selected];
      const int full_w = split ? detail_w : m->width - 2 * m->pad_edge;
      const int total = cobalt_postcard_text_total(r, sp, full_w,
                                                   thread->depth[view->nav.selected]);
      const int line_h = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->line_gap;
      const int base = cobalt_postcard_height(r, sp, 0, true);
      int fit = line_h > 0 ? (bottom - top - base) / line_h : TEXT_LINES;
      /* A column whose text overflows keeps one line free for the "more" mark. */
      if (split && total > fit) {
         fit--;
      }
      if (fit < TEXT_LINES) {
         fit = TEXT_LINES;
      }
      sel_lines = total < fit ? (total > TEXT_LINES ? total : TEXT_LINES) : fit;
      win = sel_lines < total ? sel_lines : total;
      tot = total;
      /* The TV column has its own offset (detail_scroll); the GamePad card
       * keeps text_scroll and its read-through measure. */
      first = split ? view->detail_scroll : view->text_scroll;
      if (first > total - win) {
         first = total - win;
      }
      if (first < 0) {
         first = 0;
      }
      if (split) {
         /* As in the timeline, the draw shows a clamped offset but leaves the
          * state to update, which keeps it within detail_max. */
         view->detail_max = total - win > 0 ? total - win : 0;
      } else if (touchable) {
         view->text_total = total;
         view->text_window = win;
         view->text_scroll = first;
      }
   }

   /* Make sure the (possibly tall) selected card is actually on screen. */
   while (view->nav.scroll < view->nav.selected) {
      int span = 0;
      for (int k = view->nav.scroll; k <= view->nav.selected; k++) {
         /* On the TV the selected post is not in the list column, so it is
          * measured at list height there. */
         const bool full = (k == view->nav.selected) && !split;
         span += cobalt_postcard_height(r, &thread->posts[k],
                                        full ? sel_lines : TEXT_LINES,
                                        k == view->nav.selected) + m->gap;
      }
      if (span <= bottom - top) {
         break;
      }
      view->nav.scroll++;
   }

   int y = top;
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < thread->count; i++) {
      const cobalt_post *post = &thread->posts[i];
      const bool in_full = (i == view->nav.selected) && !split;
      const int lines = in_full ? sel_lines : TEXT_LINES;
      const int h = cobalt_postcard_height(r, post, lines, i == view->nav.selected);

      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, list_w, h };
      cobalt_postcard_draw_scrolled(r, post, &rect, i == view->nav.selected, lines,
                                    thread->depth[i], in_full ? first : 0);

      /* As a remote the pad shows one line of each post, so there is no
       * window of text left to point at — the TV has the whole thing. */
      if (in_full && !cobalt_render_remote(r) && tot > win && win > 0) {
         draw_text_scrollbar(r, m, &rect, first, win, tot);
      }

      /* The post the thread was opened on gets an accent edge, so it stays
       * findable after scrolling away from it and back. */
      if (i == thread->focus && i != view->nav.selected) {
         SDL_Color edge = COBALT_COLOUR_ACCENT;
         edge.a = 140;
         SDL_Rect strip = { rect.x, rect.y, 3, rect.h };
         cobalt_fill_rect(r, &strip, edge);
      }

      cobalt_listnav_draw_add(&view->nav, touchable, &rect, i);

      last_fitted = i;
      y += h + m->gap;
   }

   /* The selected post in the right-hand column, with the window of its text
    * that the scroll state picks. Lines past the screen are cut, not wrapped
    * back into the list. */
   if (split && view->nav.selected >= 0 && view->nav.selected < thread->count) {
      const cobalt_post *sp = &thread->posts[view->nav.selected];
      const int x = detail_x;
      const int h = cobalt_postcard_height(r, sp, sel_lines, true);
      const SDL_Rect rect = { x, top, detail_w, h };

      cobalt_postcard_draw_scrolled(r, sp, &rect, true, sel_lines,
                                    thread->depth[view->nav.selected], first);
      if (tot > win && win > 0) {
         draw_text_scrollbar(r, m, &rect, first, win, tot);
         const SDL_Rect column = { x, top, detail_w, bottom - top };
         view->detail_more = cobalt_postcard_draw_more(r, &column, bottom);
      }
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   char footer[120];
   if (view->confirm_delete) {
      snprintf(footer, sizeof footer, "Delete this post for good?   A: delete   B: keep it");
   } else if (cobalt_session_busy()) {
      snprintf(footer, sizeof footer, "Working...");
   } else if (split && view->detail_more.w > 0) {
      snprintf(footer, sizeof footer,
               "A: open/reply   Y: more   Left: like   Right: repost   L/R: scroll text");
   } else {
      snprintf(footer, sizeof footer, "A: open/reply   Y: more   Left: like   Right: repost");
   }
   cobalt_draw_hints(r, footer);
}
