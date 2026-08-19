#include "app/app.h"
#include "app/compose.h"
#include "app/graph.h"
#include "app/notify.h"
#include "app/profile.h"
#include "app/search.h"
#include "app/signin.h"
#include "app/thread.h"
#include "app/timeline.h"
#include "atproto/atproto.h"
#include "atproto/session.h"
#include "net/net.h"
#include "ui/imagecache.h"
#include "util/log.h"
#include "util/paths.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
   ACTION_TIMELINE = 0,
   ACTION_COMPOSE,
   ACTION_SEARCH,
   ACTION_NOTIFICATIONS,
   ACTION_ACCOUNT,
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
   ACTION_NOTIFICATIONS,
   ACTION_ACCOUNT,
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
   ACTION_QUIT,
};

#define MENU_COUNT ((int) (sizeof(MENU) / sizeof(MENU[0])))

/* Focus animation, in units of "fraction of the way there per frame". */
#define FOCUS_RATE 0.25f

struct cobalt_app {
   cobalt_screen screen;
   cobalt_display_mode display;
   int selected;
   float focus[MENU_COUNT];

   cobalt_signin signin;
   cobalt_timeline timeline;
   cobalt_thread_view thread;
   cobalt_compose compose;
   cobalt_notify_view notify;
   cobalt_profile_view profile;
   cobalt_graph_view graph;
   cobalt_search_view search;
   /* Which row is highlighted on the account screen's small menu. */
   int account_selected;
   /* Where B from the profile screen returns to. */
   cobalt_screen profile_return;
   /* Where to return after composing — the timeline or the thread. */
   cobalt_screen compose_return;
   /* Where B from the thread screen returns to — the timeline or notifications. */
   cobalt_screen thread_return;

   /* Last completed request's message, shown on the home and account screens
    * so an auto-resume that failed while nobody was looking is not silent. */
   char notice[COBALT_MESSAGE_MAX];
   bool notice_is_error;

   /* The resume attempt is fired from the first update rather than from
    * startup, so the app has already drawn a frame and a slow PDS shows a live
    * screen instead of a black one. */
   bool resume_attempted;

   bool quit;
   uint32_t frames;

   /* Cached once — curl_version() returns a static string but formatting it
    * every frame would allocate in the render loop. */
   char curl_version[64];
   char sdl_version[32];
};

/* Hit rectangles for the GamePad list, recomputed on draw so touch and the
 * drawn layout can never drift apart. */
static SDL_Rect s_drc_hit[MENU_COUNT];
static bool s_drc_hit_valid = false;

/* Hit rectangles for the account screen's small menu. */
typedef enum {
   ACCOUNT_ROW_MUTED = 0,
   ACCOUNT_ROW_BLOCKED,
   ACCOUNT_ROW_SIGN_OUT,
   ACCOUNT_ROW_COUNT,
} account_row;

static SDL_Rect s_account_hit[ACCOUNT_ROW_COUNT];
static bool s_account_hit_valid = false;

/* --- menu description --- */

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
      case ACTION_NOTIFICATIONS:  return "Notifications";
      case ACTION_ACCOUNT:        return signed_in() ? "Account" : "Sign in";
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
      case ACTION_NOTIFICATIONS:
         return signed_in() ? "Replies, likes and follows"
                            : "Sign in to see notifications";
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
      case ACTION_NOTIFICATIONS:
         return signed_in();
      case ACTION_ACCOUNT:
         return cobalt_session_available() || signed_in();
      default:
         return true;
   }
}

cobalt_app *
cobalt_app_create(void)
{
   cobalt_app *app = (cobalt_app *) calloc(1, sizeof(cobalt_app));
   if (!app) {
      COBALT_LOGE("out of memory allocating app state");
      return NULL;
   }

   app->screen = COBALT_SCREEN_HOME;
   app->display = COBALT_DISPLAY_DUAL;
   app->selected = 1; /* Sign in — the one thing worth doing on run one. */

   cobalt_signin_init(&app->signin);
   cobalt_timeline_init(&app->timeline);
   cobalt_thread_view_init(&app->thread);
   cobalt_notify_view_init(&app->notify);
   cobalt_profile_view_init(&app->profile);
   cobalt_graph_view_init(&app->graph);
   cobalt_search_view_init(&app->search);

   curl_version_info_data *curl_info = curl_version_info(CURLVERSION_NOW);
   snprintf(app->curl_version, sizeof(app->curl_version), "curl %s / %s",
            curl_info ? curl_info->version : "?",
            (curl_info && curl_info->ssl_version) ? curl_info->ssl_version : "no TLS");

   SDL_version linked;
   SDL_GetVersion(&linked);
   snprintf(app->sdl_version, sizeof(app->sdl_version), "SDL %u.%u.%u",
            linked.major, linked.minor, linked.patch);

   COBALT_LOGI("app up: %s, %s", app->sdl_version, app->curl_version);
   return app;
}

void
cobalt_app_destroy(cobalt_app *app)
{
   if (app) {
      /* The password buffer lives in this allocation; do not hand it back to
       * the heap still holding one. */
      cobalt_signin_clear_password(&app->signin);
   }
   free(app);
}

bool
cobalt_app_should_quit(const cobalt_app *app)
{
   return app && app->quit;
}

static void
set_notice(cobalt_app *app, const char *message, bool is_error)
{
   snprintf(app->notice, sizeof(app->notice), "%s", message ? message : "");
   app->notice_is_error = is_error;
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
         if (cobalt_session_feed()->count == 0) {
            cobalt_session_begin_timeline(false);
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

/* --- session plumbing --- */

static void
handle_job_result(cobalt_app *app, const cobalt_job_result *result)
{
   switch (result->kind) {
      case COBALT_JOB_LOGIN:
         if (result->ok) {
            /* Wolfram has its own copy now, so this one has no reason to live
             * any longer — and it is about to sit in an idle screen's state. */
            cobalt_signin_clear_password(&app->signin);
            cobalt_signin_set_status(&app->signin, "", false);

            /* Land on the feed, not on a confirmation screen. Signing in is a
             * means to an end, and the account details are one menu entry
             * away for anyone who wants them. */
            cobalt_timeline_rewind(&app->timeline);
            app->screen = COBALT_SCREEN_TIMELINE;
            cobalt_session_begin_timeline(false);

            char message[COBALT_MESSAGE_MAX];
            if (result->message[0]) {
               /* Signed in, but with a caveat worth repeating verbatim. */
               snprintf(message, sizeof(message), "%s", result->message);
               set_notice(app, message, true);
            } else {
               snprintf(message, sizeof(message), "Signed in as %s",
                        cobalt_session_handle());
               set_notice(app, message, false);
            }
         } else {
            cobalt_signin_set_status(&app->signin, result->message, true);
         }
         break;

      case COBALT_JOB_RESUME:
         if (result->ok) {
            char message[COBALT_MESSAGE_MAX];
            snprintf(message, sizeof(message), "Signed in as %s",
                     cobalt_session_handle());
            set_notice(app, message, false);
            /* Warm the feed while the user is still looking at the menu, so
             * opening it is instant rather than a spinner. */
            cobalt_timeline_rewind(&app->timeline);
            cobalt_session_begin_timeline(false);
         } else {
            /* A failed resume is not an error the user asked for, so it lands
             * on the home screen as a notice rather than throwing them into
             * the sign-in form. */
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_LOGOUT:
         set_notice(app, "Signed out.", false);
         cobalt_signin_init(&app->signin);
         cobalt_timeline_init(&app->timeline);
   cobalt_thread_view_init(&app->thread);
   cobalt_notify_view_init(&app->notify);
   cobalt_profile_view_init(&app->profile);
   cobalt_graph_view_init(&app->graph);
   cobalt_search_view_init(&app->search);
         app->screen = COBALT_SCREEN_HOME;
         app->selected = 1;
         break;

      case COBALT_JOB_POST:
         if (result->ok) {
            set_notice(app, cobalt_compose_is_reply(&app->compose)
                               ? "Reply posted." : "Posted.", false);
            /* Back to where composing started, and refresh so the new post is
             * actually visible rather than only claimed. */
            app->screen = app->compose_return;
            if (app->compose_return == COBALT_SCREEN_THREAD &&
                app->compose.parent_uri[0]) {
               /* The refetch re-roots on the parent, which is usually a much
                * shorter conversation than the one being read — without this
                * the cursor stays where it was and lands past the end. */
               cobalt_thread_view_reset(&app->thread);
               cobalt_session_begin_thread(app->compose.parent_uri);
            } else {
               cobalt_session_begin_timeline(false);
               cobalt_timeline_rewind(&app->timeline);
            }
            cobalt_compose_init(&app->compose);
         } else {
            /* Stay on the compose screen with the text intact — a failed post
             * must not silently eat something someone typed on a D-pad. */
            set_notice(app, result->message, true);
         }
         break;

      case COBALT_JOB_NOTIFICATIONS:
      case COBALT_JOB_PROFILE:
      case COBALT_JOB_FOLLOW:
      case COBALT_JOB_THREAD:
      case COBALT_JOB_LIKE:
      case COBALT_JOB_REPOST:
         /* Success is visible in the card itself — the count moved and the
          * marker appeared — so only failures are worth saying out loud. */
         if (!result->ok) {
            set_notice(app, result->message, true);
         } else if (result->message[0]) {
            set_notice(app, result->message, false);
         }
         break;

      case COBALT_JOB_TIMELINE:
         if (!result->ok) {
            /* A notice rather than an error screen: a failed refresh should
             * leave whatever was already on screen readable. */
            set_notice(app, result->message, true);
         } else {
            /* Carries the "your timeline is empty" explanation on success. */
            set_notice(app, result->message, result->message[0] != '\0');
         }
         break;

      case COBALT_JOB_NONE:
      default:
         break;
   }
}

static void
update_home(cobalt_app *app, const cobalt_input *in)
{
   /* Both axes move the selection so the same code serves the TV's horizontal
    * row and the GamePad's vertical list. */
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) ||
       cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
      app->selected = (app->selected + 1) % MENU_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) ||
       cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
      app->selected = (app->selected + MENU_COUNT - 1) % MENU_COUNT;
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

static void
update_account(cobalt_app *app, const cobalt_input *in)
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
      case ACCOUNT_ROW_SIGN_OUT:
         COBALT_LOGI("account: sign out requested");
         cobalt_session_begin_logout();
         break;
      default:
         break;
   }
}

static void
update_signin(cobalt_app *app, const cobalt_input *in)
{
   switch (cobalt_signin_update(&app->signin, in)) {
      case COBALT_SIGNIN_BACK:
         cobalt_signin_clear_password(&app->signin);
         app->screen = COBALT_SCREEN_HOME;
         break;

      case COBALT_SIGNIN_SUBMIT:
         if (!cobalt_session_begin_login(app->signin.service,
                                         app->signin.identifier,
                                         app->signin.password)) {
            cobalt_signin_set_status(&app->signin,
                                     "Could not start the sign-in request.", true);
         } else {
            cobalt_signin_set_status(&app->signin, "", false);
         }
         break;

      case COBALT_SIGNIN_STAY:
      default:
         break;
   }
}

void
cobalt_app_update(cobalt_app *app, const cobalt_input *in, uint32_t now_ms)
{
   if (!app || !in) {
      return;
   }

   (void) now_ms;
   app->frames++;

   if (in->quit_requested) {
      app->quit = true;
      return;
   }

   if (!app->resume_attempted) {
      app->resume_attempted = true;
      if (cobalt_session_available() && cobalt_session_has_saved()) {
         COBALT_LOGI("app: stored session found, resuming");
         cobalt_session_begin_resume();
      }
   }

   cobalt_job_result result;
   if (cobalt_session_poll(&result)) {
      COBALT_LOGI("app: job %d finished ok=%d", (int) result.kind, (int) result.ok);
      handle_job_result(app, &result);
   }

   /*
    * Screens read the worker's feed, thread and notification buffers directly,
    * and the worker rewrites them in place while they are on screen — a like
    * moves a count, a refresh clears and refills the list. Holding the session
    * lock across the whole update is what makes that safe; the lock is never
    * held across network I/O, so this costs nothing.
    */
   cobalt_session_lock();

   switch (app->screen) {
      case COBALT_SCREEN_DIAGNOSTICS:
         if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
            app->screen = COBALT_SCREEN_HOME;
         }
         break;

      case COBALT_SCREEN_PROFILE:
         switch (cobalt_profile_view_update(&app->profile, in)) {
            case COBALT_PROFILE_VIEW_BACK:
               app->screen = app->profile_return;
               break;
            case COBALT_PROFILE_VIEW_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               app->thread_return = COBALT_SCREEN_PROFILE;
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_PROFILE_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_TIMELINE:
         switch (cobalt_timeline_update(&app->timeline, in)) {
            case COBALT_TIMELINE_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_TIMELINE_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               app->thread_return = COBALT_SCREEN_TIMELINE;
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_TIMELINE_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               app->profile_return = COBALT_SCREEN_TIMELINE;
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_TIMELINE_COMPOSE:
               cobalt_compose_init(&app->compose);
               app->compose_return = COBALT_SCREEN_TIMELINE;
               app->screen = COBALT_SCREEN_COMPOSE;
               break;
            case COBALT_TIMELINE_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_THREAD:
         switch (cobalt_thread_view_update(&app->thread, in)) {
            case COBALT_THREAD_VIEW_BACK:
               app->screen = app->thread_return;
               break;
            case COBALT_THREAD_VIEW_REPLY: {
               const cobalt_thread *conv = cobalt_session_thread();
               if (app->thread.selected < conv->count) {
                  cobalt_compose_reply_to(&app->compose,
                                          &conv->posts[app->thread.selected]);
                  app->compose_return = COBALT_SCREEN_THREAD;
                  app->screen = COBALT_SCREEN_COMPOSE;
               }
               break;
            }
            case COBALT_THREAD_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_NOTIFICATIONS:
         switch (cobalt_notify_view_update(&app->notify, in)) {
            case COBALT_NOTIFY_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_NOTIFY_OPEN_THREAD:
               cobalt_thread_view_reset(&app->thread);
               app->thread_return = COBALT_SCREEN_NOTIFICATIONS;
               app->screen = COBALT_SCREEN_THREAD;
               break;
            case COBALT_NOTIFY_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_COMPOSE:
         switch (cobalt_compose_update(&app->compose, in)) {
            case COBALT_COMPOSE_CANCELLED:
               app->screen = app->compose_return;
               break;
            case COBALT_COMPOSE_SUBMIT:
               if (!cobalt_session_begin_post(app->compose.text,
                                              app->compose.parent_uri,
                                              app->compose.parent_cid,
                                              app->compose.root_uri,
                                              app->compose.root_cid)) {
                  set_notice(app, "Could not start that post.", true);
               }
               break;
            case COBALT_COMPOSE_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_SIGN_IN:
         update_signin(app, in);
         break;

      case COBALT_SCREEN_ACCOUNT:
         update_account(app, in);
         break;

      case COBALT_SCREEN_MUTED_LIST:
      case COBALT_SCREEN_BLOCKED_LIST:
         switch (cobalt_graph_view_update(&app->graph, in)) {
            case COBALT_GRAPH_VIEW_BACK:
               app->screen = COBALT_SCREEN_ACCOUNT;
               break;
            case COBALT_GRAPH_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_SEARCH:
         switch (cobalt_search_view_update(&app->search, in)) {
            case COBALT_SEARCH_VIEW_BACK:
               app->screen = COBALT_SCREEN_HOME;
               break;
            case COBALT_SEARCH_VIEW_OPEN_PROFILE:
               cobalt_profile_view_rewind(&app->profile);
               app->profile_return = COBALT_SCREEN_SEARCH;
               app->screen = COBALT_SCREEN_PROFILE;
               break;
            case COBALT_SEARCH_VIEW_STAY:
            default:
               break;
         }
         break;

      case COBALT_SCREEN_HOME:
      default:
         update_home(app, in);
         break;
   }

   cobalt_session_unlock();

   /* Ease focus toward the selection so tiles settle rather than snap. */
   for (int i = 0; i < MENU_COUNT; i++) {
      float target = (i == app->selected) ? 1.0f : 0.0f;
      app->focus[i] += (target - app->focus[i]) * FOCUS_RATE;
   }
}

/* --- drawing --- */

static void
draw_header(cobalt_render *r, const char *subtitle)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Cobalt", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   int title_h = cobalt_font_line_height(r, COBALT_FONT_TITLE);
   SDL_Color dim = { 0xD8, 0xE6, 0xF4, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                    m->pad_edge + title_h - m->line_gap, dim);
}

/* The one-line status the home and account screens share. */
static void
draw_notice(const cobalt_app *app, cobalt_render *r, int y, int width)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   if (cobalt_session_busy()) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, "Working...", m->pad_edge, y,
                       COBALT_COLOUR_TILE);
      return;
   }

   if (app->notice[0] == '\0') {
      return;
   }

   cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, app->notice, m->pad_edge, y,
                            width, 2,
                            app->notice_is_error ? COBALT_COLOUR_ERROR
                                                 : COBALT_COLOUR_TILE);
}

static void
draw_home_tv(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(r, "AT Protocol for Wii U");

   /* A single row of large tiles, Wii U menu style — few, big, readable from
    * across a room rather than a dense list. */
   const int top = m->pad_edge + 130;
   const int tile_h = 210;
   const int total_gap = m->gap * (MENU_COUNT - 1);
   const int tile_w = (m->width - 2 * m->pad_edge - total_gap) / MENU_COUNT;

   for (int i = 0; i < MENU_COUNT; i++) {
      SDL_Rect tile = { m->pad_edge + i * (tile_w + m->gap), top, tile_w, tile_h };
      cobalt_draw_tile(r, &tile, app->focus[i]);

      SDL_Color label = menu_enabled(i) ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      cobalt_draw_text_wrapped(r, COBALT_FONT_HEADING, menu_label(i),
                               tile.x + m->pad_tile, tile.y + m->pad_tile,
                               tile.w - 2 * m->pad_tile, 2, label);

      if (!menu_enabled(i)) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, "Coming soon",
                          tile.x + m->pad_tile, tile.y + tile.h - m->pad_tile - 24,
                          COBALT_COLOUR_TEXT_DIM);
      }
   }

   /* Detail strip for the focused tile: the TV has room, so use it rather
    * than cramming the hint into the tile. */
   const int detail_y = top + tile_h + m->gap * 2;
   cobalt_draw_text(r, COBALT_FONT_BODY, menu_hint(app->selected),
                    m->pad_edge, detail_y, COBALT_COLOUR_TILE);

   draw_notice(app, r, detail_y + m->font_body + m->gap, m->width - 2 * m->pad_edge);

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    "A / touch: select     B: back     D-pad or stick: move",
                    m->pad_edge, m->height - m->pad_edge - 24, hint);
}

static void
draw_home_drc(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(r, "Off-TV ready");

   /* A vertical list: denser than the TV row, and every row is a touch target
    * comfortably larger than a fingertip. */
   const int top = m->pad_edge + 62;
   const int row_h = 54;
   const int list_w = m->width - 2 * m->pad_edge;

   for (int i = 0; i < MENU_COUNT; i++) {
      SDL_Rect row = { m->pad_edge, top + i * (row_h + m->gap / 2), list_w, row_h };
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

   const int list_bottom = top + MENU_COUNT * (row_h + m->gap / 2);
   draw_notice(app, r, list_bottom + m->gap, list_w);

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    app->display == COBALT_DISPLAY_DUAL ? "TV + GamePad" : "GamePad only",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

static void
draw_account(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(r, "Account");

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int row_h = m->font_body * 2;
   const int width = m->width - 2 * m->pad_edge;

   SDL_Rect panel = { m->pad_edge, top, width, row_h * 3 };
   cobalt_draw_tile(r, &panel, 0.0f);

   char lines[3][COBALT_MESSAGE_MAX];
   snprintf(lines[0], sizeof(lines[0]), "%s", cobalt_session_handle());
   snprintf(lines[1], sizeof(lines[1]), "%s", cobalt_session_did());
   snprintf(lines[2], sizeof(lines[2]), "%s", cobalt_session_service());

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

   static const char *ROW_LABEL[ACCOUNT_ROW_COUNT] = {
      "Muted accounts", "Blocked accounts", "Sign out",
   };
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   int row_y = panel.y + panel.h + m->gap;
   for (int i = 0; i < ACCOUNT_ROW_COUNT; i++) {
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

   draw_notice(app, r, row_y + m->gap / 2, width);

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, "A / touch: open     B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

static void
draw_diagnostics(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(r, "Diagnostics");

   /*
    * This screen exists because there is no emulator (AGENTS.md §10). It is
    * the fastest way to confirm, on the console itself, that asset paths
    * resolved, the network came up, the TLS trust store is present and the
    * session layer can run — the things that block everything downstream.
    */
   const cobalt_net_status net = cobalt_net_get_status();
   const char *ca = cobalt_session_ca_path();
   const char *blocker = cobalt_session_blocker();

   char lines[16][160];
   int count = 0;

   snprintf(lines[count++], sizeof(lines[0]), "Surface: %s (%dx%d) frame %u",
            surface == COBALT_SURFACE_DRC ? "GamePad" : "TV", m->width, m->height,
            (unsigned) app->frames);
   snprintf(lines[count++], sizeof(lines[0]), "Content: %s",
            cobalt_content_root() ? cobalt_content_root() : "NOT FOUND");
   snprintf(lines[count++], sizeof(lines[0]), "Data: %s",
            cobalt_data_root() ? cobalt_data_root() : "NOT FOUND");
   snprintf(lines[count++], sizeof(lines[0]), "Network: %s (%s)",
            cobalt_net_status_string(net), cobalt_net_local_address());
   snprintf(lines[count++], sizeof(lines[0]), "Trust store: %s",
            ca ? ca : "MISSING - run `make cacert`");
   snprintf(lines[count++], sizeof(lines[0]), "%s / %s",
            app->sdl_version, app->curl_version);
   snprintf(lines[count++], sizeof(lines[0]), "ATProto SDK: %s",
            cobalt_atproto_sdk_version());
   snprintf(lines[count++], sizeof(lines[0]), "SDK status: %s",
            cobalt_atproto_status_string());
   snprintf(lines[count++], sizeof(lines[0]), "Sign-in: %s",
            blocker ? blocker : "available");
   snprintf(lines[count++], sizeof(lines[0]), "Net worker: %s",
            cobalt_session_threaded() ? "background thread"
                                      : "SYNCHRONOUS - requests stall the frame");

   /*
    * Avatars fail quietly by design — a card falls back to its initial — so
    * without a counter here there is no way to tell "nobody has set one" from
    * "every fetch is failing", which are very different problems.
    */
   cobalt_imagecache *images = cobalt_render_images(r);
   if (!images) {
      snprintf(lines[count++], sizeof(lines[0]), "Avatars: off (%s)",
               cobalt_imagecache_supported() ? "cache unavailable"
                                             : "no SDL2_image");
   } else {
      int ready = 0, loading = 0, failed = 0;
      cobalt_imagecache_stats(images, &ready, &loading, &failed);
      snprintf(lines[count++], sizeof(lines[0]),
               "Avatars: %d ready, %d loading, %d failed", ready, loading,
               failed);
   }

   /* Same reasoning, same cache implementation, different instance — see
    * ui/render.h's cobalt_render_set_thumbs(). Reported separately because
    * the two caches can legitimately disagree: post images are far more
    * likely to be large or slow than an avatar. */
   cobalt_imagecache *thumbs = cobalt_render_thumbs(r);
   if (!thumbs) {
      snprintf(lines[count++], sizeof(lines[0]), "Thumbnails: off (%s)",
               cobalt_imagecache_supported() ? "cache unavailable"
                                             : "no SDL2_image");
   } else {
      int ready = 0, loading = 0, failed = 0;
      cobalt_imagecache_stats(thumbs, &ready, &loading, &failed);
      snprintf(lines[count++], sizeof(lines[0]),
               "Thumbnails: %d ready, %d loading, %d failed", ready, loading,
               failed);
   }

   switch (cobalt_session_state()) {
      case COBALT_AUTH_SIGNED_IN:
         snprintf(lines[count++], sizeof(lines[0]), "Session: %s at %s",
                  cobalt_session_handle(), cobalt_session_service());
         break;
      case COBALT_AUTH_WORKING:
         snprintf(lines[count++], sizeof(lines[0]), "Session: request in flight");
         break;
      case COBALT_AUTH_SIGNED_OUT:
      default:
         snprintf(lines[count++], sizeof(lines[0]), "Session: signed out%s",
                  cobalt_session_has_saved() ? " (credentials stored)" : "");
         break;
   }

   snprintf(lines[count++], sizeof(lines[0]), "Font: %s",
            cobalt_render_has_font(r) ? "loaded" : "MISSING");

   /* The GamePad panel is 480px tall and this list is long, so it drops to the
    * caption scale there rather than scrolling. The TV keeps body size. */
   const cobalt_font_id font = (surface == COBALT_SURFACE_DRC) ? COBALT_FONT_CAPTION
                                                               : COBALT_FONT_BODY;

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   SDL_Rect panel = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                      m->height - top - m->pad_edge - 40 };
   cobalt_draw_tile(r, &panel, 0.0f);

   const int line_h = cobalt_font_line_height(r, font) + m->line_gap;
   for (int i = 0; i < count; i++) {
      bool bad = (strstr(lines[i], "NOT FOUND") != NULL) ||
                 (strstr(lines[i], "MISSING") != NULL) ||
                 (strstr(lines[i], "SYNCHRONOUS") != NULL) ||
                 (blocker != NULL && strncmp(lines[i], "Sign-in:", 8) == 0) ||
                 (net != COBALT_NET_UP && strncmp(lines[i], "Network:", 8) == 0);

      cobalt_draw_text_wrapped(r, font, lines[i],
                               panel.x + m->pad_tile, panel.y + m->pad_tile + i * line_h,
                               panel.w - 2 * m->pad_tile, 1,
                               bad ? COBALT_COLOUR_ERROR : COBALT_COLOUR_TEXT);
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, "B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

/* The TV's idle card in Off-TV mode: enough to show the app is alive and
 * where to look, without duplicating a UI nobody is watching. */
static void
draw_tv_idle(cobalt_render *r)
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

void
cobalt_app_draw(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   if (!app || !r) {
      return;
   }

   if (surface == COBALT_SURFACE_TV && app->display == COBALT_DISPLAY_GAMEPAD) {
      draw_tv_idle(r);
      return;
   }

   /* Same reason as cobalt_app_update: the buffers being drawn belong to the
    * worker and it edits them in place. Both surfaces are drawn per frame, so
    * this is taken twice. */
   cobalt_session_lock();

   switch (app->screen) {
      case COBALT_SCREEN_DIAGNOSTICS:
         draw_diagnostics(app, r, surface);
         break;

      case COBALT_SCREEN_TIMELINE:
         cobalt_timeline_draw(&app->timeline, r, surface);
         break;

      case COBALT_SCREEN_THREAD:
         cobalt_thread_view_draw(&app->thread, r, surface);
         break;

      case COBALT_SCREEN_COMPOSE:
         cobalt_compose_draw(&app->compose, r, surface);
         break;

      case COBALT_SCREEN_NOTIFICATIONS:
         cobalt_notify_view_draw(&app->notify, r, surface);
         break;

      case COBALT_SCREEN_PROFILE:
         cobalt_profile_view_draw(&app->profile, r, surface);
         break;

      case COBALT_SCREEN_SIGN_IN:
         cobalt_signin_draw(&app->signin, r, surface);
         break;

      case COBALT_SCREEN_ACCOUNT:
         draw_account(app, r, surface);
         break;

      case COBALT_SCREEN_MUTED_LIST:
      case COBALT_SCREEN_BLOCKED_LIST:
         cobalt_graph_view_draw(&app->graph, r, surface);
         break;

      case COBALT_SCREEN_SEARCH:
         cobalt_search_view_draw(&app->search, r, surface);
         break;

      case COBALT_SCREEN_HOME:
      default:
         if (surface == COBALT_SURFACE_DRC) {
            draw_home_drc(app, r);
         } else {
            draw_home_tv(app, r);
         }
         break;
   }

   cobalt_session_unlock();
}
