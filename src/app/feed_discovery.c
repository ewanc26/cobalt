#include "app/feed_discovery.h"
#include "ui/postcard.h"   /* cobalt_avatar_draw */
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void
cobalt_feed_discovery_view_init(cobalt_feed_discovery_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->remove_confirm = -1;
   cobalt_listnav_init(&view->nav);
}

void
cobalt_feed_discovery_view_open(cobalt_feed_discovery_view *view)
{
   if (!view) {
      return;
   }
   view->browsing_detail = false;
   view->open_uri[0] = '\0';
   view->saving = false;
   view->remove_confirm = -1;
   view->status[0] = '\0';
   view->status_error = false;
   cobalt_listnav_rewind(&view->nav);

   /* Only one request can be in flight; the v2 saved list is chained from the
    * discovery result in app.c when it is still empty. */
   if (cobalt_session_feed_discovery_results()->count == 0) {
      cobalt_session_begin_feed_discovery(NULL);
   }
}

void
cobalt_feed_discovery_view_open_feed(cobalt_feed_discovery_view *view,
                                     const char *feed_uri)
{
   if (!view || !feed_uri || !feed_uri[0]) {
      return;
   }
   view->browsing_detail = true;
   snprintf(view->open_uri, sizeof(view->open_uri), "%s", feed_uri);
   view->saving = false;
   view->remove_confirm = -1;
   view->status[0] = '\0';
   view->status_error = false;
   cobalt_listnav_rewind(&view->nav);
   cobalt_session_begin_feed_generator(feed_uri);
}

int
cobalt_feed_saved_state(const char *feed_uri)
{
   if (!feed_uri || !feed_uri[0]) {
      return -1;
   }
   const cobalt_saved_feeds_v2 *saved = cobalt_session_saved_feeds_v2();
   for (int i = 0; i < saved->count; i++) {
      if (strcmp(saved->entries[i].uri, feed_uri) == 0) {
         return saved->entries[i].pinned ? 1 : 0;
      }
   }
   return -1;
}

static void
set_status(cobalt_feed_discovery_view *view, bool error, const char *text)
{
   snprintf(view->status, sizeof(view->status), "%s", text ? text : "");
   view->status_error = error;
}

void
cobalt_feed_discovery_view_result(cobalt_feed_discovery_view *view,
                                  cobalt_job_kind kind, bool ok,
                                  const char *message)
{
   if (!view) {
      return;
   }
   switch (kind) {
      case COBALT_JOB_SAVE_FEED:
      case COBALT_JOB_REMOVE_FEED:
      case COBALT_JOB_PIN_FEED:
         view->saving = false;
         view->remove_confirm = -1;
         set_status(view, !ok, message && message[0] ? message
                                                     : (ok ? "Done." : "That did not work."));
         /* app.c chains the saved-list refreshes from the mutation result:
          * only one request can be in flight at a time. */
         break;
      default:
         /* Discovery, generator detail and saved-feeds reads surface through
          * the lists themselves; only failures are worth a status line. */
         if (!ok) {
            set_status(view, true, message && message[0] ? message
                                                         : "That did not work.");
         }
         break;
   }
}

/* --- update --- */

static cobalt_feed_discovery_view_action
update_browse(cobalt_feed_discovery_view *view, const cobalt_input *in)
{
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_FEED_DISCOVERY_VIEW_BACK;
   }

   const cobalt_feed_discovery_page *page = cobalt_session_feed_discovery_results();
   const bool busy = cobalt_session_busy();

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
      cobalt_session_begin_feed_discovery(NULL);
      set_status(view, false, "Refreshing...");
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   if (page->count == 0) {
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   const int tapped_index = cobalt_listnav_move(&view->nav, in, page->count);

   cobalt_listnav_follow(&view->nav);

   if (!busy && page->has_more && view->nav.selected >= page->count - 1) {
      cobalt_session_begin_feed_discovery(page->cursor);
   }

   bool open = tapped_index >= 0 || cobalt_input_pressed(in, COBALT_BTN_CONFIRM);
   if (!busy && open && view->nav.selected < page->count) {
      const cobalt_feed_generator *g = &page->generators[view->nav.selected];
      if (g->uri[0]) {
         COBALT_LOGI("feed discovery: inspecting %s", g->display_name);
         snprintf(view->open_uri, sizeof(view->open_uri), "%s", g->uri);
         view->browsing_detail = true;
         view->remove_confirm = -1;
         view->status[0] = '\0';
         view->status_error = false;
         cobalt_listnav_rewind(&view->nav);
         cobalt_session_begin_feed_generator(g->uri);
      }
   }

   return COBALT_FEED_DISCOVERY_VIEW_STAY;
}

static cobalt_feed_discovery_view_action
update_detail(cobalt_feed_discovery_view *view, const cobalt_input *in)
{
   if (view->saving) {
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      if (view->remove_confirm >= 0) {
         view->remove_confirm = -1;
         view->status[0] = '\0';
         return COBALT_FEED_DISCOVERY_VIEW_STAY;
      }
      view->browsing_detail = false;
      view->remove_confirm = -1;
      view->status[0] = '\0';
      cobalt_listnav_rewind(&view->nav);
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   const bool busy = cobalt_session_busy();
   const char *uri = view->open_uri;
   const int saved_state = cobalt_feed_saved_state(uri);

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      if (uri[0]) {
         return COBALT_FEED_DISCOVERY_VIEW_OPEN_FEED;
      }
   }

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      if (saved_state >= 0) {
         set_status(view, false, "Already in your feeds.");
      } else if (!cobalt_session_begin_save_feed(uri, false)) {
         set_status(view, true, "Could not start saving that feed.");
      } else {
         view->saving = true;
         set_status(view, false, "Saving feed...");
      }
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_SHOULDER_R)) {
      if (saved_state < 0) {
         set_status(view, true, "Save the feed before pinning it.");
      } else if (!cobalt_session_begin_pin_feed(uri, saved_state != 1)) {
         set_status(view, true, "Could not start changing the pin.");
      } else {
         view->saving = true;
         set_status(view, false, saved_state == 1 ? "Unpinning..." : "Pinning...");
      }
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   if (!busy && cobalt_input_pressed(in, COBALT_BTN_ALT_X)) {
      if (saved_state < 0) {
         set_status(view, true, "That feed is not saved.");
      } else if (view->remove_confirm == 1) {
         view->remove_confirm = -1;
         if (!cobalt_session_begin_remove_feed(uri)) {
            set_status(view, true, "Could not start removing that feed.");
         } else {
            view->saving = true;
            set_status(view, false, "Removing feed...");
         }
      } else {
         view->remove_confirm = 1;
         set_status(view, true, "Press X again to remove this feed.");
      }
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }

   return COBALT_FEED_DISCOVERY_VIEW_STAY;
}

cobalt_feed_discovery_view_action
cobalt_feed_discovery_view_update(cobalt_feed_discovery_view *view,
                                  const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_FEED_DISCOVERY_VIEW_STAY;
   }
   return view->browsing_detail ? update_detail(view, in)
                                : update_browse(view, in);
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
draw_generator_row(cobalt_render *r, const cobalt_feed_generator *g,
                   const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int left = rect->x + m->pad_tile;
   const int right = rect->x + rect->w - m->pad_tile;
   const int side = ROW_AVATAR_SIDE(m);
   const int y = rect->y + (rect->h - side) / 2;

   cobalt_avatar_draw(r, g->avatar, g->display_name, g->did, left, y, side);
   const int text_left = left + side + ROW_AVATAR_GAP(m);
   if (right - text_left <= 0) {
      return;
   }

   const int name_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const bool has_desc = g->description[0] != '\0';
   const int desc_h = has_desc ? cobalt_font_line_height(r, COBALT_FONT_CAPTION) : 0;
   const int text_y = y + (side - (name_h + desc_h)) / 2;
   const int text_w = right - text_left;

   cobalt_draw_text_fitted(r, COBALT_FONT_BODY, g->display_name, text_left, text_y,
                           text_w, COBALT_COLOUR_TEXT);
   if (has_desc) {
      cobalt_draw_text_fitted(r, COBALT_FONT_CAPTION, g->description, text_left,
                              text_y + name_h, text_w, COBALT_COLOUR_TEXT_DIM);
   }
}

static void
draw_hint_line(cobalt_feed_discovery_view *view, cobalt_render *r,
               const char *controls)
{
   char hint[COBALT_FEED_STATUS_MAX + 80];
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int y = m->height - m->pad_edge - cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   if (view->status[0]) {
      snprintf(hint, sizeof(hint), "%s  %s", view->status,
               controls ? controls : "");
      cobalt_draw_text_fitted(r, COBALT_FONT_CAPTION, hint, m->pad_edge, y,
                              m->width - 2 * m->pad_edge,
                              view->status_error ? COBALT_COLOUR_ERROR
                                                  : COBALT_COLOUR_TEXT_DIM);
   } else if (controls) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, controls, m->pad_edge, y,
                       COBALT_COLOUR_TEXT_DIM);
   }
}

static void
draw_browse(cobalt_feed_discovery_view *view, cobalt_render *r,
            cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_feed_discovery_page *page = cobalt_session_feed_discovery_results();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_page_title(r, "Discover feeds", "suggested");

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (page->count == 0) {
      const bool busy = cobalt_session_busy();
      cobalt_draw_empty_state(r, top, bottom,
                              busy ? "Discovering feeds..." : "No feeds discovered",
                              busy ? "" : "Press X to try again.");
      cobalt_listnav_draw_empty(&view->nav, touchable);
      draw_hint_line(view, r, busy ? "Working..." : "X: refresh   B: back");
      return;
   }

   const int gap = m->gap / 2;
   int h = row_height(m);
   int y = top;
   if (surface == COBALT_SURFACE_DRC) {
      int rows = page->count - view->nav.scroll;
      const int fit = (bottom - top + gap) / (h + gap);
      if (rows > fit) {
         rows = fit > 0 ? fit : 1;
      }
      y += cobalt_list_fill(bottom - top, rows, gap, h, &h);
   }
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < page->count; i++) {
      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      draw_generator_row(r, &page->generators[i], &rect,
                         i == view->nav.selected, m);

      cobalt_listnav_draw_add(&view->nav, touchable, &rect, i);

      last_fitted = i;
      y += h + gap;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   char controls[96];
   snprintf(controls, sizeof(controls), "%s%sB: back",
            cobalt_session_busy() ? "Working...   " : "A: inspect   X: refresh   ",
            page->has_more ? "more below   " : "");
   draw_hint_line(view, r, controls);
}

static void
draw_detail(cobalt_feed_discovery_view *view, cobalt_render *r,
            cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_feed_generator *g = cobalt_session_feed_generator();
   const bool busy = cobalt_session_busy();
   (void) surface;

   cobalt_draw_page_title(r, (g && g->display_name[0]) ? g->display_name : "Feed",
                         NULL);

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const int left = m->pad_edge;
   const int width = m->width - 2 * m->pad_edge;

   int y = top;
   if (!busy && g) {
      const int side = m->font_body * 3;
      cobalt_avatar_draw(r, g->avatar, g->display_name, g->did, left, y, side);
      const int text_left = left + side + m->gap;
      if (m->width - m->pad_edge - text_left > 0) {
         char stats[128];
         stats[0] = '\0';
         if (g->has_like_count) {
            snprintf(stats, sizeof(stats), "%lld likes", (long long) g->like_count);
         }
         const int saved_state = cobalt_feed_saved_state(view->open_uri);
         const char *saved_label =
            saved_state == 1 ? "saved, pinned" : saved_state == 0 ? "saved" : NULL;
         if (saved_label) {
            snprintf(stats + strlen(stats), sizeof(stats) - strlen(stats),
                     "%s%s", stats[0] ? ", " : "", saved_label);
         }
         cobalt_draw_text_fitted(r, COBALT_FONT_BODY, g->did, text_left, y,
                                 m->width - m->pad_edge - text_left,
                                 COBALT_COLOUR_TEXT_DIM);
         if (stats[0]) {
            cobalt_draw_text_fitted(r, COBALT_FONT_CAPTION, stats, text_left,
                                    y + cobalt_font_line_height(r, COBALT_FONT_BODY),
                                    m->width - m->pad_edge - text_left,
                                    COBALT_COLOUR_TEXT);
         }
      }
      y += side + m->gap;

      if (g->description[0]) {
         y += cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, g->description,
                                       left, y, width, 4, COBALT_COLOUR_TEXT) +
              m->gap / 2;
      }
   } else if (busy) {
      cobalt_draw_empty_state(r, top, bottom, "Loading feed...", "");
   } else {
      cobalt_draw_empty_state(r, top, bottom, "Feed unavailable",
                              "It may have been deleted.");
   }

   (void) y;
   char controls[160];
   const int saved_state = cobalt_feed_saved_state(view->open_uri);
   if (view->saving || busy) {
      snprintf(controls, sizeof(controls), "Working...");
   } else if (view->remove_confirm == 1) {
      snprintf(controls, sizeof(controls), "X: confirm remove   B: cancel");
   } else {
      snprintf(controls, sizeof(controls),
               "A: open   %sR: %s   X: remove   B: back",
               saved_state >= 0 ? "Y: saved   " : "Y: save   ",
               saved_state == 1 ? "unpin" : "pin");
   }
   draw_hint_line(view, r, controls);
}

void
cobalt_feed_discovery_view_draw(cobalt_feed_discovery_view *view,
                                cobalt_render *r,
                                cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }
   if (view->browsing_detail) {
      draw_detail(view, r, surface);
   } else {
      draw_browse(view, r, surface);
   }
}
