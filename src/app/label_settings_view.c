#include "app/label_settings_view.h"

#include "app/app_internal.h"
#include "atproto/labels.h"
#include "atproto/session.h"

#include <string.h>

/* Row 0 is the adult-content switch; the rest are the labels a person can set.
 * Adult labels read as hidden while adult content is off, so they say so. */
typedef struct {
   const char *id;      /* the label identifier, or NULL for the adult switch */
   const char *name;
   bool adult;          /* only meaningful with adult content on */
} label_row;

static const label_row ROWS[] = {
   { NULL,            "Adult content", false },
   { "porn",          "Adult nudity and porn", true },
   { "sexual",        "Sexually suggestive", true },
   { "nudity",        "Non-sexual nudity", false },
   { "graphic-media", "Graphic media", true },
};
#define LABEL_ROW_COUNT ((int) (sizeof ROWS / sizeof ROWS[0]))

static int s_selected = 0;
static SDL_Rect s_hit[LABEL_ROW_COUNT];
static bool s_hit_valid = false;

void
cobalt_app_open_label_settings(void)
{
   s_selected = 0;
   s_hit_valid = false;
}

/* The next visibility in the cycle a person steps through. */
static const char *
next_visibility(const char *current)
{
   if (strcmp(current, "hide") == 0) {
      return "warn";
   }
   if (strcmp(current, "warn") == 0) {
      return "ignore";
   }
   return "hide";
}

static void
activate(cobalt_app *app, int row)
{
   const label_row *lr = &ROWS[row];

   if (cobalt_session_busy()) {
      return;
   }
   if (!lr->id) {
      cobalt_session_begin_label_pref("adult", cobalt_labels_adult_enabled() ? "off" : "on");
      return;
   }
   if (lr->adult && !cobalt_labels_adult_enabled()) {
      /* The engine hides adult labels whatever the setting says, so writing one
       * now would change nothing the person can see. */
      cobalt_app_set_notice(app, "Turn on adult content first.", true);
      return;
   }
   cobalt_session_begin_label_pref(lr->id,
                                   next_visibility(cobalt_labels_visibility(lr->id)));
}

void
cobalt_app_update_label_settings(cobalt_app *app, const cobalt_input *in)
{
   int activated = -1;

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      app->screen = COBALT_SCREEN_ACCOUNT;
      return;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP) && s_selected > 0) {
      s_selected--;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_DOWN) && s_selected < LABEL_ROW_COUNT - 1) {
      s_selected++;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      activated = s_selected;
   } else if (s_hit_valid && in->touch_ended) {
      for (int i = 0; i < LABEL_ROW_COUNT; i++) {
         if (cobalt_input_tapped(in, &s_hit[i])) {
            s_selected = i;
            activated = i;
            break;
         }
      }
   }
   if (activated >= 0) {
      activate(app, activated);
   }
}

void
cobalt_app_draw_label_settings(cobalt_app *app, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = cobalt_content_top(r);
   const int row_h = m->font_body * 2 + (surface == COBALT_SURFACE_DRC ? 2 : 0);
   const int width = m->width - 2 * m->pad_edge;
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);

   cobalt_draw_page_title(r, "Content labels",
                          "Choose what is hidden, covered or shown.");

   int row_y = top;
   for (int i = 0; i < LABEL_ROW_COUNT; i++) {
      const label_row *lr = &ROWS[i];
      SDL_Rect row = { m->pad_edge, row_y, width, row_h };
      const bool focused = (s_selected == i);
      const char *value;
      char value_buf[24];

      if (!lr->id) {
         value = cobalt_labels_adult_enabled() ? "On" : "Off";
      } else if (lr->adult && !cobalt_labels_adult_enabled()) {
         value = "Hidden (adult off)";
      } else {
         const char *v = cobalt_labels_visibility(lr->id);
         snprintf(value_buf, sizeof value_buf, "%s",
                  strcmp(v, "hide") == 0 ? "Hide"
                  : strcmp(v, "ignore") == 0 ? "Show" : "Warn");
         value = value_buf;
      }

      cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);
      cobalt_draw_text(r, COBALT_FONT_HEADING, lr->name, row.x + m->pad_tile,
                       row.y + (row_h - label_h) / 2, COBALT_COLOUR_TEXT);
      int value_w = 0;
      cobalt_text_size(r, COBALT_FONT_HEADING, value, &value_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_HEADING, value, row.x + row.w - m->pad_tile - value_w,
                       row.y + (row_h - label_h) / 2, COBALT_COLOUR_ACCENT_TEXT);

      if (surface == COBALT_SURFACE_DRC) {
         s_hit[i] = row;
      }
      row_y += row_h + m->gap / 2;
   }
   if (surface == COBALT_SURFACE_DRC) {
      s_hit_valid = true;
   }

   cobalt_app_draw_notice(app, r, row_y - m->gap / 2, width);
   cobalt_draw_hints(r, cobalt_session_busy() ? "Working..." : "A: change   B: back");
}
