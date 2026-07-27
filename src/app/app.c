#include "app/app.h"
#include "atproto/atproto.h"
#include "net/net.h"
#include "util/log.h"
#include "util/paths.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
   ACTION_NONE = 0,       /* placeholder entry, not yet implemented */
   ACTION_TIMELINE,
   ACTION_SIGNIN,
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
   ACTION_QUIT,
} menu_action;

typedef struct {
   const char *label;
   const char *hint;
   menu_action action;
   bool enabled;   /* static default; see menu_enabled() for entries that depend on state */
} menu_item;

/*
 * Timeline and Sign in are now wired up (see atproto.c), but their enabled
 * state depends on runtime state (session present, SDK available) rather than
 * being fixed at compile time — see menu_enabled() below. The `enabled` field
 * here is only the fallback for entries that don't depend on state.
 */
static const menu_item MENU[] = {
   { "Timeline",     "Your Bluesky home feed",             ACTION_TIMELINE,       false },
   { "Sign in",      "Connect with an app password",       ACTION_SIGNIN,         false },
   { "Diagnostics",  "Paths, network and library status",  ACTION_DIAGNOSTICS,    true  },
   { "TV display",   "Switch between TV+GamePad and Off-TV", ACTION_TOGGLE_DISPLAY, true },
   { "Quit",         "Return to the Wii U Menu",           ACTION_QUIT,           true  },
};

#define MENU_COUNT ((int) (sizeof(MENU) / sizeof(MENU[0])))

/* Focus animation, in units of "fraction of the way there per frame". */
#define FOCUS_RATE 0.25f

/* Sign-in form fields, in tab order. */
typedef enum {
   SIGNIN_FIELD_IDENTIFIER = 0,
   SIGNIN_FIELD_PASSWORD,
   SIGNIN_FIELD_SUBMIT,
   SIGNIN_FIELD_COUNT,
} signin_field;

#define SIGNIN_BUF_SIZE 128
#define ERROR_BUF_SIZE 128

struct cobalt_app {
   cobalt_screen screen;
   cobalt_display_mode display;
   int selected;
   float focus[MENU_COUNT];

   bool quit;
   uint32_t frames;
   uint32_t started_ms;

   /* Cached once — curl_version() returns a static string but formatting it
    * every frame would allocate in the render loop. */
   char curl_version[64];
   char sdl_version[32];

   /*
    * Sign-in screen. cobalt_atproto_login() blocks on the network, so a
    * confirm on the Submit field only sets signin_pending_login — the actual
    * call happens at the top of the *next* cobalt_app_update(), by which time
    * this frame's "Signing in..." draw has already been presented once.
    */
   signin_field signin_focus;
   bool signin_editing;
   char signin_identifier[SIGNIN_BUF_SIZE];
   char signin_password[SIGNIN_BUF_SIZE];
   bool signin_submitting;
   bool signin_pending_login;
   char signin_error[ERROR_BUF_SIZE];

   /* Timeline screen. Same deferred-call pattern as sign-in. */
   cobalt_feed_post timeline_posts[COBALT_TIMELINE_MAX_POSTS];
   int timeline_count;
   int timeline_scroll;
   bool timeline_loading;
   bool timeline_pending_fetch;
   char timeline_error[ERROR_BUF_SIZE];
};

/* Hit rectangles for the GamePad list, recomputed on draw so touch and the
 * drawn layout can never drift apart. */
static SDL_Rect s_drc_hit[MENU_COUNT];
static bool s_drc_hit_valid = false;

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
   app->selected = 2; /* Diagnostics — the only thing worth opening on run one. */
   app->started_ms = SDL_GetTicks();

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
   free(app);
}

bool
cobalt_app_should_quit(const cobalt_app *app)
{
   return app && app->quit;
}

/* Entries whose availability depends on runtime state rather than being
 * fixed at compile time — see the comment on MENU's `enabled` field. */
static bool
menu_enabled(int index)
{
   switch (MENU[index].action) {
      case ACTION_TIMELINE:
         return cobalt_atproto_has_session();

      case ACTION_SIGNIN: {
         cobalt_atproto_status st = cobalt_atproto_get_status();
         return st != COBALT_ATPROTO_ABSENT && st != COBALT_ATPROTO_ERROR;
      }

      default:
         return MENU[index].enabled;
   }
}

/* Returns a pointer valid only until the next call — `buf` is the backing
 * storage, sized by the caller, so this never allocates. */
static const char *
menu_label(int index, char *buf, size_t buf_size)
{
   if (MENU[index].action == ACTION_SIGNIN && cobalt_atproto_has_session()) {
      snprintf(buf, buf_size, "Sign out");
      return buf;
   }
   return MENU[index].label;
}

static const char *
menu_hint(int index, char *buf, size_t buf_size)
{
   if (MENU[index].action == ACTION_SIGNIN && cobalt_atproto_has_session()) {
      const char *handle = cobalt_atproto_session_handle();
      snprintf(buf, buf_size, "Signed in as %s", handle ? handle : "?");
      return buf;
   }
   if (MENU[index].action == ACTION_TIMELINE && !cobalt_atproto_has_session()) {
      return "Sign in first";
   }
   return MENU[index].hint;
}

static void
signin_reset(cobalt_app *app)
{
   app->signin_focus = SIGNIN_FIELD_IDENTIFIER;
   app->signin_editing = false;
   app->signin_identifier[0] = '\0';
   app->signin_password[0] = '\0';
   app->signin_error[0] = '\0';
   app->signin_submitting = false;
   app->signin_pending_login = false;
}

static void
activate(cobalt_app *app, int index)
{
   if (index < 0 || index >= MENU_COUNT || !menu_enabled(index)) {
      COBALT_LOGD("menu: entry %d is not selectable yet", index);
      return;
   }

   switch (MENU[index].action) {
      case ACTION_TIMELINE:
         app->screen = COBALT_SCREEN_TIMELINE;
         app->timeline_count = 0;
         app->timeline_scroll = 0;
         app->timeline_error[0] = '\0';
         app->timeline_loading = true;
         app->timeline_pending_fetch = true;
         COBALT_LOGI("menu: opened timeline");
         break;

      case ACTION_SIGNIN:
         if (cobalt_atproto_has_session()) {
            cobalt_atproto_logout();
            COBALT_LOGI("menu: signed out");
         } else {
            app->screen = COBALT_SCREEN_SIGNIN;
            signin_reset(app);
            COBALT_LOGI("menu: opened sign-in");
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

      case ACTION_NONE:
      default:
         break;
   }
}

/* Trims one codepoint off the end of a UTF-8 buffer — skips continuation
 * bytes (10xxxxxx) so a multi-byte character never gets left half-erased. */
static void
utf8_backspace(char *buf)
{
   size_t len = strlen(buf);
   if (len == 0) {
      return;
   }
   len--;
   while (len > 0 && (buf[len] & 0xC0) == 0x80) {
      len--;
   }
   buf[len] = '\0';
}

static char *
signin_active_buffer(cobalt_app *app)
{
   switch (app->signin_focus) {
      case SIGNIN_FIELD_IDENTIFIER: return app->signin_identifier;
      case SIGNIN_FIELD_PASSWORD:   return app->signin_password;
      default:                      return NULL;
   }
}

static void
perform_login(cobalt_app *app)
{
   cobalt_login_result result = cobalt_atproto_login(app->signin_identifier,
                                                      app->signin_password);
   app->signin_submitting = false;

   switch (result) {
      case COBALT_LOGIN_OK:
         /* Wipe the password from app memory now that Wolfram/curl have made
          * their own copies internally — nothing here needs it again. */
         memset(app->signin_password, 0, sizeof(app->signin_password));
         app->screen = COBALT_SCREEN_HOME;
         COBALT_LOGI("app: sign-in succeeded");
         break;

      case COBALT_LOGIN_BAD_CREDENTIALS:
         snprintf(app->signin_error, sizeof(app->signin_error),
                  "Incorrect identifier or app password.");
         break;

      case COBALT_LOGIN_NETWORK_ERROR:
         snprintf(app->signin_error, sizeof(app->signin_error),
                  "No network connection.");
         break;

      case COBALT_LOGIN_UNAVAILABLE:
      default:
         snprintf(app->signin_error, sizeof(app->signin_error),
                  "ATProto support is not available in this build.");
         break;
   }
}

static void
perform_timeline_fetch(cobalt_app *app)
{
   int count = 0;
   cobalt_timeline_result result =
      cobalt_atproto_fetch_timeline(app->timeline_posts, COBALT_TIMELINE_MAX_POSTS, &count);
   app->timeline_loading = false;
   app->timeline_count = count;

   switch (result) {
      case COBALT_TIMELINE_OK:
         COBALT_LOGI("app: fetched %d timeline posts", count);
         break;

      case COBALT_TIMELINE_NOT_SIGNED_IN:
         snprintf(app->timeline_error, sizeof(app->timeline_error),
                  "Not signed in.");
         break;

      case COBALT_TIMELINE_NETWORK_ERROR:
         snprintf(app->timeline_error, sizeof(app->timeline_error),
                  "No network connection.");
         break;

      case COBALT_TIMELINE_UNAVAILABLE:
      default:
         snprintf(app->timeline_error, sizeof(app->timeline_error),
                  "ATProto support is not available in this build.");
         break;
   }
}

static void
update_home(cobalt_app *app, const cobalt_input *in)
{
   /* Both axes move the selection so the same code serves the TV's
    * horizontal row and the GamePad's vertical list. */
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

   /* Ease focus toward the selection so tiles settle rather than snap. */
   for (int i = 0; i < MENU_COUNT; i++) {
      float target = (i == app->selected) ? 1.0f : 0.0f;
      app->focus[i] += (target - app->focus[i]) * FOCUS_RATE;
   }
}

static void
update_signin(cobalt_app *app, const cobalt_input *in)
{
   if (app->signin_submitting) {
      /* A login attempt is already in flight (deferred to the top of this
       * function on the next update) — ignore input until it resolves. */
      return;
   }

   if (app->signin_editing) {
      char *field = signin_active_buffer(app);
      if (field) {
         size_t used = strlen(field);
         size_t room = SIGNIN_BUF_SIZE - used - 1;
         if (in->text[0] && room > 0) {
            strncat(field, in->text, room);
         }
         if (in->backspace) {
            utf8_backspace(field);
         }
      }

      /* Either the on-screen keyboard's own "done" or the confirm button
       * ends editing this field — not verified on hardware which one the Wii
       * U SDL port actually delivers here, see cobalt_input's text-entry note. */
      if (in->text_confirmed || cobalt_input_pressed(in, COBALT_BTN_CONFIRM) ||
          cobalt_input_pressed(in, COBALT_BTN_BACK)) {
         cobalt_input_stop_text_edit();
         app->signin_editing = false;
      }
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      app->screen = COBALT_SCREEN_HOME;
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      app->signin_focus = (signin_field) ((app->signin_focus + 1) % SIGNIN_FIELD_COUNT);
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      app->signin_focus =
         (signin_field) ((app->signin_focus + SIGNIN_FIELD_COUNT - 1) % SIGNIN_FIELD_COUNT);
   }

   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      if (app->signin_focus == SIGNIN_FIELD_SUBMIT) {
         if (app->signin_identifier[0] && app->signin_password[0]) {
            app->signin_error[0] = '\0';
            app->signin_submitting = true;
            app->signin_pending_login = true;
         } else {
            snprintf(app->signin_error, sizeof(app->signin_error),
                     "Enter both an identifier and an app password.");
         }
      } else {
         cobalt_input_start_text_edit();
         app->signin_editing = true;
      }
   }
}

static void
update_timeline(cobalt_app *app, const cobalt_input *in)
{
   if (app->timeline_loading) {
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      app->screen = COBALT_SCREEN_HOME;
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && app->timeline_scroll < app->timeline_count - 1) {
      app->timeline_scroll++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && app->timeline_scroll > 0) {
      app->timeline_scroll--;
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

   /*
    * Deferred blocking calls. These run at the *start* of the update that
    * follows the frame which requested them (activate() / update_signin()
    * only set the pending flag), so the "Signing in..."/"Loading..." screen
    * drawn for that prior frame has already been presented once before the
    * network call blocks — see the struct comment on signin_pending_login.
    */
   if (app->signin_pending_login) {
      app->signin_pending_login = false;
      perform_login(app);
   }
   if (app->timeline_pending_fetch) {
      app->timeline_pending_fetch = false;
      perform_timeline_fetch(app);
   }

   switch (app->screen) {
      case COBALT_SCREEN_DIAGNOSTICS:
         if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
            app->screen = COBALT_SCREEN_HOME;
         }
         break;

      case COBALT_SCREEN_SIGNIN:
         update_signin(app, in);
         break;

      case COBALT_SCREEN_TIMELINE:
         update_timeline(app, in);
         break;

      case COBALT_SCREEN_HOME:
      default:
         update_home(app, in);
         break;
   }
}

/* --- drawing --- */

static void
draw_header(cobalt_app *app, cobalt_render *r, const char *subtitle)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   cobalt_draw_text(r, COBALT_FONT_TITLE, "Cobalt", m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   int title_h = cobalt_font_line_height(r, COBALT_FONT_TITLE);
   SDL_Color dim = { 0xD8, 0xE6, 0xF4, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                    m->pad_edge + title_h - m->line_gap, dim);
   (void) app;
}

static void
draw_home_tv(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(app, r, "AT Protocol for Wii U");

   /* A single row of large tiles, Wii U menu style — few, big, readable from
    * across a room rather than a dense list. */
   const int top = m->pad_edge + 130;
   const int tile_h = 210;
   const int total_gap = m->gap * (MENU_COUNT - 1);
   const int tile_w = (m->width - 2 * m->pad_edge - total_gap) / MENU_COUNT;

   for (int i = 0; i < MENU_COUNT; i++) {
      SDL_Rect tile = { m->pad_edge + i * (tile_w + m->gap), top, tile_w, tile_h };
      cobalt_draw_tile(r, &tile, app->focus[i]);

      bool enabled = menu_enabled(i);
      char label_buf[32];
      SDL_Color label_colour = enabled ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      cobalt_draw_text_wrapped(r, COBALT_FONT_HEADING, menu_label(i, label_buf, sizeof(label_buf)),
                               tile.x + m->pad_tile, tile.y + m->pad_tile,
                               tile.w - 2 * m->pad_tile, 2, label_colour);

      if (!enabled) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, "Not available yet",
                          tile.x + m->pad_tile, tile.y + tile.h - m->pad_tile - 24,
                          COBALT_COLOUR_TEXT_DIM);
      }
   }

   /* Detail strip for the focused tile: the TV has room, so use it rather
    * than cramming the hint into the tile. */
   char hint_buf[64];
   const int detail_y = top + tile_h + m->gap * 2;
   cobalt_draw_text(r, COBALT_FONT_BODY, menu_hint(app->selected, hint_buf, sizeof(hint_buf)),
                    m->pad_edge, detail_y, COBALT_COLOUR_TILE);

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    "A / touch: select     B: back     D-pad or stick: move",
                    m->pad_edge, m->height - m->pad_edge - 24, hint);
}

static void
draw_home_drc(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(app, r, "Off-TV ready");

   /* A vertical list: denser than the TV row, and every row is a touch target
    * comfortably larger than a fingertip. */
   const int top = m->pad_edge + 62;
   const int row_h = 54;
   const int list_w = m->width - 2 * m->pad_edge;

   for (int i = 0; i < MENU_COUNT; i++) {
      SDL_Rect row = { m->pad_edge, top + i * (row_h + m->gap / 2), list_w, row_h };
      s_drc_hit[i] = row;

      cobalt_draw_tile(r, &row, app->focus[i]);

      char label_buf[32];
      const char *label_text = menu_label(i, label_buf, sizeof(label_buf));
      SDL_Color label_colour = menu_enabled(i) ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      int text_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text(r, COBALT_FONT_BODY, label_text,
                       row.x + m->pad_tile, row.y + (row_h - text_h) / 2, label_colour);

      char hint_buf[64];
      const char *hint_text = menu_hint(i, hint_buf, sizeof(hint_buf));
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

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    app->display == COBALT_DISPLAY_DUAL ? "TV + GamePad" : "GamePad only",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

/* The TV's idle card in Off-TV mode: enough to show the app is alive and
 * where to look, without duplicating a UI nobody is watching. */
static void
draw_tv_idle(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   (void) app;

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

static void
draw_diagnostics(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(app, r, "Diagnostics");

   /*
    * This screen exists because there is no emulator (AGENTS.md §10). It is
    * the fastest way to confirm, on the console itself, that asset paths
    * resolved, the network came up and the TLS stack linked — the three things
    * that block everything downstream.
    */
   const cobalt_net_status net = cobalt_net_get_status();

   char lines[11][160];
   int count = 0;

   snprintf(lines[count++], sizeof(lines[0]), "Surface: %s (%dx%d)",
            surface == COBALT_SURFACE_DRC ? "GamePad" : "TV", m->width, m->height);
   snprintf(lines[count++], sizeof(lines[0]), "Content: %s",
            cobalt_content_root() ? cobalt_content_root() : "NOT FOUND");
   snprintf(lines[count++], sizeof(lines[0]), "Data: %s",
            cobalt_data_root() ? cobalt_data_root() : "NOT FOUND");
   snprintf(lines[count++], sizeof(lines[0]), "Network: %s (%s)",
            cobalt_net_status_string(net), cobalt_net_local_address());
   snprintf(lines[count++], sizeof(lines[0]), "%s", app->sdl_version);
   snprintf(lines[count++], sizeof(lines[0]), "%s", app->curl_version);
   snprintf(lines[count++], sizeof(lines[0]), "ATProto SDK: %s",
            cobalt_atproto_sdk_version());
   snprintf(lines[count++], sizeof(lines[0]), "SDK status: %s",
            cobalt_atproto_status_string());
   snprintf(lines[count++], sizeof(lines[0]), "Session: %s",
            cobalt_atproto_has_session() ? cobalt_atproto_session_handle() : "signed out");
   snprintf(lines[count++], sizeof(lines[0]), "Font: %s",
            cobalt_render_has_font(r) ? "loaded" : "MISSING");
   snprintf(lines[count++], sizeof(lines[0]), "Frames: %u", (unsigned) app->frames);

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   SDL_Rect panel = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                      m->height - top - m->pad_edge - 40 };
   cobalt_draw_tile(r, &panel, 0.0f);

   const int line_h = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->line_gap;
   for (int i = 0; i < count; i++) {
      bool bad = (strstr(lines[i], "NOT FOUND") != NULL) ||
                 (strstr(lines[i], "MISSING") != NULL) ||
                 (net != COBALT_NET_UP && strncmp(lines[i], "Network:", 8) == 0);

      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, lines[i],
                               panel.x + m->pad_tile, panel.y + m->pad_tile + i * line_h,
                               panel.w - 2 * m->pad_tile, 1,
                               bad ? COBALT_COLOUR_ERROR : COBALT_COLOUR_TEXT);
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, "B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

/*
 * One layout for both surfaces — unlike Home's TV-row/GamePad-list split, a
 * sign-in form is naturally vertical on either screen; the metrics struct
 * already gives each surface its own width and type scale.
 *
 * Touch is not wired up for the fields themselves yet (AGENTS.md §5 wants
 * every screen usable both ways) — only D-pad/stick navigation plus the
 * confirm button works here so far. Follow draw_home_drc's s_drc_hit pattern
 * to add tap-to-focus for the three rows when picking this back up.
 */
static void
draw_signin(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(app, r, "Sign in with an app password");

   const int top = m->pad_edge + 130;
   const int row_h = 64;
   const int form_w = m->width - 2 * m->pad_edge;

   struct {
      const char *placeholder;
      char *value;
      bool mask;
   } fields[2] = {
      { "Identifier (handle or email)", app->signin_identifier, false },
      { "App password", app->signin_password, true },
   };

   for (int i = 0; i < 2; i++) {
      SDL_Rect row = { m->pad_edge, top + i * (row_h + m->gap), form_w, row_h };
      bool focused = ((int) app->signin_focus == i);
      cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);

      char shown[SIGNIN_BUF_SIZE + 2];
      bool has_value = fields[i].value[0] != '\0';
      if (has_value) {
         if (fields[i].mask) {
            size_t n = strlen(fields[i].value);
            if (n > sizeof(shown) - 2) {
               n = sizeof(shown) - 2;
            }
            memset(shown, '*', n);
            shown[n] = '\0';
         } else {
            snprintf(shown, sizeof(shown), "%s", fields[i].value);
         }
      } else {
         shown[0] = '\0';
      }

      bool editing_this = focused && app->signin_editing;
      if (editing_this) {
         strncat(shown, "|", sizeof(shown) - strlen(shown) - 1);
      } else if (!has_value) {
         snprintf(shown, sizeof(shown), "%s", fields[i].placeholder);
      }

      SDL_Color colour = has_value ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      int text_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text(r, COBALT_FONT_BODY, shown,
                       row.x + m->pad_tile, row.y + (row_h - text_h) / 2, colour);
   }

   SDL_Rect submit = { m->pad_edge, top + 2 * (row_h + m->gap), form_w, row_h };
   bool submit_focused = (app->signin_focus == SIGNIN_FIELD_SUBMIT);
   cobalt_draw_tile(r, &submit, submit_focused ? 1.0f : 0.0f);
   int submit_text_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   cobalt_draw_text_centred(r, COBALT_FONT_HEADING,
                            app->signin_submitting ? "Signing in..." : "Sign in",
                            submit.x, submit.y + (row_h - submit_text_h) / 2, submit.w,
                            COBALT_COLOUR_TEXT);

   if (app->signin_error[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, app->signin_error,
                               m->pad_edge, top + 3 * (row_h + m->gap) + m->gap,
                               form_w, 2, COBALT_COLOUR_ERROR);
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    app->signin_editing ? "A / Enter: done typing"
                                        : "A: edit / confirm     B: back     Up/Down: move",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

static void
draw_timeline(cobalt_app *app, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   draw_header(app, r, "Your Bluesky home feed");

   const int top = m->pad_edge + 130;
   SDL_Rect panel = { m->pad_edge, top, m->width - 2 * m->pad_edge,
                      m->height - top - m->pad_edge - 40 };
   cobalt_draw_tile(r, &panel, 0.0f);

   if (app->timeline_loading) {
      cobalt_draw_text_centred(r, COBALT_FONT_HEADING, "Loading...",
                               panel.x, panel.y + panel.h / 2 - 16, panel.w,
                               COBALT_COLOUR_TEXT);
   } else if (app->timeline_error[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, app->timeline_error,
                               panel.x + m->pad_tile, panel.y + m->pad_tile,
                               panel.w - 2 * m->pad_tile, 3, COBALT_COLOUR_ERROR);
   } else if (app->timeline_count == 0) {
      cobalt_draw_text_centred(r, COBALT_FONT_BODY, "No posts to show",
                               panel.x, panel.y + panel.h / 2 - 12, panel.w,
                               COBALT_COLOUR_TEXT_DIM);
   } else {
      const int row_h = 96;
      int visible = panel.h / row_h;
      if (visible < 1) {
         visible = 1;
      }

      for (int row = 0; row < visible && app->timeline_scroll + row < app->timeline_count; row++) {
         const cobalt_feed_post *post = &app->timeline_posts[app->timeline_scroll + row];
         int y = panel.y + m->pad_tile + row * row_h;

         const char *author = post->author_display_name[0] ? post->author_display_name
                                                             : post->author_handle;
         cobalt_draw_text(r, COBALT_FONT_BODY, author, panel.x + m->pad_tile, y,
                          COBALT_COLOUR_TEXT);
         cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, post->text,
                                  panel.x + m->pad_tile,
                                  y + cobalt_font_line_height(r, COBALT_FONT_BODY),
                                  panel.w - 2 * m->pad_tile, 2, COBALT_COLOUR_TEXT_DIM);
      }
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, "B: back     Up/Down: scroll",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

void
cobalt_app_draw(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   if (!app || !r) {
      return;
   }

   if (surface == COBALT_SURFACE_TV && app->display == COBALT_DISPLAY_GAMEPAD) {
      draw_tv_idle(app, r);
      return;
   }

   switch (app->screen) {
      case COBALT_SCREEN_DIAGNOSTICS:
         draw_diagnostics(app, r, surface);
         break;

      case COBALT_SCREEN_SIGNIN:
         draw_signin(app, r);
         break;

      case COBALT_SCREEN_TIMELINE:
         draw_timeline(app, r);
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
}
