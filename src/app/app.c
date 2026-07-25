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
   ACTION_DIAGNOSTICS,
   ACTION_TOGGLE_DISPLAY,
   ACTION_QUIT,
} menu_action;

typedef struct {
   const char *label;
   const char *hint;
   menu_action action;
   bool enabled;
} menu_item;

/*
 * Build order (AGENTS.md §12) puts networking before any UI that depends on
 * it, so timeline and sign-in are present but explicitly disabled rather than
 * hidden — the shape of the app is visible on hardware from the first run
 * without pretending features exist.
 */
static const menu_item MENU[] = {
   { "Timeline",     "Your Bluesky home feed",             ACTION_NONE,           false },
   { "Sign in",      "Connect with an app password",       ACTION_NONE,           false },
   { "Diagnostics",  "Paths, network and library status",  ACTION_DIAGNOSTICS,    true  },
   { "TV display",   "Switch between TV+GamePad and Off-TV", ACTION_TOGGLE_DISPLAY, true },
   { "Quit",         "Return to the Wii U Menu",           ACTION_QUIT,           true  },
};

#define MENU_COUNT ((int) (sizeof(MENU) / sizeof(MENU[0])))

/* Focus animation, in units of "fraction of the way there per frame". */
#define FOCUS_RATE 0.25f

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

static void
activate(cobalt_app *app, int index)
{
   if (index < 0 || index >= MENU_COUNT || !MENU[index].enabled) {
      COBALT_LOGD("menu: entry %d is not selectable yet", index);
      return;
   }

   switch (MENU[index].action) {
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

   if (app->screen == COBALT_SCREEN_DIAGNOSTICS) {
      if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
         app->screen = COBALT_SCREEN_HOME;
      }
      return;
   }

   /* Home screen. Both axes move the selection so the same code serves the
    * TV's horizontal row and the GamePad's vertical list. */
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

      SDL_Color label = MENU[i].enabled ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      cobalt_draw_text_wrapped(r, COBALT_FONT_HEADING, MENU[i].label,
                               tile.x + m->pad_tile, tile.y + m->pad_tile,
                               tile.w - 2 * m->pad_tile, 2, label);

      if (!MENU[i].enabled) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, "Coming soon",
                          tile.x + m->pad_tile, tile.y + tile.h - m->pad_tile - 24,
                          COBALT_COLOUR_TEXT_DIM);
      }
   }

   /* Detail strip for the focused tile: the TV has room, so use it rather
    * than cramming the hint into the tile. */
   const int detail_y = top + tile_h + m->gap * 2;
   cobalt_draw_text(r, COBALT_FONT_BODY, MENU[app->selected].hint,
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

      SDL_Color label = MENU[i].enabled ? COBALT_COLOUR_TEXT : COBALT_COLOUR_TEXT_DIM;
      int text_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text(r, COBALT_FONT_BODY, MENU[i].label,
                       row.x + m->pad_tile, row.y + (row_h - text_h) / 2, label);

      int hint_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, MENU[i].hint, &hint_w, NULL);
      if (hint_w > 0 && hint_w < list_w / 2) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, MENU[i].hint,
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

   char lines[10][160];
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
