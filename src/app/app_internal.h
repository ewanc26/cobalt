#pragma once

/*
 * What the app's files share and nothing else may use: the application state,
 * and the screens' update and draw entry points that app.c dispatches to.
 * The public interface is app/app.h.
 *
 * app.c owns the lifecycle, the job results and the per-frame dispatch;
 * app_home.c the home menu and the small menu screens (account, feeds, sign
 * in); app_menus.c the More menus and what choosing from them does;
 * app_diag.c the diagnostics screen.
 */

#include "app/app.h"
#include "app/compose.h"
#include "app/entropyview.h"
#include "app/graph.h"
#include "app/lists.h"
#include "app/notify.h"
#include "app/profile.h"
#include "app/search.h"
#include "app/signin.h"
#include "app/thread.h"
#include "audio/sound.h"
#include "util/buildinfo.h"
#include "app/timeline.h"
#include "app/update.h"
#include "app/update.h"
#include "atproto/atproto.h"
#include "atproto/session.h"
#include "net/net.h"
#include "ui/imagecache.h"
#include "ui/popup.h"
#include "ui/imageview.h"
#include "util/log.h"
#include "util/paths.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Entries on the home menu; app_home.c checks its table against this. */
#define COBALT_HOME_MENU_COUNT 11

struct cobalt_app {
   cobalt_screen screen;
   cobalt_display_mode display;
   int selected;
   float focus[COBALT_HOME_MENU_COUNT];

   cobalt_signin signin;
   cobalt_update_view update;
   cobalt_entropy_view entropy;
   cobalt_timeline timeline;
   cobalt_thread_view thread;
   cobalt_compose compose;
   cobalt_notify_view notify;
   cobalt_profile_view profile;
   cobalt_graph_view graph;
   cobalt_search_view search;
   cobalt_lists_view lists;
   cobalt_popup popup;
   cobalt_screen popup_screen;
   bool popup_in_thread;
   cobalt_imageview imageview;
   /* Which row is highlighted on the account screen's small menu. */
   int account_selected;
   /* Which row is highlighted on the feed-picker screen's small menu. */
   int feeds_selected;
   int feeds_scroll;
   /* The shared feed window holds a custom feed, not the home timeline. */
   bool viewing_custom_feed;
   /* ...or the results of a post search, which also must not pass for home. */
   bool viewing_search;
   /* Where B from the profile screen returns to. */
   cobalt_screen profile_return;
   /* profile_return as it was when a followers/following list was opened. */
   cobalt_screen follows_profile_return;
   /* Where to return after composing — the timeline or the thread. */
   cobalt_screen compose_return;
   /* Where B from the thread screen returns to — the timeline or notifications. */
   cobalt_screen thread_return;
   /* Where B from a likes/reposts list returns to — the timeline or the
    * thread. The post it belongs to is already in app->graph.actor. */
   cobalt_screen likes_return;

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

/* app.c */
void cobalt_app_draw_header(cobalt_render *r, const char *subtitle);
void cobalt_app_draw_notice(const cobalt_app *app, cobalt_render *r, int y, int width);

/* app_home.c */
void cobalt_app_update_home(cobalt_app *app, const cobalt_input *in);
void cobalt_app_update_account(cobalt_app *app, const cobalt_input *in);
void cobalt_app_update_feeds(cobalt_app *app, const cobalt_input *in);
void cobalt_app_update_signin(cobalt_app *app, const cobalt_input *in);
void cobalt_app_draw_home_tv(cobalt_app *app, cobalt_render *r);
void cobalt_app_draw_home_drc(cobalt_app *app, cobalt_render *r);
void cobalt_app_draw_account(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface);
void cobalt_app_draw_feeds(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface);
void cobalt_app_draw_tv_idle(cobalt_render *r);

/* app_menus.c */
void cobalt_app_open_post_menu(cobalt_app *app, const cobalt_post *post, bool in_thread);
void cobalt_app_open_profile_menu(cobalt_app *app);
void cobalt_app_open_notification_menu(cobalt_app *app, const cobalt_notification *item);
void cobalt_app_popup_choose(cobalt_app *app, int index);

/* app_diag.c */
void cobalt_app_draw_diagnostics(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface);
