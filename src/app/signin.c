#include "app/signin.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define FIELD_SERVICE    0
#define FIELD_IDENTIFIER 1
#define FIELD_PASSWORD   2
#define ROW_SUBMIT       3

typedef struct {
   const char *label;
   const char *placeholder;
   bool masked;
} field_def;

static const field_def FIELDS[] = {
   { "Server",          "bsky.social",           false },
   { "Handle or email", "you.bsky.social",       false },
   { "App password",    "xxxx-xxxx-xxxx-xxxx",   true  },
};

static char *
field_buffer(cobalt_signin *s, int index, size_t *out_capacity)
{
   switch (index) {
      case FIELD_SERVICE:
         *out_capacity = sizeof(s->service);
         return s->service;
      case FIELD_IDENTIFIER:
         *out_capacity = sizeof(s->identifier);
         return s->identifier;
      case FIELD_PASSWORD:
         *out_capacity = sizeof(s->password);
         return s->password;
      default:
         *out_capacity = 0;
         return NULL;
   }
}

void
cobalt_signin_init(cobalt_signin *s)
{
   if (!s) {
      return;
   }
   memset(s, 0, sizeof(*s));
   s->editing = -1;
   /* Start on the identifier: the server field is prefilled for almost
    * everyone, and making people step past it every time is friction for no
    * reason. */
   s->focus = FIELD_IDENTIFIER;
}

void
cobalt_signin_clear_password(cobalt_signin *s)
{
   if (!s) {
      return;
   }
   /* Through a volatile pointer so the compiler cannot drop the store as dead.
    * The keyboard may still point at this buffer, which is fine — it is
    * re-opened against it on the next edit. */
   volatile char *p = s->password;
   for (size_t i = 0; i < sizeof(s->password); i++) {
      p[i] = '\0';
   }
}

void
cobalt_signin_set_status(cobalt_signin *s, const char *message, bool is_error)
{
   if (!s) {
      return;
   }
   snprintf(s->status, sizeof(s->status), "%s", message ? message : "");
   s->status_is_error = is_error;
}

/* --- input --- */

static void
begin_edit(cobalt_signin *s, int field)
{
   size_t capacity = 0;
   char *buffer = field_buffer(s, field, &capacity);
   if (!buffer) {
      return;
   }

   s->editing = field;
   cobalt_keyboard_open(&s->kb, buffer, capacity, FIELDS[field].masked);
   COBALT_LOGD("signin: editing field %d", field);
}

cobalt_signin_action
cobalt_signin_update(cobalt_signin *s, const cobalt_input *in)
{
   if (!s || !in) {
      return COBALT_SIGNIN_STAY;
   }

   /* A request is in flight: the screen is showing progress, and letting the
    * user edit fields that have already been handed to the worker would only
    * mislead them about what is being sent. */
   if (cobalt_session_busy()) {
      return COBALT_SIGNIN_STAY;
   }

   if (s->editing >= 0) {
      switch (cobalt_keyboard_update(&s->kb, in)) {
         case COBALT_KB_ACCEPTED:
            COBALT_LOGD("signin: field %d committed", s->editing);
            s->editing = -1;
            break;
         case COBALT_KB_CANCELLED:
            /* The buffer was edited in place, so cancelling keeps what was
             * typed rather than reverting. Backspace is on B for corrections;
             * a revert would need a snapshot per field for little gain. */
            s->editing = -1;
            break;
         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_SIGNIN_STAY;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_DOWN)) {
      s->focus = (s->focus + 1) % COBALT_SIGNIN_ROWS;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_UP)) {
      s->focus = (s->focus + COBALT_SIGNIN_ROWS - 1) % COBALT_SIGNIN_ROWS;
   }

   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_SIGNIN_BACK;
   }

   int activated = -1;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      activated = s->focus;
   } else if (s->hit_valid && in->touch_ended) {
      for (int i = 0; i < COBALT_SIGNIN_ROWS; i++) {
         if (cobalt_input_tapped(in, &s->hit[i])) {
            s->focus = i;
            activated = i;
            break;
         }
      }
   }

   if (activated < 0) {
      return COBALT_SIGNIN_STAY;
   }

   if (activated != ROW_SUBMIT) {
      begin_edit(s, activated);
      return COBALT_SIGNIN_STAY;
   }

   if (s->identifier[0] == '\0') {
      cobalt_signin_set_status(s, "Enter the handle or email for the account.", true);
      s->focus = FIELD_IDENTIFIER;
      return COBALT_SIGNIN_STAY;
   }
   if (s->password[0] == '\0') {
      cobalt_signin_set_status(s, "Enter an app password. Create one under "
                                  "Settings > App Passwords on Bluesky.", true);
      s->focus = FIELD_PASSWORD;
      return COBALT_SIGNIN_STAY;
   }

   return COBALT_SIGNIN_SUBMIT;
}

/* --- drawing --- */

static void
draw_header(cobalt_render *r, const char *title, const char *subtitle)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   cobalt_draw_text(r, COBALT_FONT_TITLE, title, m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   const int title_h = cobalt_font_line_height(r, COBALT_FONT_TITLE);
   SDL_Color dim = { 0xD8, 0xE6, 0xF4, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, subtitle, m->pad_edge,
                    m->pad_edge + title_h - m->line_gap, dim);
}

/* What a field shows when it has no value yet. */
static const char *
field_display(const cobalt_signin *s, int index, char *scratch, size_t scratch_size)
{
   size_t capacity = 0;
   const char *value = field_buffer((cobalt_signin *) s, index, &capacity);

   if (!value || value[0] == '\0') {
      return FIELDS[index].placeholder;
   }

   if (!FIELDS[index].masked) {
      return value;
   }

   /* Never draw a password, even the one the user just typed — the TV is a
    * shared screen and this app is meant to be used in a living room. */
   size_t chars = strlen(value);
   if (chars > scratch_size - 1) {
      chars = scratch_size - 1;
   }
   memset(scratch, '*', chars);
   scratch[chars] = '\0';
   return scratch;
}

static void
draw_status(const cobalt_signin *s, cobalt_render *r, int y)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   if (cobalt_session_busy()) {
      cobalt_draw_text(r, COBALT_FONT_BODY, "Signing in...",
                       m->pad_edge, y, COBALT_COLOUR_TILE);
      return;
   }

   if (s->status[0] == '\0') {
      return;
   }

   SDL_Color colour = s->status_is_error ? COBALT_COLOUR_ERROR : COBALT_COLOUR_TILE;
   cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, s->status,
                            m->pad_edge, y, m->width - 2 * m->pad_edge, 2, colour);
}

/* The whole-screen single-field view used while the keyboard is up. */
static void
draw_editing(cobalt_signin *s, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int field = s->editing;

   draw_header(r, FIELDS[field].label,
               field == FIELD_PASSWORD
                  ? "App password, not your account password"
                  : "A / touch: type     B: backspace");

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int value_h = m->font_body * 2;

   SDL_Rect value_rect = { m->pad_edge, top, m->width - 2 * m->pad_edge, value_h };
   cobalt_draw_tile(r, &value_rect, 1.0f);

   char shown[128];
   cobalt_keyboard_display_text(&s->kb, shown, sizeof(shown));
   const int text_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   cobalt_draw_text(r, COBALT_FONT_BODY, shown, value_rect.x + m->pad_tile,
                    value_rect.y + (value_h - text_h) / 2, COBALT_COLOUR_TEXT);

   SDL_Rect keys = {
      m->pad_edge,
      value_rect.y + value_h + m->gap,
      m->width - 2 * m->pad_edge,
      cobalt_keyboard_height(surface),
   };
   cobalt_keyboard_draw(&s->kb, r, surface, &keys);
}

static void
draw_fields(cobalt_signin *s, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const bool touchable = (surface == COBALT_SURFACE_DRC);

   draw_header(r, "Sign in", "App password — never your account password");

   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int row_w = m->width - 2 * m->pad_edge;
   const int row_gap = m->gap;

   /*
    * A field row stacks its label above its value, so the height comes from
    * the two fonts rather than a guess. The floor keeps the row a usable touch
    * target if the font failed to load and the line heights come back as zero.
    */
   const int label_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int value_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   int row_h = m->pad_tile + label_h + value_h;
   if (row_h < m->font_body * 2) {
      row_h = m->font_body * 2;
   }

   int y = top;
   for (int i = 0; i < ROW_SUBMIT; i++) {
      SDL_Rect row = { m->pad_edge, y, row_w, row_h };
      const bool focused = (s->focus == i);
      cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);

      cobalt_draw_text(r, COBALT_FONT_CAPTION, FIELDS[i].label,
                       row.x + m->pad_tile, row.y + m->pad_tile / 2,
                       COBALT_COLOUR_TEXT_DIM);

      char scratch[COBALT_PASSWORD_MAX];
      const char *shown = field_display(s, i, scratch, sizeof(scratch));

      size_t capacity = 0;
      const char *raw = field_buffer(s, i, &capacity);
      const bool empty = (!raw || raw[0] == '\0');

      cobalt_draw_text(r, COBALT_FONT_BODY, shown,
                       row.x + m->pad_tile,
                       row.y + m->pad_tile / 2 + label_h,
                       empty ? COBALT_COLOUR_TEXT_DIM : COBALT_COLOUR_TEXT);

      if (touchable) {
         s->hit[i] = row;
      }
      y += row_h + row_gap;
   }

   SDL_Rect submit = { m->pad_edge, y, row_w, row_h };
   cobalt_draw_tile(r, &submit, s->focus == ROW_SUBMIT ? 1.0f : 0.0f);
   const int submit_h = cobalt_font_line_height(r, COBALT_FONT_HEADING);
   cobalt_draw_text_centred(r, COBALT_FONT_HEADING, "Sign in", submit.x,
                            submit.y + (row_h - submit_h) / 2, submit.w,
                            COBALT_COLOUR_ACCENT);
   if (touchable) {
      s->hit[ROW_SUBMIT] = submit;
      s->hit_valid = true;
   }

   y += row_h + row_gap;
   /* Two lines, not three: the unavailable-build note below needs the space
    * more than a long error message does. */
   draw_status(s, r, y);

   /* If sign-in cannot work at all, say so here rather than letting someone
    * type a password into a build that was never going to send it. */
   const char *blocker = cobalt_session_blocker();
   if (blocker && !cobalt_session_busy()) {
      char note[COBALT_MESSAGE_MAX];
      snprintf(note, sizeof(note), "Sign-in is unavailable: %s. "
                                   "See Diagnostics.", blocker);
      cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, note, m->pad_edge,
                               m->height - m->pad_edge - 2 * m->font_caption,
                               row_w, 2, COBALT_COLOUR_ERROR);
   }
}

void
cobalt_signin_draw(cobalt_signin *s, cobalt_render *r, cobalt_surface_id surface)
{
   if (!s || !r) {
      return;
   }

   if (s->editing >= 0) {
      draw_editing(s, r, surface);
   } else {
      draw_fields(s, r, surface);
   }
}
