/* The home menu and the small menu screens: account, feed picker and sign in. */

#include "app/app_internal.h"

typedef enum {
   ACTION_TIMELINE = 0,
   ACTION_COMPOSE,
   ACTION_SEARCH,
   ACTION_FEEDS,
   ACTION_LISTS,
   ACTION_NOTIFICATIONS,
   ACTION_ACCOUNT,
   ACTION_UPDATES,
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
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
   ACTION_COMPOSE,
   ACTION_SEARCH,
   ACTION_FEEDS,
   ACTION_LISTS,
   ACTION_NOTIFICATIONS,
   ACTION_ACCOUNT,
   ACTION_UPDATES,
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
   ACTION_QUIT,
};

_Static_assert((int) (sizeof(MENU) / sizeof(MENU[0])) == COBALT_HOME_MENU_COUNT,
               "the home menu table and COBALT_HOME_MENU_COUNT disagree");
#define MENU_COUNT COBALT_HOME_MENU_COUNT


/* Hit rectangles for the GamePad list, recomputed on draw so touch and the
 * drawn layout can never drift apart. */
static SDL_Rect s_drc_hit[MENU_COUNT];
static bool s_drc_hit_valid = false;

/* Hit rectangles for the account screen's small menu. */
typedef enum {
   ACCOUNT_ROW_PROFILE = 0,
   ACCOUNT_ROW_MUTED,
   ACCOUNT_ROW_BLOCKED,
   ACCOUNT_ROW_LANG,
   ACCOUNT_ROW_SIGN_OUT,
   ACCOUNT_ROW_COUNT,
} account_row;

static SDL_Rect s_account_hit[ACCOUNT_ROW_COUNT];
static bool s_account_hit_valid = false;

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

static const char *
menu_label(int index)
{
   switch (MENU[index]) {
      case ACTION_TIMELINE:       return "Timeline";
      case ACTION_COMPOSE:        return "New post";
      case ACTION_SEARCH:         return "Search";
      case ACTION_FEEDS:          return "Feeds";
      case ACTION_LISTS:          return "Lists";
      case ACTION_NOTIFICATIONS:  return "Notifications";
      case ACTION_ACCOUNT:        return signed_in() ? "Account" : "Sign in";
      case ACTION_UPDATES:        return "Updates";
      case ACTION_DIAGNOSTICS:    return "Diagnostics";
      case ACTION_TOGGLE_DISPLAY: return "TV display";
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
      case ACTION_ACCOUNT:
         if (signed_in()) {
            return cobalt_session_handle();
         }
         return cobalt_session_available() ? "Connect with an app password"
                                           : "Unavailable — see Diagnostics";
      case ACTION_UPDATES:
         return "Check GitHub for a newer Cobalt";
      case ACTION_DIAGNOSTICS:
         return "Paths, network and library status";
      case ACTION_TOGGLE_DISPLAY:
         return "Switch between TV+GamePad and Off-TV";
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
      case ACTION_COMPOSE:
      case ACTION_SEARCH:
      case ACTION_FEEDS:
      case ACTION_LISTS:
      case ACTION_NOTIFICATIONS:
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

      case ACTION_COMPOSE:
         cobalt_compose_init(&app->compose);
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

      case ACTION_ACCOUNT:
         if (signed_in()) {
            app->screen = COBALT_SCREEN_ACCOUNT;
         } else {
            cobalt_signin_set_status(&app->signin, "", false);
            app->screen = COBALT_SCREEN_SIGN_IN;
         }
         break;

      case ACTION_UPDATES:
         app->screen = COBALT_SCREEN_UPDATE;
         cobalt_update_view_open(&app->update);
         COBALT_LOGI("menu: opened updates");
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
   /* The GamePad is the controller and always shows the vertical list, so
    * every direction steps by one whatever the TV is drawing; the TV grid just
    * follows the highlight. */
   const int vstep = 1;

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
cobalt_app_update_account(cobalt_app *app, const cobalt_input *in)
{
   if (cobalt_session_busy()) {
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      app->screen = COBALT_SCREEN_HOME;
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      app->account_selected = (app->account_selected + 1) % ACCOUNT_ROW_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      app->account_selected =
         (app->account_selected + ACCOUNT_ROW_COUNT - 1) % ACCOUNT_ROW_COUNT;
   }

   int activated = -1;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      activated = app->account_selected;
   } else if (s_account_hit_valid && in->touch_ended) {
      for (int i = 0; i < ACCOUNT_ROW_COUNT; i++) {
         if (cobalt_input_tapped(in, &s_account_hit[i])) {
            app->account_selected = i;
            activated = i;
            break;
         }
      }
   }

   switch (activated) {
      case ACCOUNT_ROW_PROFILE: {
         const char *self = cobalt_session_did();
         if (self[0]) {
            cobalt_session_begin_profile(self);
            cobalt_profile_view_rewind(&app->profile);
            app->profile_return = COBALT_SCREEN_ACCOUNT;
            app->screen = COBALT_SCREEN_PROFILE;
            COBALT_LOGI("account: opened own profile");
         }
         break;
      }
      case ACCOUNT_ROW_MUTED:
         cobalt_graph_view_open(&app->graph, COBALT_GRAPH_MUTED);
         app->screen = COBALT_SCREEN_MUTED_LIST;
         COBALT_LOGI("account: opened muted accounts");
         break;
      case ACCOUNT_ROW_BLOCKED:
         cobalt_graph_view_open(&app->graph, COBALT_GRAPH_BLOCKED);
         app->screen = COBALT_SCREEN_BLOCKED_LIST;
         COBALT_LOGI("account: opened blocked accounts");
         break;
      case ACCOUNT_ROW_LANG:
         cobalt_session_cycle_post_lang();
         break;
      case ACCOUNT_ROW_SIGN_OUT:
         COBALT_LOGI("account: sign out requested");
         cobalt_session_begin_logout();
         break;
      default:
         break;
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

   /* Two rows of large tiles, Wii U menu style. Ten in one row left each
    * about 100px wide, which truncated every label. */
   const int cols = (MENU_COUNT + 1) / 2;
   const int rows = (MENU_COUNT + cols - 1) / cols;
   const int top = cobalt_content_top(r);
   const int tile_h = 130;
   const int tile_w = (m->width - 2 * m->pad_edge - m->gap * (cols - 1)) / cols;

   for (int i = 0; i < MENU_COUNT; i++) {
      const int col = i % cols;
      const int row = i / cols;
      SDL_Rect tile = { m->pad_edge + col * (tile_w + m->gap),
                        top + row * (tile_h + m->gap), tile_w, tile_h };
      cobalt_draw_tile(r, &tile, app->focus[i]);

      SDL_Color label = menu_enabled(i) ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      int label_w = 0;
      cobalt_text_size(r, COBALT_FONT_BODY, menu_label(i), &label_w, NULL);
      cobalt_draw_text_wrapped(r, label_w > tile.w - 2 * m->pad_tile ? COBALT_FONT_CAPTION
                                                                      : COBALT_FONT_BODY,
                               menu_label(i), tile.x + m->pad_tile, tile.y + m->pad_tile,
                               tile.w - 2 * m->pad_tile, 2, label);

      if (!menu_enabled(i)) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION,
                          (signed_in() || MENU[i] == ACTION_ACCOUNT) ? "Unavailable" : "Sign in first",
                          tile.x + m->pad_tile, tile.y + tile.h - m->pad_tile - 24,
                          COBALT_COLOUR_TEXT_DIM);
      }
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
   cobalt_app_draw_header(r, "Off-TV ready");

   /* A vertical list: denser than the TV row, and every row is a touch target
    * comfortably larger than a fingertip. */
   const int top = cobalt_content_top(r);
   const int row_h = 44;
   const int pitch = row_h + m->gap / 2;
   const int list_w = m->width - 2 * m->pad_edge;

   /* Ten rows do not fit in 480px: show a window that follows the selection and
    * leaves room under it for a notice and the footer. */
   int visible = (m->height - top - m->pad_edge - (app->notice[0] ? 70 : 34)) / pitch;
   if (visible < 1) visible = 1;
   if (visible > MENU_COUNT) visible = MENU_COUNT;
   static int first;
   if (app->selected < first) first = app->selected;
   if (app->selected >= first + visible) first = app->selected - visible + 1;
   if (first > MENU_COUNT - visible) first = MENU_COUNT - visible;
   if (first < 0) first = 0;

   for (int i = 0; i < MENU_COUNT; i++) {
      if (i < first || i >= first + visible) {
         SDL_Rect none = { 0, 0, 0, 0 };
         s_drc_hit[i] = none;
         continue;
      }
      SDL_Rect row = { m->pad_edge, top + (i - first) * pitch, list_w, row_h };
      s_drc_hit[i] = row;

      cobalt_draw_tile(r, &row, app->focus[i]);

      SDL_Color label = menu_enabled(i) ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      int text_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text(r, COBALT_FONT_BODY, menu_label(i),
                       row.x + m->pad_tile, row.y + (row_h - text_h) / 2, label);

      const char *hint_text = menu_hint(i);
      int hint_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, hint_text, &hint_w, NULL);
      if (hint_w > 0 && hint_w < list_w / 2) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, hint_text,
                          row.x + list_w - m->pad_tile - hint_w,
                          row.y + (row_h - cobalt_font_line_height(r, COBALT_FONT_CAPTION)) / 2,
                          COBALT_COLOUR_TEXT_DIM);
      }
   }

   s_drc_hit_valid = true;

   char more[32] = "";
   if (visible < MENU_COUNT) {
      snprintf(more, sizeof more, "%d of %d", app->selected + 1, MENU_COUNT);
   }

   const int list_bottom = top + visible * pitch;
   cobalt_app_draw_notice(app, r, list_bottom + 4, list_w);

   SDL_Color hint = { 0x4F, 0x5C, 0x66, 0xFF };
   cobalt_draw_hints(r, app->display == COBALT_DISPLAY_DUAL ? "TV + GamePad" : "GamePad only");
   if (more[0]) {
      int w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, more, &w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, more, m->width - m->pad_edge - w,
                       m->height - m->pad_edge - 20, hint);
   }
}

void
cobalt_app_draw_account(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_app_draw_header(r, "Account");

   const int top = cobalt_content_top(r);
   const int row_h = m->font_body * 2;
   const int width = m->width - 2 * m->pad_edge;

   SDL_Rect panel = { m->pad_edge, top, width, row_h * 3 };
   cobalt_draw_tile(r, &panel, app->account_selected == ACCOUNT_ROW_PROFILE ? 1.0f : 0.0f);
   if (surface == COBALT_SURFACE_DRC) {
      s_account_hit[ACCOUNT_ROW_PROFILE] = panel;
   }

   char lines[3][COBALT_MESSAGE_MAX];
   snprintf(lines[0], sizeof(lines[0]), "%s", cobalt_session_handle());
   snprintf(lines[1], sizeof(lines[1]), "%s", cobalt_session_did());
   snprintf(lines[2], sizeof(lines[2]), "%s  -  A: view your profile", cobalt_session_service());

   const cobalt_font_id fonts[3] = {
      COBALT_FONT_HEADING, COBALT_FONT_CAPTION, COBALT_FONT_CAPTION
   };
   const SDL_Color colours[3] = {
      COBALT_COLOUR_TEXT, COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_TEXT_DIM
   };

   int y = panel.y + m->pad_tile;
   for (int i = 0; i < 3; i++) {
      y += cobalt_draw_text_wrapped(r, fonts[i], lines[i], panel.x + m->pad_tile, y,
                                    panel.w - 2 * m->pad_tile, 1, colours[i]);
   }

   char lang_label[48];
   snprintf(lang_label, sizeof(lang_label), "Post language: %s",
            cobalt_session_post_lang()[0] ? cobalt_session_post_lang() : "none");
   const char *ROW_LABEL[ACCOUNT_ROW_COUNT] = {
      "", "Muted accounts", "Blocked accounts", lang_label, "Sign out",
   };
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   int row_y = panel.y + panel.h + m->gap;
   for (int i = ACCOUNT_ROW_MUTED; i < ACCOUNT_ROW_COUNT; i++) {
      SDL_Rect row = { m->pad_edge, row_y, width, row_h };
      const bool focused = (app->account_selected == i);
      cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);
      cobalt_draw_text_centred(r, COBALT_FONT_HEADING, ROW_LABEL[i], row.x,
                               row.y + (row_h - label_h) / 2, row.w,
                               i == ACCOUNT_ROW_SIGN_OUT ? COBALT_COLOUR_ERROR
                                                        : COBALT_COLOUR_TEXT);

      if (surface == COBALT_SURFACE_DRC) {
         s_account_hit[i] = row;
      }
      row_y += row_h + m->gap / 2;
   }
   if (surface == COBALT_SURFACE_DRC) {
      s_account_hit_valid = true;
   }

   cobalt_app_draw_notice(app, r, row_y + m->gap / 2, width);

   cobalt_draw_hints(r, "A / touch: open");
}

void
cobalt_app_draw_feeds(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_app_draw_header(r, "Feeds");

   const int top = cobalt_content_top(r);
   const int row_h = m->font_body * 2;
   const int width = m->width - 2 * m->pad_edge;
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   const int step = row_h + m->gap / 2;
   const int bottom = m->height - m->pad_edge - 28;
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
   }

   if (feed_count() > fit) {
      char pos[32];
      snprintf(pos, sizeof(pos), "%d of %d", app->feeds_selected + 1, feed_count());
      cobalt_draw_text(r, COBALT_FONT_CAPTION, pos, m->width - m->pad_edge - 90,
                       m->height - m->pad_edge - 20, COBALT_COLOUR_TEXT_DIM);
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
