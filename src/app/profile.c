#include "app/profile.h"
#include "atproto/session.h"
#include "ui/postcard.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define TEXT_LINES 2

/* Row 0 is the header card; posts start at 1. */
#define HEADER_ROW 0

void
cobalt_profile_view_init(cobalt_profile_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   view->last_visible = -1;
}

void
cobalt_profile_view_rewind(cobalt_profile_view *view)
{
   if (!view) {
      return;
   }
   view->selected = 0;
   view->scroll = 0;
   view->last_visible = -1;
}

const cobalt_post *
cobalt_profile_view_selected_post(const cobalt_profile_view *view)
{
   if (!view || view->selected <= HEADER_ROW) {
      return NULL;
   }
   const cobalt_feed *feed = cobalt_session_author_feed();
   const int index = view->selected - 1;
   return (index < feed->count) ? &feed->posts[index] : NULL;
}

cobalt_profile_action
cobalt_profile_view_update(cobalt_profile_view *view, const cobalt_input *in)
{
   if (!view || !in) {
      return COBALT_PROFILE_VIEW_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_PROFILE_VIEW_BACK;
   }

   const cobalt_profile *profile = cobalt_session_profile();
   const cobalt_feed *feed = cobalt_session_author_feed();
   const bool busy = cobalt_session_busy();
   const int rows = feed->count + 1;   /* header plus posts */

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && view->selected < rows - 1) {
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

   /* A refresh can return fewer posts than were on screen, which would leave
    * the cursor past the end. The header always exists, so `rows` is at least
    * one and the cursor never clamps to -1 here. */
   cobalt_list_clamp(&view->selected, &view->scroll, rows);

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

   if (busy) {
      return COBALT_PROFILE_VIEW_STAY;
   }

   if (view->selected == HEADER_ROW) {
      /* A on the header follows or unfollows. Your own profile has no such
       * button, and the session layer refuses it there too. */
      if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM) && profile->loaded &&
          !profile->is_self) {
         cobalt_session_begin_follow();
      }
      return COBALT_PROFILE_VIEW_STAY;
   }

   const cobalt_post *post = cobalt_profile_view_selected_post(view);
   if (!post) {
      return COBALT_PROFILE_VIEW_STAY;
   }

   /* Same bindings as everywhere else a post card appears. */
   if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
      cobalt_session_begin_like(post->uri, post->cid);
   } else if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
      cobalt_session_begin_repost(post->uri, post->cid);
   } else if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      cobalt_session_begin_thread(post->uri);
      return COBALT_PROFILE_VIEW_OPEN_THREAD;
   }

   return COBALT_PROFILE_VIEW_STAY;
}

/* --- drawing --- */

/*
 * The profile avatar is drawn larger than a card's, because this screen is
 * about the account rather than about a post. Sized from the heading font for
 * the same reason the card's is sized from the body font: the two surfaces have
 * their own type scales.
 */
#define PROFILE_AVATAR_SIDE(m) ((m)->font_heading * 3)

static int
header_height(cobalt_render *r, const cobalt_profile *profile,
              const cobalt_metrics *m)
{
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   int h = m->pad_tile + heading_h + caption_h;   /* name, handle */

   /* The name and handle sit beside the avatar; anything below it clears the
    * whole column, so the header grows when the avatar is the taller of the
    * two rather than letting the circle overrun the bio. */
   const int beside = heading_h + caption_h;
   const int avatar = PROFILE_AVATAR_SIDE(m);
   if (avatar > beside) {
      h += avatar - beside;
   }

   if (profile->description[0]) {
      h += 2 * (caption_h + m->line_gap);
   }
   h += caption_h;                                /* counts */
   if (!profile->is_self) {
      h += m->font_body * 2 + m->gap;             /* follow button */
   }
   h += m->pad_tile;
   return h;
}

static void
draw_header(cobalt_render *r, const cobalt_profile *profile,
            const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   const int left = rect->x + m->pad_tile;
   const int width = rect->w - 2 * m->pad_tile;
   int y = rect->y + m->pad_tile;

   const int avatar = PROFILE_AVATAR_SIDE(m);
   cobalt_avatar_draw(r, profile->avatar, profile->display_name,
                      profile->handle, left, y, avatar);

   /* Name and handle beside the avatar; everything after clears it. */
   const int name_left = left + avatar + m->pad_tile / 2;
   cobalt_draw_text(r, COBALT_FONT_HEADING, profile->display_name, name_left, y,
                    COBALT_COLOUR_TEXT);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, profile->handle, name_left,
                    y + heading_h, COBALT_COLOUR_TEXT_DIM);

   const int beside = heading_h + caption_h;
   y += (avatar > beside) ? avatar : beside;

   if (profile->description[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, profile->description,
                               left, y, width, 2, COBALT_COLOUR_TEXT);
      y += 2 * (caption_h + m->line_gap);
   }

   char line[COBALT_POST_META_MAX + 48];
   snprintf(line, sizeof(line), "%s \xC2\xB7 %s", profile->counts,
            profile->post_count);
   cobalt_draw_text(r, COBALT_FONT_CAPTION, line, left, y, COBALT_COLOUR_TEXT_DIM);
   y += caption_h;

   if (!profile->is_self) {
      const bool following = profile->viewer_following[0] != '\0';
      SDL_Rect button = { left, y + m->gap / 2, width / 2, m->font_body * 2 };
      cobalt_draw_tile(r, &button, focused ? 1.0f : 0.0f);

      const int label_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text_centred(r, COBALT_FONT_BODY,
                               following ? "Following" : "Follow", button.x,
                               button.y + (button.h - label_h) / 2, button.w,
                               following ? COBALT_COLOUR_TEXT_DIM
                                         : COBALT_COLOUR_ACCENT);
   }
}

void
cobalt_profile_view_draw(cobalt_profile_view *view, cobalt_render *r,
                         cobalt_surface_id surface)
{
   if (!view || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const cobalt_profile *profile = cobalt_session_profile();
   const cobalt_feed *feed = cobalt_session_author_feed();
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Profile", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);
   {
      SDL_Color dim = { 0xD8, 0xE6, 0xF4, 0xFF };
      cobalt_draw_text(r, COBALT_FONT_CAPTION,
                       cobalt_session_busy() ? "Loading..." : profile->handle,
                       m->pad_edge,
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

   if (!profile->loaded) {
      SDL_Rect card = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                        m->font_body * 4 };
      cobalt_draw_tile(r, &card, 0.0f);
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_session_busy() ? "Loading the profile..."
                                             : "This profile is unavailable.",
                       card.x + m->pad_tile, card.y + m->pad_tile,
                       COBALT_COLOUR_TEXT);
      if (touchable) {
         view->last_visible = -1;
      }
      return;
   }

   const int rows = feed->count + 1;
   int y = top;
   int last_fitted = view->scroll;

   for (int i = view->scroll; i < rows; i++) {
      const bool is_header = (i == HEADER_ROW);
      const int h = is_header ? header_height(r, profile, m)
                              : cobalt_postcard_height(r, &feed->posts[i - 1],
                                                       TEXT_LINES);

      if (y + h > bottom && i > view->scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      if (is_header) {
         draw_header(r, profile, &rect, i == view->selected, m);
      } else {
         cobalt_postcard_draw(r, &feed->posts[i - 1], &rect,
                              i == view->selected, TEXT_LINES, 0);
      }

      if (touchable && view->hit_count < COBALT_FEED_MAX_POSTS + 1) {
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

   /* An account with no posts is normal; say so rather than leaving a gap. */
   if (feed->count == 0 && !cobalt_session_busy()) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, "No posts to show.", m->pad_edge,
                       y + m->gap, COBALT_COLOUR_TILE);
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy()
                       ? "Working..."
                       : "A: follow / open   Left: like   Right: repost   B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}
