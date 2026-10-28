#include "app/settings_view.h"

#include "app/app_internal.h"

#include <stdio.h>

/* Rows in draw order; Up/Down cycles this list. */
typedef enum {
   SETTINGS_ROW_THEME = 0,
   SETTINGS_ROW_TEXT_SCALE,
   SETTINGS_ROW_HIGH_CONTRAST,
   SETTINGS_ROW_RESET,
   SETTINGS_ROW_COUNT,
} settings_row;

/* The GamePad's row rectangles, refreshed by every draw so touch and the
 * drawn layout cannot drift apart. The TV records nothing: it has no touch. */
static SDL_Rect s_hit[SETTINGS_ROW_COUNT];
static bool s_hit_valid = false;

/* The palette changes before the next frame, so the switch is visible
 * immediately; the file is what makes it survive a restart. */
static void
apply_palette(cobalt_app *app)
{
   cobalt_theme_apply(app->settings.theme);
   cobalt_theme_set_high_contrast(app->settings.high_contrast);
}

static void
save(cobalt_app *app, const char *saved)
{
   char path[256];

   if (!cobalt_data_path(path, sizeof(path), "settings.dat") ||
       !cobalt_settings_save(path, &app->settings)) {
      snprintf(app->notice, sizeof(app->notice),
               "Setting changed, but could not be saved to the SD card.");
      app->notice_is_error = true;
   } else {
      snprintf(app->notice, sizeof(app->notice), "%s", saved);
      app->notice_is_error = false;
   }
}

static const char *
text_scale_value(cobalt_text_scale scale)
{
   switch (scale) {
      case COBALT_TEXT_SCALE_SMALL:  return "100%";
      case COBALT_TEXT_SCALE_LARGE:  return "130%";
      case COBALT_TEXT_SCALE_NORMAL:
      default:                       return "115%";
   }
}

/* Apply and persist one row. The notice names what changed; a failed write
 * still leaves the new value in memory for this run. */
static void
toggle_row(cobalt_app *app, int row)
{
   char saved[96];

   switch (row) {
      case SETTINGS_ROW_THEME:
         app->settings.theme = app->settings.theme == COBALT_THEME_DARK
                                  ? COBALT_THEME_LIGHT
                                  : COBALT_THEME_DARK;
         apply_palette(app);
         COBALT_LOGI("settings: theme -> %s",
                     app->settings.theme == COBALT_THEME_DARK ? "dark" : "light");
         snprintf(saved, sizeof(saved), "%s theme.",
                  app->settings.theme == COBALT_THEME_DARK ? "Dark" : "Light");
         break;

      case SETTINGS_ROW_TEXT_SCALE:
         if (app->settings.text_scale == COBALT_TEXT_SCALE_SMALL) {
            app->settings.text_scale = COBALT_TEXT_SCALE_NORMAL;
         } else if (app->settings.text_scale == COBALT_TEXT_SCALE_NORMAL) {
            app->settings.text_scale = COBALT_TEXT_SCALE_LARGE;
         } else {
            app->settings.text_scale = COBALT_TEXT_SCALE_SMALL;
         }
         COBALT_LOGI("settings: text scale -> %d%%",
                     (int) app->settings.text_scale);
         snprintf(saved, sizeof(saved), "Text scale: %s.",
                  text_scale_value(app->settings.text_scale));
         break;

      case SETTINGS_ROW_HIGH_CONTRAST:
         app->settings.high_contrast = !app->settings.high_contrast;
         apply_palette(app);
         COBALT_LOGI("settings: high contrast -> %s",
                     app->settings.high_contrast ? "on" : "off");
         snprintf(saved, sizeof(saved), "High contrast %s.",
                  app->settings.high_contrast ? "on" : "off");
         break;

      case SETTINGS_ROW_RESET:
         cobalt_settings_defaults(&app->settings);
         apply_palette(app);
         COBALT_LOGI("settings: reset to defaults");
         snprintf(saved, sizeof(saved), "Settings reset to defaults.");
         break;

      default:
         return;
   }

   save(app, saved);
}

void
cobalt_settings_view_update(cobalt_app *app, const cobalt_input *in)
{
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      app->screen = COBALT_SCREEN_HOME;
      return;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      app->settings_selected =
         (app->settings_selected + 1) % SETTINGS_ROW_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      app->settings_selected =
         (app->settings_selected + SETTINGS_ROW_COUNT - 1) % SETTINGS_ROW_COUNT;
   }

   int activated = -1;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      activated = app->settings_selected;
   } else if (s_hit_valid && in->touch_ended) {
      for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
         if (cobalt_input_tapped(in, &s_hit[i])) {
            app->settings_selected = i;
            activated = i;
            break;
         }
      }
   }

   if (activated >= 0) {
      toggle_row(app, activated);
   }
}

void
cobalt_settings_view_draw(cobalt_app *app, cobalt_render *r,
                          cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_app_draw_header(r, "Settings");

   const int top = cobalt_content_top(r);
   const int width = m->width - 2 * m->pad_edge;
   /* The same fingertip-height row as the Account and Feeds screens. */
   const int row_h = m->font_body * 2 + (surface == COBALT_SURFACE_DRC ? 2 : 0);
   const int heading_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const char *const LABELS[SETTINGS_ROW_COUNT] = {
      "Theme", "Text scale", "High contrast", "Reset to defaults",
   };
   const char *const VALUES[SETTINGS_ROW_COUNT] = {
      app->settings.theme == COBALT_THEME_DARK ? "Dark" : "Light",
      text_scale_value(app->settings.text_scale),
      app->settings.high_contrast ? "On" : "Off",
      "Reset",
   };

   int y = top;
   for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
      SDL_Rect row = { m->pad_edge, y, width, row_h };
      const bool focused = (app->settings_selected == i);

      cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);
      cobalt_draw_text(r, COBALT_FONT_HEADING, LABELS[i], row.x + m->pad_tile,
                       row.y + (row_h - heading_h) / 2, COBALT_COLOUR_TEXT);

      int value_w = 0;
      cobalt_text_size(r, COBALT_FONT_HEADING, VALUES[i], &value_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_HEADING, VALUES[i],
                       row.x + row.w - m->pad_tile - value_w,
                       row.y + (row_h - heading_h) / 2, COBALT_COLOUR_ACCENT_TEXT);

      if (surface == COBALT_SURFACE_DRC) {
         s_hit[i] = row;
      }
      y += row_h + m->gap / 2;
   }
   if (surface == COBALT_SURFACE_DRC) {
      s_hit_valid = true;
   }

   y += m->gap / 2;
   y += cobalt_draw_text_wrapped(
      r, COBALT_FONT_BODY,
      "Settings apply to both screens and save as you change them.",
      m->pad_edge, y, width, 2, COBALT_COLOUR_TEXT_DIM);
   y += caption_h / 2;

   cobalt_app_draw_notice(app, r, y, width);
   cobalt_draw_hints(r, "A / touch: change   B: back   Up/Down: move");
}
