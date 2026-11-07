/* The home menu and the small menu screens: the feed picker and sign in. */

#include "app/app_internal.h"

typedef enum {
   ACTION_TIMELINE = 0,
   ACTION_BOOKMARKS,
   ACTION_DRAFTS,
   ACTION_COMPOSE,
   ACTION_SEARCH,
   ACTION_FEEDS,
   ACTION_LISTS,
   ACTION_NOTIFICATIONS,
   ACTION_MESSAGES,
   ACTION_QUEUE,
   ACTION_ACCOUNT,
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
   ACTION_SETTINGS,
   ACTION_CREDITS,
   ACTION_QUIT,
} menu_action;

/*
 * Home menu order. Entries are described by functions rather than a static
 * table because two of them change with session state — the account entry is
 * "Sign in" or "Account" depending on whether there is one, and it is only
 * selectable when a sign-in could actually succeed.
 */
static const menu_action MENU[] = {
   ACTION_TIMELINE,
   ACTION_BOOKMARKS,
   ACTION_DRAFTS,
   ACTION_COMPOSE,
   ACTION_SEARCH,
   ACTION_FEEDS,
   ACTION_LISTS,
   ACTION_NOTIFICATIONS,
   ACTION_MESSAGES,
   ACTION_QUEUE,
   ACTION_ACCOUNT,
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
   ACTION_SETTINGS,
   ACTION_CREDITS,
   ACTION_QUIT,
};

_Static_assert((int) (sizeof(MENU) / sizeof(MENU[0])) == COBALT_HOME_MENU_COUNT,
               "the home menu table and COBALT_HOME_MENU_COUNT disagree");
#define MENU_COUNT COBALT_HOME_MENU_COUNT


/* Hit rectangles for the GamePad grid, recomputed on draw so touch and the
 * drawn layout can never drift apart. */
static SDL_Rect s_drc_hit[MENU_COUNT];
static bool s_drc_hit_valid = false;


/*
 * The feeds picker lists the account's saved feeds (fetched on entry). When
 * there are none, or the fetch has not landed yet, Bluesky's official
 * "What's Hot" stands in so the screen is never empty.
 */
typedef struct {
   const char *label;
   const char *uri;
} feed_entry;

static const feed_entry FALLBACK_FEED = {
   "What's Hot",
   "at://did:plc:z72i7hdynmk6r22z27h6tvur/app.bsky.feed.generator/whats-hot",
};

#define FEED_MAX COBALT_SAVED_FEEDS_MAX

static SDL_Rect s_feeds_hit[FEED_MAX];
static bool s_feeds_hit_valid = false;
/* The slice of the feed list the last GamePad draw put on screen. */
static int s_feeds_first = 0;
static int s_feeds_visible = 0;
/* Rows the GamePad (the smaller surface) fitted on its last draw; the scroll
 * window follows it so the selection is never below the fold on either. */
static int s_feeds_rows = 5;

static int
feed_count(void)
{
   const int n = cobalt_session_saved_feeds()->count;
   return n > 0 ? n : 1;
}

static feed_entry
feed_at(int i)
{
   const cobalt_saved_feeds *f = cobalt_session_saved_feeds();
   if (f->count > 0 && i >= 0 && i < f->count) {
      feed_entry e = { f->feeds[i].label, f->feeds[i].uri };
      return e;
   }
   return FALLBACK_FEED;
}

static bool
signed_in(void)
{
   return cobalt_session_state() == COBALT_AUTH_SIGNED_IN;
}

/* A right-aligned count in the footer strip ("11 of 12"), centred against the
 * prompt row beside it so the two sit on one line rather than one over the
 * other when the pills are fingertip sized. */
static int
footer_label_y(cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int pill_h = cobalt_pill_height(r);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   return m->height - m->pad_edge / 2 - pill_h + (pill_h - caption_h) / 2;
}

static const char *
menu_label(int index)
{
   switch (MENU[index]) {
      case ACTION_TIMELINE:       return "Timeline";
      case ACTION_BOOKMARKS:      return "Bookmarks";
      case ACTION_DRAFTS:         return "Drafts";
      case ACTION_COMPOSE:        return "New post";
      case ACTION_SEARCH:         return "Search";
      case ACTION_FEEDS:          return "Feeds";
      case ACTION_LISTS:          return "Lists";
      case ACTION_NOTIFICATIONS:  return "Notifications";
      case ACTION_MESSAGES:       return "Messages";
      case ACTION_QUEUE:          return "Queued posts";
      case ACTION_ACCOUNT:        return signed_in() ? "Account" : "Sign in";
      case ACTION_DIAGNOSTICS:    return "Diagnostics";
      case ACTION_TOGGLE_DISPLAY: return "TV display";
      case ACTION_SETTINGS:       return "Settings";
      case ACTION_CREDITS:        return "Credits";
      case ACTION_QUIT:           return "Quit";
      default:                    return "";
   }
}

static const char *
menu_hint(int index)
{
   switch (MENU[index]) {
      case ACTION_TIMELINE:
         return signed_in() ? "Your Bluesky home feed"
                            : "Sign in to read your feed";
      case ACTION_BOOKMARKS:
         return signed_in() ? "Posts you saved for later"
                            : "Sign in to see your bookmarks";
      case ACTION_DRAFTS:
         return "Posts you saved to finish later";
      case ACTION_COMPOSE:
         return signed_in() ? "Write something" : "Sign in to post";
      case ACTION_SEARCH:
         return signed_in() ? "Find accounts" : "Sign in to search";
      case ACTION_FEEDS:
         return signed_in() ? "Browse custom feeds" : "Sign in to browse feeds";
      case ACTION_LISTS:
         return signed_in() ? "Your curated lists" : "Sign in to see your lists";
      case ACTION_NOTIFICATIONS:
         return signed_in() ? "Replies, likes and follows"
                            : "Sign in to see notifications";
      case ACTION_MESSAGES:
         return signed_in() ? "Your direct message conversations"
                            : "Sign in to see messages";
      case ACTION_QUEUE:
         return "Posts saved for later, to send or discard";
      case ACTION_ACCOUNT:
         if (signed_in()) {
            return cobalt_session_handle();
         }
         return cobalt_session_available() ? "Connect with an app password"
                                           : "Unavailable — see Diagnostics";
      case ACTION_DIAGNOSTICS:
         return "Paths, network and library status";
      case ACTION_TOGGLE_DISPLAY:
         return "Switch between TV+GamePad and Off-TV";
      case ACTION_SETTINGS:
         return "Theme, post text size and contrast";
      case ACTION_CREDITS:
         return "The repositories behind the stack";
      case ACTION_QUIT:
         return "Return to the Wii U Menu";
      default:
         return "";
   }
}

static bool
menu_enabled(int index)
{
   switch (MENU[index]) {
      case ACTION_TIMELINE:
      case ACTION_BOOKMARKS:
      case ACTION_COMPOSE:
      case ACTION_SEARCH:
      case ACTION_FEEDS:
      case ACTION_LISTS:
      case ACTION_NOTIFICATIONS:
      case ACTION_MESSAGES:
         return signed_in();
      case ACTION_ACCOUNT:
         return cobalt_session_available() || signed_in();
      default:
         return true;
   }
}

static void
activate(cobalt_app *app, int index)
{
   if (index < 0 || index >= MENU_COUNT || !menu_enabled(index)) {
      COBALT_LOGD("menu: entry %d is not selectable", index);
      return;
   }

   switch (MENU[index]) {
      case ACTION_TIMELINE:
         app->screen = COBALT_SCREEN_TIMELINE;
         /* Only fetch if there is nothing to show. Re-entering the screen
          * should not throw away a scroll position the user was partway
          * through; refresh is on + and is deliberately explicit. */
         if (cobalt_session_feed()->count == 0 || app->viewing_custom_feed ||
             app->viewing_search) {
            cobalt_timeline_rewind(&app->timeline);
            cobalt_session_begin_timeline(false);
            app->viewing_custom_feed = false;
            app->viewing_search = false;
         }
         COBALT_LOGI("menu: opened timeline");
         break;

      case ACTION_BOOKMARKS:
         cobalt_timeline_rewind(&app->bookmarks);
         cobalt_session_begin_bookmarks(false);
         app->screen = COBALT_SCREEN_BOOKMARKS;
         COBALT_LOGI("menu: opened bookmarks");
         break;

      case ACTION_DRAFTS:
         cobalt_draft_view_open(&app->draft_view);
         app->screen = COBALT_SCREEN_DRAFTS;
         COBALT_LOGI("menu: opened drafts");
         break;

      case ACTION_COMPOSE:
         cobalt_compose_init(&app->compose);
         app->draft_origin = -1;
         app->compose_return = COBALT_SCREEN_HOME;
         app->screen = COBALT_SCREEN_COMPOSE;
         COBALT_LOGI("menu: composing a new post");
         break;

      case ACTION_SEARCH:
         cobalt_search_view_open(&app->search);
         app->screen = COBALT_SCREEN_SEARCH;
         COBALT_LOGI("menu: opened search");
         break;

      case ACTION_FEEDS:
         app->feeds_selected = 0;
         app->feeds_scroll = 0;
         if (signed_in()) {
            cobalt_session_begin_saved_feeds();
         }
         app->screen = COBALT_SCREEN_FEEDS;
         COBALT_LOGI("menu: opened feeds");
         break;

      case ACTION_LISTS:
         cobalt_lists_view_open(&app->lists);
         app->screen = COBALT_SCREEN_LISTS;
         COBALT_LOGI("menu: opened lists");
         break;

      case ACTION_NOTIFICATIONS:
         app->screen = COBALT_SCREEN_NOTIFICATIONS;
         if (cobalt_session_notifications()->count == 0) {
            cobalt_session_begin_notifications(false);
         }
         COBALT_LOGI("menu: opened notifications");
         break;

      case ACTION_MESSAGES:
         app->screen = COBALT_SCREEN_MESSAGES;
         if (cobalt_session_chat_inbox()->count == 0) {
            cobalt_session_begin_chat_inbox(false);
         }
         COBALT_LOGI("menu: opened messages");
         break;

      case ACTION_QUEUE:
         cobalt_queue_view_open(&app->queue_view);
         app->screen = COBALT_SCREEN_QUEUE;
         COBALT_LOGI("menu: opened queued posts");
         break;

      case ACTION_ACCOUNT:
         if (signed_in()) {
            app->screen = COBALT_SCREEN_ACCOUNT;
         } else {
            cobalt_signin_set_status(&app->signin, "", false);
            app->screen = COBALT_SCREEN_SIGN_IN;
         }
         break;

      case ACTION_DIAGNOSTICS:
         app->screen = COBALT_SCREEN_DIAGNOSTICS;
         /* Refresh here rather than per frame: AC queries are not free. */
         cobalt_net_refresh();
         COBALT_LOGI("menu: opened diagnostics");
         break;

      case ACTION_TOGGLE_DISPLAY:
         app->display = (app->display == COBALT_DISPLAY_DUAL) ? COBALT_DISPLAY_GAMEPAD
                                                              : COBALT_DISPLAY_DUAL;
         COBALT_LOGI("menu: display mode -> %s",
                     app->display == COBALT_DISPLAY_DUAL ? "TV + GamePad" : "GamePad only");
         break;

      case ACTION_SETTINGS:
         app->settings_selected = 0;
         app->screen = COBALT_SCREEN_SETTINGS;
         COBALT_LOGI("menu: opened settings");
         break;

      case ACTION_CREDITS:
         app->screen = COBALT_SCREEN_CREDITS;
         COBALT_LOGI("menu: opened credits");
         break;

      case ACTION_QUIT:
         COBALT_LOGI("menu: quit requested");
         app->quit = true;
         break;

      default:
         break;
   }
}

void
cobalt_app_update_home(cobalt_app *app, const cobalt_input *in)
{
   /* Both surfaces draw the same four-column grid, so up and down move a whole
    * row and left and right move within it. Deriving the step from the layout
    * rather than the surface means the TV moves by the row the person can see,
    * and a menu started on one screen keeps its spatial order on the other. */
   const int vstep = COBALT_HOME_COLS;

   if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
      app->selected = (app->selected + 1) % MENU_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
      app->selected = (app->selected + MENU_COUNT - 1) % MENU_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      app->selected = (app->selected + vstep) % MENU_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      app->selected = (app->selected + MENU_COUNT - vstep) % MENU_COUNT;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      activate(app, app->selected);
   }

   /* Touch: the GamePad list is the only touchable surface. */
   if (s_drc_hit_valid && in->touch_ended) {
      for (int i = 0; i < MENU_COUNT; i++) {
         if (cobalt_input_tapped(in, &s_drc_hit[i])) {
            app->selected = i;
            activate(app, i);
            break;
         }
      }
   }
}

void
cobalt_app_update_feeds(cobalt_app *app, const cobalt_input *in)
{
   if (cobalt_session_busy()) {
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      app->screen = COBALT_SCREEN_HOME;
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      app->feeds_selected = (app->feeds_selected + 1) % feed_count();
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      app->feeds_selected = (app->feeds_selected + feed_count() - 1) % feed_count();
   }

   /* Dragging moves the selection down a row per row of finger travel, the same
    * as the D-pad; the window below then follows it. */
   if (s_feeds_hit_valid && s_feeds_visible > 0) {
      cobalt_input_drag_list(in, &app->feeds_selected, feed_count(),
                              &s_feeds_hit[s_feeds_first], s_feeds_visible);
   }

   {
      const int rows = s_feeds_rows > 0 ? s_feeds_rows : 1;
      if (app->feeds_selected < app->feeds_scroll) {
         app->feeds_scroll = app->feeds_selected;
      } else if (app->feeds_selected >= app->feeds_scroll + rows) {
         app->feeds_scroll = app->feeds_selected - rows + 1;
      }
      if (app->feeds_scroll < 0) {
         app->feeds_scroll = 0;
      }
   }

   int activated = -1;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      activated = app->feeds_selected;
   } else if (s_feeds_hit_valid && in->touch_ended) {
      for (int i = app->feeds_scroll; i < feed_count() && i < app->feeds_scroll + s_feeds_rows; i++) {
         if (cobalt_input_tapped(in, &s_feeds_hit[i])) {
            app->feeds_selected = i;
            activated = i;
            break;
         }
      }
   }

   if (activated >= 0 && activated < feed_count()) {
      const feed_entry e = feed_at(activated);
      COBALT_LOGI("feeds: opening %s", e.label);
      cobalt_timeline_rewind(&app->timeline);
      cobalt_session_begin_feed(e.uri, false);
      app->viewing_custom_feed = true;
      app->feed_back = COBALT_SCREEN_FEEDS;
      app->viewing_search = false;
      app->screen = COBALT_SCREEN_TIMELINE;
   }
}

void
cobalt_app_update_signin(cobalt_app *app, const cobalt_input *in)
{
   switch (cobalt_signin_update(&app->signin, in)) {
      case COBALT_SIGNIN_BACK:
         cobalt_signin_clear_password(&app->signin);
         app->screen = COBALT_SCREEN_HOME;
         break;

      case COBALT_SIGNIN_SUBMIT:
         if (app->signin.password[0]) {
            if (!cobalt_session_begin_login(app->signin.service,
                                            app->signin.identifier,
                                            app->signin.password)) {
               cobalt_signin_set_status(&app->signin,
                                        "Could not start the sign-in request.", true);
            } else {
               cobalt_signin_set_status(&app->signin, "", false);
            }
         } else {
            if (!cobalt_session_begin_oauth(app->signin.service,
                                            app->signin.identifier)) {
               cobalt_signin_set_status(
                  &app->signin,
                  "Could not start browser sign-in. Enter the OAuth node URL in the first field.",
                  true);
            } else {
               cobalt_signin_set_status(&app->signin, "", false);
            }
         }
         break;

      case COBALT_SIGNIN_STAY:
      default:
         break;
   }
}

void
cobalt_app_draw_home_tv(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_app_draw_header(r, "Bluesky for Wii U");

   /* Four columns of large tiles, Wii U menu style. Six columns plus two rows
    * fitted the height, but each tile was narrower than the longer labels:
    * "Notifications" and "Queued posts" had to drop to the caption face, so
    * one row of the grid read at a different size from the other. The extra
    * row buys tiles wide enough to set every label in the body face. */
   const int cols = COBALT_HOME_COLS;
   const int rows = (MENU_COUNT + cols - 1) / cols;
   const int top = cobalt_content_top(r);
   const int tile_w = (m->width - 2 * m->pad_edge - m->gap * (cols - 1)) / cols;
   /* 14 entries need a fourth row. Keep the tiles inside the content area by
    * shrinking them to the room that is actually left, rather than letting the
    * detail strip and footer run off the screen. Three rows still get the full
    * 110, so the signed-out menu looks exactly as it did. */
   int tile_h = 110;
   {
      const int avail = cobalt_content_bottom(r) - top;
      /* The focused tile's hint, then a notice that may wrap to two lines.
       * The notice is only reserved when there is one: reserving it always
       * left a dead band between the strip and the footer on every screen
       * that had nothing to report — which is most of them. */
      const int notice_room =
         (app->notice[0] || cobalt_session_busy())
            ? 2 * cobalt_font_line_height(r, COBALT_FONT_CAPTION)
            : 0;
      const int detail_room = m->font_body + 3 * m->gap + notice_room;
      const int fit = (avail - detail_room - (rows - 1) * m->gap) / rows;
      if (fit < 70) tile_h = 70;
      else if (fit < tile_h) tile_h = fit;
   }
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   for (int i = 0; i < MENU_COUNT; i++) {
      const int col = i % cols;
      const int row = i / cols;
      SDL_Rect tile = { m->pad_edge + col * (tile_w + m->gap),
                        top + row * (tile_h + m->gap), tile_w, tile_h };
      cobalt_draw_tile(r, &tile, app->focus[i]);

      const bool enabled = menu_enabled(i);
      SDL_Color label = enabled ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;

      /* The label is centred in the tile so a row of them reads as one band.
       * A disabled tile is dimmed but says nothing itself: the note inside
       * every such tile would repeat "Sign in first" seven times across the
       * grid, and the strip under it already says why the focused tile is
       * unavailable in words. */
      const int label_max = tile.w - 2 * m->pad_tile;
      int label_w = 0;
      cobalt_text_size(r, COBALT_FONT_BODY, menu_label(i), &label_w, NULL);
      const cobalt_font_id label_font =
         (label_w > label_max) ? COBALT_FONT_CAPTION : COBALT_FONT_BODY;
      const int label_line = (label_font == COBALT_FONT_BODY)
                                ? label_h
                                : cobalt_font_line_height(r, COBALT_FONT_CAPTION);
      cobalt_draw_text_wrapped(r, label_font, menu_label(i), tile.x + m->pad_tile,
                               tile.y + (tile.h - label_line) / 2, label_max, 1,
                               label);
   }

   /* Detail strip for the focused tile: the TV has room, so use it rather
    * than cramming the hint into the tile. */
   const int detail_y = top + rows * tile_h + (rows - 1) * m->gap + m->gap * 2;
   cobalt_draw_text(r, COBALT_FONT_BODY, menu_hint(app->selected),
                    m->pad_edge, detail_y, COBALT_COLOUR_TEXT);

   cobalt_app_draw_notice(app, r, detail_y + m->font_body + m->gap, m->width - 2 * m->pad_edge);

   cobalt_draw_hints(r, "A / touch: select     B: back     D-pad or stick: move");
}

void
cobalt_app_draw_home_drc(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   /* What the pad is doing, which depends on the mode it is in: the TV's remote
    * while both are on, the whole app with the TV off. The footer names the
    * mode; this line names the job. */
   cobalt_app_draw_header(r, cobalt_render_remote(r) ? "Remote for the TV"
                                                     : "Off-TV play");

   /* Four columns, four rows: the same grid as the TV, so an entry sits in the
    * same place whichever screen is in hand, and all sixteen still fit without
    * scrolling. The Wii U Menu's own pad grid is icon rows like this; at
    * 854x480 four columns leave each tile a 64 px finger target instead of the
    * forty-pixel strips that eight rows needed. */
   const int top = cobalt_content_top(r);
   const int bottom = cobalt_content_bottom(r);
   const int list_w = m->width - 2 * m->pad_edge;
   const int cols = COBALT_HOME_COLS;
   const int rows = (MENU_COUNT + cols - 1) / cols;
   const int col_gap = m->gap;
   const int row_gap = m->gap / 2;
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int hint_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   /* The focused entry's hint sits under the grid as it does on the TV, and a
    * notice takes the next line when there is one. Both are reserved before
    * the rows are sized, so neither is ever drawn over the footer. */
   const int notice_reserve =
      (app->notice[0] || cobalt_session_busy())
         ? 2 * caption_h + row_gap
         : 0;
   const int detail_reserve = hint_h + m->gap + notice_reserve;
   int row_h = (bottom - top - detail_reserve - (rows - 1) * row_gap) / rows;
   if (row_h > 64) row_h = 64;   /* roomy, but still a grid, not a card stack */
   if (row_h < 44) row_h = 44;   /* the panel's fingertip minimum */

   const int col_w = (list_w - (cols - 1) * col_gap) / cols;
   const int text_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   for (int i = 0; i < MENU_COUNT; i++) {
      const int col = i % cols;
      const int row = i / cols;
      SDL_Rect tile = { m->pad_edge + col * (col_w + col_gap),
                        top + row * (row_h + row_gap), col_w, row_h };
      s_drc_hit[i] = tile;

      cobalt_draw_tile(r, &tile, app->focus[i]);
      cobalt_draw_text(r, COBALT_FONT_BODY, menu_label(i),
                       tile.x + m->pad_tile, tile.y + (row_h - text_h) / 2,
                       menu_enabled(i) ? COBALT_COLOUR_TEXT
                                       : COBALT_COLOUR_TEXT_DIM);
   }

   s_drc_hit_valid = true;

   const int list_bottom = top + rows * row_h + (rows - 1) * row_gap;
   const int hint_y = list_bottom + m->gap;
   cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, menu_hint(app->selected),
                            m->pad_edge, hint_y, list_w, 1,
                            COBALT_COLOUR_TEXT);
   cobalt_app_draw_notice(app, r, hint_y + hint_h + row_gap, list_w);

   cobalt_draw_hints(r, app->display == COBALT_DISPLAY_DUAL ? "TV + GamePad" : "GamePad only");
}

void
cobalt_app_draw_feeds(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   char subtitle[32];
   const int saved = cobalt_session_saved_feeds()->count;
   if (saved > 0) {
      snprintf(subtitle, sizeof(subtitle), "%d saved feed%s", saved,
               saved == 1 ? "" : "s");
   } else {
      snprintf(subtitle, sizeof(subtitle), "What's Hot");
   }
   cobalt_draw_page_title(r, "Feeds", subtitle);

   const int top = cobalt_content_top(r);
   /* 44 on the GamePad for the same reason as the account rows. */
   const int row_h = m->font_body * 2 + (surface == COBALT_SURFACE_DRC ? 2 : 0);
   const int width = m->width - 2 * m->pad_edge;
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   const int step = row_h + m->gap / 2;
   const int bottom = cobalt_content_bottom(r);
   int fit = (bottom - top + m->gap / 2) / step;
   if (fit < 1) fit = 1;
   if (surface == COBALT_SURFACE_DRC) {
      s_feeds_rows = fit;
   }

   int row_y = top;
   int drawn = 0;
   for (int i = app->feeds_scroll; i < feed_count() && drawn < fit; i++, drawn++) {
      SDL_Rect row = { m->pad_edge, row_y, width, row_h };
      const bool focused = (app->feeds_selected == i);
      cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);
      cobalt_draw_text_centred(r, COBALT_FONT_HEADING, feed_at(i).label, row.x,
                               row.y + (row_h - label_h) / 2, row.w,
                               COBALT_COLOUR_TEXT);

      if (surface == COBALT_SURFACE_DRC) {
         s_feeds_hit[i] = row;
      }
      row_y += step;
   }
   if (surface == COBALT_SURFACE_DRC) {
      s_feeds_hit_valid = true;
      s_feeds_first = app->feeds_scroll;
      s_feeds_visible = drawn;
   }

   if (feed_count() > fit) {
      char pos[32];
      snprintf(pos, sizeof(pos), "%d of %d", app->feeds_selected + 1, feed_count());
      cobalt_draw_text(r, COBALT_FONT_CAPTION, pos, m->width - m->pad_edge - 90,
                       footer_label_y(r), COBALT_COLOUR_TEXT_DIM);
   }

   cobalt_app_draw_notice(app, r, row_y - m->gap / 2 + m->gap / 2, width);

   cobalt_draw_hints(r, "A / touch: open");
}

/* The TV's idle card in Off-TV mode: enough to show the app is alive and
 * where to look, without duplicating a UI nobody is watching. */
void
cobalt_app_draw_tv_idle(cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   SDL_Rect card = { m->width / 2 - 320, m->height / 2 - 110, 640, 220 };
   cobalt_draw_tile(r, &card, 0.0f);

   cobalt_draw_text_centred(r, COBALT_FONT_HEADING, "Playing on the GamePad",
                            card.x, card.y + m->pad_tile * 2, card.w,
                            COBALT_COLOUR_TEXT);
   cobalt_draw_text_centred(r, COBALT_FONT_BODY,
                            "Select \"TV display\" to bring the TV view back",
                            card.x, card.y + m->pad_tile * 2 + 60, card.w,
                            COBALT_COLOUR_TEXT_DIM);
}
