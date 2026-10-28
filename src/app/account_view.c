#include "app/account_view.h"

#include "app/app_internal.h"

/* Rows in draw order; Up/Down cycles this list. PROFILE is the identity panel
 * above the rows, and is selectable through that panel. */
typedef enum {
   ACCOUNT_ROW_PROFILE = 0,
   ACCOUNT_ROW_MUTED,
   ACCOUNT_ROW_BLOCKED,
   ACCOUNT_ROW_LANG,
   ACCOUNT_ROW_SIGN_OUT,
   ACCOUNT_ROW_COUNT,
} account_row;

/* Hit rectangles for the account screen's small menu, refreshed by every draw
 * so touch and the drawn layout cannot drift apart. */
static SDL_Rect s_account_hit[ACCOUNT_ROW_COUNT];
static bool s_account_hit_valid = false;

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
            cobalt_app_push_return(app, COBALT_SCREEN_ACCOUNT, NULL);
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
cobalt_app_draw_account(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_app_draw_header(r, "Account");

   const int top = cobalt_content_top(r);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   /* 44 on the GamePad, the same as a Home row and about 7mm on its 480-line
    * panel; these four were 34, the smallest touch targets in the app. Four of
    * them plus the panel and the hint strip only just fit, so the panel below
    * is sized to the lines it holds rather than to a guessed multiple. */
   const int row_h = m->font_body * 2 + (surface == COBALT_SURFACE_DRC ? 2 : 0);
   const int width = m->width - 2 * m->pad_edge;

   /* Two lines: who you are, then the DID and the server it lives on. The PDS
    * is the only detail that is not on the screen anywhere else and it fits
    * beside the DID, which is what keeps the panel short enough for four
    * fingertip-height rows above the hint strip. "A: view your profile" is
    * gone from here: the footer already says the panel opens. */
   SDL_Rect panel = { m->pad_edge, top, width,
                      m->pad_tile + heading_h + caption_h + m->pad_tile / 2 };
   cobalt_draw_tile(r, &panel, app->account_selected == ACCOUNT_ROW_PROFILE ? 1.0f : 0.0f);
   if (surface == COBALT_SURFACE_DRC) {
      s_account_hit[ACCOUNT_ROW_PROFILE] = panel;
   }

   char lines[2][COBALT_MESSAGE_MAX];
   snprintf(lines[0], sizeof(lines[0]), "%s", cobalt_session_handle());
   if (cobalt_session_service()[0]) {
      snprintf(lines[1], sizeof(lines[1]), "%s  %s", cobalt_session_did(),
               cobalt_session_service());
   } else {
      snprintf(lines[1], sizeof(lines[1]), "%s", cobalt_session_did());
   }

   const cobalt_font_id fonts[2] = { COBALT_FONT_HEADING, COBALT_FONT_CAPTION };
   const SDL_Color colours[2] = { COBALT_COLOUR_TEXT, COBALT_COLOUR_TEXT_DIM };

   int y = panel.y + m->pad_tile;
   for (int i = 0; i < 2; i++) {
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

   /* row_y already sits a half gap below the last row, so back off to the row's
    * bottom edge: the notice then ends above the hint strip, which is pinned to
    * the bottom of the screen. */
   cobalt_app_draw_notice(app, r, row_y - m->gap / 2, width);

   cobalt_draw_hints(r, "A / touch: open");
}
