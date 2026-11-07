#include "app/profile.h"
#include "atproto/session.h"
#include "ui/postcard.h"
#include "ui/theme.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#include <stdio.h>
#include <string.h>

#define TEXT_LINES 2

/* Row 0 is the header card; posts start at 1. */
#define HEADER_ROW 0

/* Tab pills recorded by the last GamePad draw, and only on the GamePad: the TV
 * has no touch. Indexed by the tab enum. */
static SDL_Rect s_tab_hit[WF_PROFILE_TAB_COUNT];
static bool s_tab_hit_valid;

void
cobalt_profile_view_init(cobalt_profile_view *view)
{
   if (!view) {
      return;
   }
   memset(view, 0, sizeof(*view));
   cobalt_listnav_init(&view->nav);
}

void
cobalt_profile_view_rewind(cobalt_profile_view *view)
{
   if (!view) {
      return;
   }
   cobalt_listnav_rewind(&view->nav);
}

int
cobalt_profile_tab_step(int current, bool is_self, int delta)
{
   const int count = is_self ? WF_PROFILE_TAB_COUNT
                             : WF_PROFILE_TAB_COUNT - 1;   /* no Likes */
   if (count <= 0) {
      return WF_PROFILE_TAB_POSTS;
   }
   if (current < 0 || current >= count) {
      current = WF_PROFILE_TAB_POSTS;
   }
   int next = (current + delta) % count;
   if (next < 0) {
      next += count;
   }
   return next;
}

const cobalt_post *
cobalt_profile_view_selected_post(const cobalt_profile_view *view)
{
   if (!view || view->nav.selected <= HEADER_ROW) {
      return NULL;
   }
   const cobalt_feed *feed = cobalt_session_author_feed();
   const int index = view->nav.selected - 1;
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

   /* A refresh can return fewer posts than were on screen; the clamp inside
    * keeps the cursor in range. The header always exists, so `rows` is at
    * least one and the cursor never clamps to -1. */
   cobalt_listnav_move(&view->nav, in, rows);
   cobalt_listnav_follow(&view->nav);

   if (busy) {
      return COBALT_PROFILE_VIEW_STAY;
   }

   /* Shoulders cycle the visible tabs, so changing one does not require the
    * More menu. The strip is the primary control; this is the same action on
    * a controller, where there is no pointer. */
   if (cobalt_input_pressed(in, COBALT_BTN_SHOULDER_L) ||
       cobalt_input_pressed(in, COBALT_BTN_SHOULDER_R)) {
      const int delta = cobalt_input_pressed(in, COBALT_BTN_SHOULDER_R) ? 1 : -1;
      const int next = cobalt_profile_tab_step(cobalt_session_profile_tab(),
                                               profile->is_self, delta);
      if (next != cobalt_session_profile_tab() &&
          cobalt_session_begin_profile_tab(next)) {
         cobalt_profile_view_rewind(view);
      }
      return COBALT_PROFILE_VIEW_STAY;
   }

   /* Touch: the strip's rects come from the last GamePad draw. Likes is not
    * drawn on someone else's profile, and its slot stays empty there. */
   if (s_tab_hit_valid && in->touch_ended) {
      for (int tab = 0; tab < WF_PROFILE_TAB_COUNT; tab++) {
         if (tab == WF_PROFILE_TAB_LIKES && !profile->is_self) {
            continue;
         }
         if (cobalt_input_tapped(in, &s_tab_hit[tab])) {
            if (tab != cobalt_session_profile_tab() &&
                cobalt_session_begin_profile_tab(tab)) {
               cobalt_profile_view_rewind(view);
            }
            return COBALT_PROFILE_VIEW_STAY;
         }
      }
   }

   /* Followers, following and, on a post, the post's own actions live in the
    * More menu, the same Y as the timeline and thread (cobalt#109). */
   if (profile->loaded && cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      return COBALT_PROFILE_VIEW_MENU;
   }

   if (view->nav.selected == HEADER_ROW) {
      /* Same three-button language a post row already uses (A / Left /
       * Right), just mapped to follow/mute/block instead of open/like/repost.
       * None apply to your own profile, and the session layer refuses them
       * there too. */
      if (profile->loaded && !profile->is_self) {
         if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
            cobalt_session_begin_follow();
         } else if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
            cobalt_session_begin_mute();
         } else if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
            cobalt_session_begin_block();
         }
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

/*
 * Split play: the pad's copy of a profile is the handle, the counts and then
 * the posts. The bio, the big avatar and the follow button are all reading —
 * the TV has them, at a size they can be read at — and on a 480-line screen
 * they cost the posts the room they need to be a list. The follow action is
 * still on the footer, where every other action on this screen is.
 */
static bool
compact_header(cobalt_render *r)
{
   return cobalt_render_remote(r);
}

static int
header_avatar_side(cobalt_render *r, const cobalt_metrics *m)
{
   if (!compact_header(r)) {
      return PROFILE_AVATAR_SIDE(m);
   }
   /* Exactly the two lines beside it, so a remote row is as tall as its
    * words and no taller. */
   return cobalt_font_line_height(r, COBALT_FONT_HEADING) +
          cobalt_font_line_height(r, COBALT_FONT_CAPTION);
}

/*
 * A wide (TV) header puts the bio and the counts beside the avatar and the
 * follow button at the right edge, instead of stacking everything below the
 * name. On a 16:9 screen the stacked card was 266px tall and left the right
 * half empty — tall enough that not one post fitted under it. The same words
 * beside the avatar are a 150px card and the first post comes back.
 *
 * Shared by header_height and draw_header so the reserved box and the drawing
 * cannot disagree.
 */
typedef struct {
   int avatar;
   int text_left;
   int text_width;
   int text_h;
   int bio_lines;
   int button_w;
} wide_header;

static wide_header
wide_header_measure(cobalt_render *r, const cobalt_profile *profile,
                    const cobalt_metrics *m)
{
   wide_header w = { 0 };
   const int card_w = m->width - 2 * m->pad_edge;
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   w.avatar = header_avatar_side(r, m);
   w.text_left = m->pad_edge + m->pad_tile + w.avatar + m->pad_tile / 2;
   int right = m->pad_edge + card_w - m->pad_tile;

   if (!profile->is_self) {
      const char *label = profile->viewer_following[0] ? "Following" : "Follow";
      int label_w = 0;
      cobalt_text_size(r, COBALT_FONT_BODY, label, &label_w, NULL);
      w.button_w = label_w + 2 * m->pad_tile;
      if (w.button_w > card_w / 3) {
         w.button_w = card_w / 3;
      }
      right -= w.button_w + m->gap;
   }

   w.text_width = right - w.text_left;
   if (w.text_width < 0) {
      w.text_width = 0;
   }

   /* Height follows the bio's actual wrap, up to the two lines the design
    * allows; a one-line bio must not reserve a second line it never draws. */
   if (profile->description[0] && w.text_width > 0) {
      w.bio_lines = cobalt_text_wrapped_lines(r, COBALT_FONT_CAPTION,
                                              profile->description,
                                              w.text_width);
      if (w.bio_lines > 2) {
         w.bio_lines = 2;
      }
   }

   w.text_h = heading_h + w.bio_lines * (caption_h + m->line_gap) + caption_h;
   return w;
}

/* The counts, with mute/block appended on someone else's profile: a remote
 * has no separate status line, so Left and Right must change something the
 * person holding it can read. */
static void
format_counts_line(const cobalt_profile *profile, char *out, size_t size)
{
   snprintf(out, size, "%s \xC2\xB7 %s", profile->counts, profile->post_count);
   if (profile->is_self) {
      return;
   }
   const bool muted = profile->viewer_muted;
   const bool blocking = profile->viewer_blocking[0] != '\0';
   if (!muted && !blocking) {
      return;
   }
   const size_t used = strlen(out);
   if (used + 24 < size) {
      snprintf(out + used, size - used, " \xC2\xB7 %s%s%s",
               muted ? "Muted" : "", (muted && blocking) ? " / " : "",
               blocking ? "Blocked" : "");
   }
}

static int
header_height(cobalt_render *r, const cobalt_profile *profile,
              const cobalt_metrics *m)
{
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   if (compact_header(r)) {
      /* Handle, name and counts; the bio and big avatar stay on the TV. */
      return 2 * m->pad_tile + heading_h + 2 * caption_h;
   }

   const wide_header w = wide_header_measure(r, profile, m);
   const int inner = w.text_h > w.avatar ? w.text_h : w.avatar;
   return 2 * m->pad_tile + inner;
}

static void
draw_header(cobalt_render *r, const cobalt_profile *profile,
            const SDL_Rect *rect, bool focused, const cobalt_metrics *m)
{
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   const int left = rect->x + m->pad_tile;

   if (!compact_header(r)) {
      const wide_header w = wide_header_measure(r, profile, m);
      const int inner = w.text_h > w.avatar ? w.text_h : w.avatar;
      const int avatar_y = rect->y + (rect->h - w.avatar) / 2;
      cobalt_avatar_draw(r, profile->avatar, profile->display_name,
                         profile->handle, left, avatar_y, w.avatar);

      int wy = rect->y + m->pad_tile + (inner - w.text_h) / 2;
      int handle_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, profile->handle, &handle_w, NULL);
      /* The handle follows the name on the same band. Fit the name to the room
       * the handle leaves, but never give the handle more than half the band:
       * a long handle should drop, not erase the person's name. */
      int name_room = w.text_width;
      if (profile->handle[0] && handle_w + m->pad_tile < w.text_width / 2) {
         name_room = w.text_width - handle_w - m->pad_tile;
      }
      const int name_w = cobalt_draw_text_fitted(r, COBALT_FONT_HEADING,
                                                 profile->display_name, w.text_left,
                                                 wy, name_room, COBALT_COLOUR_TEXT);
      const int handle_x = w.text_left + name_w + m->pad_tile;
      if (profile->handle[0] &&
          handle_x + handle_w <= w.text_left + w.text_width) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, profile->handle, handle_x,
                          wy + heading_h - caption_h,
                          COBALT_COLOUR_TEXT_DIM);
      }
      wy += heading_h;

      if (w.bio_lines > 0) {
         cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION,
                                  profile->description, w.text_left, wy,
                                  w.text_width, w.bio_lines,
                                  COBALT_COLOUR_TEXT);
         wy += w.bio_lines * (caption_h + m->line_gap);
      }

      char line[COBALT_POST_META_MAX + 48];
      format_counts_line(profile, line, sizeof(line));
      cobalt_draw_text(r, COBALT_FONT_CAPTION, line, w.text_left, wy,
                       COBALT_COLOUR_TEXT_DIM);

      /* Follow sits at the right edge, centred on the card: the far end of a
       * wide header is otherwise empty, and reaching it is a pointer action,
       * not a list row. */
      if (!profile->is_self) {
         const bool following = profile->viewer_following[0] != '\0';
         SDL_Rect button = { rect->x + rect->w - m->pad_tile - w.button_w,
                             rect->y + (rect->h - m->font_body * 2) / 2,
                             w.button_w, m->font_body * 2 };
         cobalt_draw_tile(r, &button, focused ? 1.0f : 0.0f);
         const int label_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
         cobalt_draw_text_centred(r, COBALT_FONT_BODY,
                                  following ? "Following" : "Follow",
                                  button.x,
                                  button.y + (button.h - label_h) / 2, button.w,
                                  following ? COBALT_COLOUR_TEXT_DIM
                                            : COBALT_COLOUR_ACCENT_TEXT);
      }
      return;
   }

   int y = rect->y + m->pad_tile;

   const int avatar = header_avatar_side(r, m);
   cobalt_avatar_draw(r, profile->avatar, profile->display_name,
                      profile->handle, left, y, avatar);

   /* Name and handle beside the avatar; everything after clears it. Both stop
    * at the card's right edge, which on this layout is the only thing keeping
    * a long name from running under the Follow control. */
   const int name_left = left + avatar + m->pad_tile / 2;
   const int name_room = (rect->x + rect->w - m->pad_tile) - name_left;
   cobalt_draw_text_fitted(r, COBALT_FONT_HEADING, profile->display_name,
                           name_left, y, name_room, COBALT_COLOUR_TEXT);
   cobalt_draw_text_fitted(r, COBALT_FONT_CAPTION, profile->handle, name_left,
                           y + heading_h, name_room, COBALT_COLOUR_TEXT_DIM);

   const int beside = heading_h + caption_h;
   y += (avatar > beside) ? avatar : beside;

   char line[COBALT_POST_META_MAX + 48];
   format_counts_line(profile, line, sizeof(line));
   cobalt_draw_text(r, COBALT_FONT_CAPTION, line, left, y, COBALT_COLOUR_TEXT_DIM);
}

/* The profile's tabs, as the app's pill strip. The active tab is marked with
 * an accent underline rather than colour alone: the position survives a
 * colour-blind eye and the high-contrast palette, and it keeps every label at
 * full text contrast. Returns the strip's height. */
static int
draw_tab_strip(cobalt_render *r, const cobalt_metrics *m, int y, bool touchable)
{
   const int pill_h = cobalt_pill_height(r);
   const int gap = m->gap / 2;
   const int active = cobalt_session_profile_tab();
   const bool is_self = cobalt_session_profile()->is_self;
   int x = m->pad_edge;

   if (touchable) {
      memset(s_tab_hit, 0, sizeof(s_tab_hit));
   }

   for (int tab = 0; tab < WF_PROFILE_TAB_COUNT; tab++) {
      if (tab == WF_PROFILE_TAB_LIKES && !is_self) {
         continue;
      }
      const char *name = wf_profile_tab_name(tab);
      const int w = cobalt_pill_width(r, NULL, name);
      cobalt_draw_pill(r, NULL, name, x, y);
      if (tab == active) {
         SDL_Rect line = { x + pill_h / 4, y + pill_h - 3,
                           w - pill_h / 2, 3 };
         if (line.w > 0) {
            cobalt_fill_rect(r, &line, COBALT_COLOUR_ACCENT);
         }
      }
      if (touchable) {
         s_tab_hit[tab] = (SDL_Rect){ x, y, w, pill_h };
      }
      x += w + gap;
   }

   if (touchable) {
      s_tab_hit_valid = true;
   }
   return pill_h;
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

   {
      /* The handle identifies the screen; the active tab has its own visible
       * strip below the title. */
      char subtitle[96];
      if (cobalt_session_busy()) {
         snprintf(subtitle, sizeof(subtitle), "Loading...");
      } else {
         snprintf(subtitle, sizeof(subtitle), "%s", profile->handle);
      }
      cobalt_draw_page_title(r, "Profile", subtitle);
   }

   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   /* The tabs are pinned above the scrolling header, as on the other clients:
    * they are how the screen is steered, so they must not scroll away with
    * the card they filter. */
   const int tabs_h = draw_tab_strip(r, m, top, touchable);
   const int list_top = top + tabs_h + m->gap / 2;

   cobalt_listnav_draw_begin(&view->nav, touchable);

   if (!profile->loaded) {
      const bool busy = cobalt_session_busy();
      cobalt_draw_empty_state(r, list_top, bottom,
                              busy ? "Loading the profile..."
                                   : "This profile is unavailable",
                              "");
      cobalt_listnav_draw_empty(&view->nav, touchable);
      return;
   }

   const int rows = feed->count + 1;
   int y = list_top;
   int last_fitted = view->nav.scroll;

   for (int i = view->nav.scroll; i < rows; i++) {
      const bool is_header = (i == HEADER_ROW);
      const int h = is_header ? header_height(r, profile, m)
                              : cobalt_postcard_height(r, &feed->posts[i - 1],
                                                       TEXT_LINES,
                                                       i == view->nav.selected);

      if (y + h > bottom && i > view->nav.scroll) {
         break;
      }

      SDL_Rect rect = { m->pad_edge, y, m->width - 2 * m->pad_edge, h };
      if (is_header) {
         draw_header(r, profile, &rect, i == view->nav.selected, m);
      } else {
         cobalt_postcard_draw(r, &feed->posts[i - 1], &rect,
                              i == view->nav.selected, TEXT_LINES, 0);
      }

      cobalt_listnav_draw_add(&view->nav, touchable, &rect, i);

      last_fitted = i;
      y += h + m->gap;
   }

   cobalt_listnav_draw_end(&view->nav, touchable, last_fitted);

   /* An account with no posts is normal; say so rather than leaving a gap. */
   if (feed->count == 0 && !cobalt_session_busy()) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, "Nothing to show here.", m->pad_edge,
                       y + m->gap, COBALT_COLOUR_TEXT_DIM);
   }

   /* Name what the buttons do on the row that is actually selected: they act
    * on the account on the header row and on the post on any other, and on
    * your own profile the header row has nothing for A, Left or Right (the
    * session layer refuses them for is_self) — so promising those would be
    * false. */
   char hint_text[160];
   if (view->nav.selected == HEADER_ROW) {
      if (profile->is_self) {
         snprintf(hint_text, sizeof(hint_text), "Y: more   L/R: tab");
      } else {
         snprintf(hint_text, sizeof(hint_text),
                  "A: %s   Left: mute   Right: block   Y: more   L/R: tab",
                  profile->viewer_following[0] ? "unfollow" : "follow");
      }
   } else {
      snprintf(hint_text, sizeof(hint_text),
               "A: thread   Left: like   Right: repost   Y: more   L/R: tab");
   }
   cobalt_draw_hints(r, cobalt_session_busy() ? "Working..." : hint_text);
}

/* --- editing your own profile --- */

void
cobalt_profile_edit_open(cobalt_profile_edit *edit, const cobalt_profile *profile)
{
   if (!edit) {
      return;
   }
   memset(edit, 0, sizeof(*edit));
   edit->field = 0;
   if (profile) {
      snprintf(edit->display_name, sizeof(edit->display_name), "%s",
               profile->display_name);
      snprintf(edit->description, sizeof(edit->description), "%s",
               profile->description);
   }
}

bool
cobalt_profile_edit_valid(const char *display_name, const char *description)
{
   if (!display_name || !description) {
      return false;
   }
   return strlen(display_name) <= COBALT_PROFILE_NAME_MAX &&
          strlen(description) < COBALT_PROFILE_BIO_MAX;
}

cobalt_profile_edit_action
cobalt_profile_edit_update(cobalt_profile_edit *edit, const cobalt_input *in)
{
   if (!edit || !in) {
      return COBALT_PROFILE_EDIT_STAY;
   }

   if (edit->editing) {
      const cobalt_kb_result r = cobalt_keyboard_update(&edit->kb, in);
      if (r != COBALT_KB_IDLE) {
         edit->editing = false;
      }
      return COBALT_PROFILE_EDIT_STAY;
   }

   /* A save owns the screen until its result arrives, so a failure can put
    * the reason next to the text it is about. */
   if (edit->saving) {
      return COBALT_PROFILE_EDIT_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_PROFILE_EDIT_BACK;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) ||
       cobalt_input_pressed(in, COBALT_BTN_UP)) {
      edit->field = edit->field == 0 ? 1 : 0;
      return COBALT_PROFILE_EDIT_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      edit->editing = true;
      if (edit->field == 0) {
         cobalt_keyboard_open(&edit->kb, edit->display_name,
                              sizeof(edit->display_name), false, false);
      } else {
         cobalt_keyboard_open(&edit->kb, edit->description,
                              sizeof(edit->description), false, true);
      }
      return COBALT_PROFILE_EDIT_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_ALT_Y) && !edit->saving &&
       cobalt_profile_edit_valid(edit->display_name, edit->description)) {
      return COBALT_PROFILE_EDIT_SAVE;
   }

   return COBALT_PROFILE_EDIT_STAY;
}

void
cobalt_profile_edit_draw(cobalt_profile_edit *edit, cobalt_render *r,
                         cobalt_surface_id surface)
{
   if (!edit || !r) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const int width = m->width - 2 * m->pad_edge;

   if (edit->editing) {
      /* One field at a time while the keyboard is up. The keyboard takes the
       * bottom of the panel, so the active value stays visible above it. */
      SDL_Rect field = { m->pad_edge, top, width,
                         m->pad_tile + cobalt_font_line_height(r, COBALT_FONT_CAPTION) +
                            cobalt_font_line_height(r, COBALT_FONT_BODY) };
      cobalt_draw_tile(r, &field, 1.0f);
      cobalt_draw_text(r, COBALT_FONT_CAPTION,
                       edit->field == 0 ? "Display name" : "Bio",
                       field.x + m->pad_tile, field.y + m->pad_tile,
                       COBALT_COLOUR_TEXT_DIM);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               edit->field == 0 ? edit->display_name
                                                : edit->description,
                               field.x + m->pad_tile,
                               field.y + m->pad_tile +
                                  cobalt_font_line_height(r, COBALT_FONT_CAPTION),
                               field.w - 2 * m->pad_tile, 1, COBALT_COLOUR_TEXT);

      const int kb_h = cobalt_keyboard_height(surface);
      SDL_Rect keys = { m->pad_edge, bottom - kb_h, width, kb_h };
      cobalt_keyboard_draw(&edit->kb, r, surface, &keys);
      cobalt_draw_hints(r, "OK: keep   B: cancel");
      return;
   }

   const int label_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   int y = top;
   for (int i = 0; i < 2; i++) {
      const bool bio = i == 1;
      const char *value = bio ? edit->description : edit->display_name;
      const int lines = bio ? 3 : 1;
      SDL_Rect tile = { m->pad_edge, y, width,
                        m->pad_tile + label_h + lines *
                           cobalt_font_line_height(r, COBALT_FONT_BODY) };
      cobalt_draw_tile(r, &tile, edit->field == i ? 1.0f : 0.0f);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, bio ? "Bio" : "Display name",
                       tile.x + m->pad_tile, tile.y + m->pad_tile,
                       COBALT_COLOUR_TEXT_DIM);
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, value[0] ? value : "(not set)",
                               tile.x + m->pad_tile, tile.y + m->pad_tile + label_h,
                               tile.w - 2 * m->pad_tile, lines,
                               value[0] ? COBALT_COLOUR_TEXT
                                        : COBALT_COLOUR_TEXT_DIM);
      y += tile.h + m->gap;
   }

   cobalt_draw_hints(r, edit->saving ? "Saving..."
                                     : "A: edit   Y: save   B: cancel");
}
