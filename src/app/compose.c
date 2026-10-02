#include "app/compose.h"
#include "atproto/session.h"
#include "util/log.h"
#include "util/paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIRM_POST    0
#define CONFIRM_EDIT    1
#define CONFIRM_DISCARD 2
#define CONFIRM_IMAGE   3
#define CONFIRM_COUNT   4

/* Stored in confirm_choice as these ids; the row shows them in this order,
 * minus IMAGE on a reply. */
static const int CONFIRM_ORDER[CONFIRM_COUNT] = {
   CONFIRM_POST, CONFIRM_IMAGE, CONFIRM_EDIT, CONFIRM_DISCARD
};

static int
confirm_ids(const cobalt_compose *compose, int out[CONFIRM_COUNT])
{
   int n = 0;
   for (int i = 0; i < CONFIRM_COUNT; i++) {
      if (CONFIRM_ORDER[i] == CONFIRM_IMAGE && cobalt_compose_is_reply(compose)) {
         continue;
      }
      out[n++] = CONFIRM_ORDER[i];
   }
   return n;
}

static const char *
confirm_label(const cobalt_compose *compose, int id)
{
   switch (id) {
      case CONFIRM_POST:    return "Post";
      case CONFIRM_IMAGE:   return compose->attach_path[0] ? "Remove image"
                                                            : "Add image";
      case CONFIRM_EDIT:    return "Keep editing";
      default:              return "Discard";
   }
}

static int
name_cmp(const void *a, const void *b)
{
   return strcmp((const char *) a, (const char *) b);
}

int
cobalt_compose_scan_images(const char *dir, char names[][COBALT_PICKER_NAME_MAX],
                           int max)
{
   DIR *d = dir ? opendir(dir) : NULL;
   if (!d) {
      return 0;
   }
   int n = 0;
   struct dirent *e;
   while (n < max && (e = readdir(d)) != NULL) {
      if (e->d_name[0] == '.' || !cobalt_attach_mime(e->d_name) ||
          strlen(e->d_name) >= COBALT_PICKER_NAME_MAX) {
         continue;
      }
      char path[COBALT_ATTACH_PATH_MAX];
      if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >=
          (int) sizeof(path)) {
         continue;
      }
      struct stat st;
      if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
          st.st_size > COBALT_ATTACH_MAX_BYTES) {
         continue;
      }
      snprintf(names[n++], COBALT_PICKER_NAME_MAX, "%s", e->d_name);
   }
   closedir(d);
   qsort(names, (size_t) n, COBALT_PICKER_NAME_MAX, name_cmp);
   return n;
}

void
cobalt_compose_open_picker(cobalt_compose *compose, const char *dir)
{
   if (!compose || !dir) {
      return;
   }
   snprintf(compose->picker_dir, sizeof(compose->picker_dir), "%s", dir);
   compose->picker_count = cobalt_compose_scan_images(
      dir, compose->picker_names, COBALT_PICKER_MAX);
   compose->picker_sel = 0;
   compose->picking = true;
}

static const char *const REPLY_GATE_LABELS[COBALT_REPLY_GATE_COUNT] = {
   "Everyone can reply", "Followed/mentioned can reply", "Replies off"
};

/* Hit targets for the confirmation row, rebuilt on every GamePad draw. */
static SDL_Rect s_confirm_hit[CONFIRM_COUNT];
static bool s_confirm_hit_valid = false;

void
cobalt_compose_init(cobalt_compose *compose)
{
   if (!compose) {
      return;
   }
   memset(compose, 0, sizeof(*compose));
   cobalt_keyboard_open(&compose->kb, compose->text, sizeof(compose->text), false);
}

void
cobalt_compose_quote(cobalt_compose *compose, const cobalt_post *post)
{
   if (!compose) {
      return;
   }
   cobalt_compose_init(compose);
   if (!post || !post->uri[0] || !post->cid[0]) {
      return;
   }
   snprintf(compose->quote_uri, sizeof(compose->quote_uri), "%s", post->uri);
   snprintf(compose->quote_cid, sizeof(compose->quote_cid), "%s", post->cid);
   snprintf(compose->quote_handle, sizeof(compose->quote_handle), "%s",
            post->handle);
}

bool
cobalt_compose_is_quote(const cobalt_compose *compose)
{
   return compose && compose->quote_uri[0];
}

void
cobalt_compose_reply_to(cobalt_compose *compose, const cobalt_post *post)
{
   if (!compose) {
      return;
   }
   cobalt_compose_init(compose);
   if (!post) {
      return;
   }

   snprintf(compose->parent_uri, sizeof(compose->parent_uri), "%s", post->uri);
   snprintf(compose->parent_cid, sizeof(compose->parent_cid), "%s", post->cid);

   /*
    * The feed and thread parsers already resolve this: a post that is not a
    * reply is recorded as its own root, so there is no special case here and
    * no chance of sending an empty root ref.
    */
   snprintf(compose->root_uri, sizeof(compose->root_uri), "%s",
            post->root_uri[0] ? post->root_uri : post->uri);
   snprintf(compose->root_cid, sizeof(compose->root_cid), "%s",
            post->root_cid[0] ? post->root_cid : post->cid);

   snprintf(compose->reply_to, sizeof(compose->reply_to), "%s", post->handle);
}

bool
cobalt_compose_is_reply(const cobalt_compose *compose)
{
   return compose && compose->parent_uri[0] != '\0';
}

int
cobalt_compose_remaining(const cobalt_compose *compose)
{
   if (!compose) {
      return COBALT_COMPOSE_GRAPHEMES;
   }

   /* Count lead bytes: every UTF-8 codepoint has exactly one, so this is a
    * codepoint count without decoding anything. */
   int codepoints = 0;
   for (const char *c = compose->text; *c; c++) {
      if (((unsigned char) *c & 0xC0) != 0x80) {
         codepoints++;
      }
   }
   return COBALT_COMPOSE_GRAPHEMES - codepoints;
}

/* --- input --- */

cobalt_compose_action
cobalt_compose_update(cobalt_compose *compose, const cobalt_input *in)
{
   if (!compose || !in) {
      return COBALT_COMPOSE_STAY;
   }

   /* A request is in flight: freeze the screen rather than letting someone
    * edit text that has already been handed to the worker. */
   if (cobalt_session_busy()) {
      return COBALT_COMPOSE_STAY;
   }

   if (!compose->confirming) {
      switch (cobalt_keyboard_update(&compose->kb, in)) {
         case COBALT_KB_ACCEPTED:
            if (compose->text[0] == '\0') {
               /* Nothing to post. Stay put rather than opening a confirmation
                * whose only sensible answer is "no". */
               return COBALT_COMPOSE_STAY;
            }
            compose->confirming = true;
            compose->confirm_choice = CONFIRM_POST;
            break;

         case COBALT_KB_CANCELLED:
            /* Cancel from the keyboard backs out of composing entirely, and
             * the draft goes with it. Anything else would need somewhere to
             * keep drafts, which this does not have yet. */
            return COBALT_COMPOSE_CANCELLED;

         case COBALT_KB_IDLE:
         default:
            break;
      }
      return COBALT_COMPOSE_STAY;
   }

   if (compose->alt_editing) {
      /* Accept or cancel both end the step; cancel just leaves the text as
       * typed so far. Alt text is optional. */
      if (cobalt_keyboard_update(&compose->alt_kb, in) != COBALT_KB_IDLE) {
         compose->alt_editing = false;
      }
      return COBALT_COMPOSE_STAY;
   }

   if (compose->picking) {
      if (cobalt_input_pressed(in, COBALT_BTN_UP) && compose->picker_sel > 0) {
         compose->picker_sel--;
      }
      if (cobalt_input_pressed(in, COBALT_BTN_DOWN) &&
          compose->picker_sel + 1 < compose->picker_count) {
         compose->picker_sel++;
      }
      if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
         compose->picking = false;
      } else if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM) &&
                 compose->picker_count > 0) {
         snprintf(compose->attach_path, sizeof(compose->attach_path), "%s/%s",
                  compose->picker_dir, compose->picker_names[compose->picker_sel]);
         compose->picking = false;
         compose->attach_alt[0] = '\0';
         cobalt_keyboard_open(&compose->alt_kb, compose->attach_alt,
                              sizeof(compose->attach_alt), false);
         compose->alt_editing = true;
      }
      return COBALT_COMPOSE_STAY;
   }

   /* Confirmation row. */
   int ids[CONFIRM_COUNT];
   const int nids = confirm_ids(compose, ids);
   int pos = 0;
   for (int i = 0; i < nids; i++) {
      if (ids[i] == compose->confirm_choice) {
         pos = i;
      }
   }
   if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
      pos = (pos + nids - 1) % nids;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
      pos = (pos + 1) % nids;
   }
   compose->confirm_choice = ids[pos];

   /* B goes back to editing rather than discarding: losing a post someone
    * just typed on a console keyboard would be a genuinely bad outcome. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      compose->confirming = false;
      return COBALT_COMPOSE_STAY;
   }

   /* Reply-control cycling. Top-level posts only — see the enum's doc
    * comment in compose.h for why replies don't get this choice. */
   if (!cobalt_compose_is_reply(compose) &&
       cobalt_input_pressed(in, COBALT_BTN_ALT_Y)) {
      compose->reply_gate =
         (compose->reply_gate + 1) % COBALT_REPLY_GATE_COUNT;
   }

   int chosen = -1;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      chosen = compose->confirm_choice;
   } else if (s_confirm_hit_valid && in->touch_ended) {
      for (int i = 0; i < nids; i++) {
         if (cobalt_input_tapped(in, &s_confirm_hit[i])) {
            compose->confirm_choice = ids[i];
            chosen = ids[i];
            break;
         }
      }
   }

   switch (chosen) {
      case CONFIRM_POST:
         if (cobalt_compose_remaining(compose) < 0) {
            /* Over the limit. Send them back to trim it rather than letting
             * the server reject it after a round trip. */
            compose->confirming = false;
            return COBALT_COMPOSE_STAY;
         }
         return COBALT_COMPOSE_SUBMIT;

      case CONFIRM_IMAGE:
         if (compose->attach_path[0]) {
            compose->attach_path[0] = '\0';
            compose->attach_alt[0] = '\0';
         } else {
            char dir[COBALT_ATTACH_PATH_MAX];
            if (cobalt_data_path(dir, sizeof(dir), "images")) {
               cobalt_compose_open_picker(compose, dir);
            }
         }
         return COBALT_COMPOSE_STAY;

      case CONFIRM_EDIT:
         compose->confirming = false;
         return COBALT_COMPOSE_STAY;

      case CONFIRM_DISCARD:
         return COBALT_COMPOSE_CANCELLED;

      default:
         return COBALT_COMPOSE_STAY;
   }
}

/* --- drawing --- */

static void
draw_header(cobalt_compose *compose, cobalt_render *r)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);

   const char *title = cobalt_compose_is_reply(compose)  ? "Reply"
                       : cobalt_compose_is_quote(compose) ? "Quote post"
                                                          : "New post";
   cobalt_draw_text(r, COBALT_FONT_TITLE, title, m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   char subtitle[COBALT_POST_NAME_MAX + 40];
   const int remaining = cobalt_compose_remaining(compose);
   if (cobalt_compose_is_reply(compose)) {
      snprintf(subtitle, sizeof(subtitle), "to %s    %d left", compose->reply_to,
               remaining);
   } else if (cobalt_compose_is_quote(compose)) {
      snprintf(subtitle, sizeof(subtitle), "quoting %s    %d left",
               compose->quote_handle, remaining);
   } else {
      snprintf(subtitle, sizeof(subtitle), "%d characters left", remaining);
   }

   /* The counter turns red before it is a problem, not after. */
   SDL_Color colour = { 0xD8, 0xE6, 0xF4, 0xFF };
   cobalt_draw_text(r, remaining < 0 ? COBALT_FONT_CAPTION : COBALT_FONT_CAPTION,
                    subtitle, m->pad_edge,
                    m->pad_edge + cobalt_font_line_height(r, COBALT_FONT_TITLE) -
                       m->line_gap,
                    remaining < 0 ? COBALT_COLOUR_ERROR : colour);
}

static void
draw_editing(cobalt_compose *compose, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);

   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int lines = 3;
   const int box_h = m->pad_tile * 2 + lines * (body_h + m->line_gap);

   SDL_Rect box = { m->pad_edge, top, m->width - 2 * m->pad_edge, box_h };
   cobalt_draw_tile(r, &box, 1.0f);

   if (compose->text[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, compose->text,
                               box.x + m->pad_tile, box.y + m->pad_tile,
                               box.w - 2 * m->pad_tile, lines, COBALT_COLOUR_TEXT);
   } else {
      cobalt_draw_text(r, COBALT_FONT_BODY,
                       cobalt_compose_is_reply(compose) ? "Write a reply..."
                                                        : "What's up?",
                       box.x + m->pad_tile, box.y + m->pad_tile,
                       COBALT_COLOUR_TEXT_DIM);
   }

   SDL_Rect keys = { m->pad_edge, box.y + box_h + m->gap,
                     m->width - 2 * m->pad_edge,
                     cobalt_keyboard_height(surface) };
   cobalt_keyboard_draw(&compose->kb, r, surface, &keys);
}

static void
draw_confirming(cobalt_compose *compose, cobalt_render *r,
                cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   /* Show the whole post here, not a two-line preview — this is the last
    * chance to notice a typo before it is public. */
   const int lines = (surface == COBALT_SURFACE_DRC) ? 6 : 8;
   const int box_h = m->pad_tile * 2 + lines * (body_h + m->line_gap);

   SDL_Rect box = { m->pad_edge, top, m->width - 2 * m->pad_edge, box_h };
   cobalt_draw_tile(r, &box, 0.0f);
   cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, compose->text,
                            box.x + m->pad_tile, box.y + m->pad_tile,
                            box.w - 2 * m->pad_tile, lines, COBALT_COLOUR_TEXT);

   int ids[CONFIRM_COUNT];
   const int nids = confirm_ids(compose, ids);

   const int row_h = m->font_body * 2;
   const int gap = m->gap;
   const int button_w = (box.w - gap * (nids - 1)) / nids;
   const int row_y = box.y + box_h + gap * 2;

   for (int i = 0; i < nids; i++) {
      SDL_Rect button = { m->pad_edge + i * (button_w + gap), row_y, button_w,
                          row_h };
      const bool focused = (ids[i] == compose->confirm_choice);
      cobalt_draw_tile(r, &button, focused ? 1.0f : 0.0f);

      SDL_Color colour = (ids[i] == CONFIRM_DISCARD) ? COBALT_COLOUR_ERROR
                                                : COBALT_COLOUR_ACCENT;
      const int label_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text_centred(r, COBALT_FONT_BODY, confirm_label(compose, ids[i]), button.x,
                               button.y + (row_h - label_h) / 2, button.w,
                               focused ? colour : COBALT_COLOUR_TEXT_DIM);

      if (surface == COBALT_SURFACE_DRC) {
         s_confirm_hit[i] = button;
      }
   }

   if (surface == COBALT_SURFACE_DRC) {
      s_confirm_hit_valid = true;
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };

   if (!cobalt_compose_is_reply(compose)) {
      char gate_line[64];
      snprintf(gate_line, sizeof(gate_line), "Y: %s",
               REPLY_GATE_LABELS[compose->reply_gate]);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, gate_line, m->pad_edge,
                       row_y + row_h + gap, hint);

      if (compose->attach_path[0]) {
         const char *name = strrchr(compose->attach_path, '/');
         char img_line[COBALT_PICKER_NAME_MAX + 16];
         snprintf(img_line, sizeof(img_line), "Image: %s",
                  name ? name + 1 : compose->attach_path);
         cobalt_draw_text(r, COBALT_FONT_CAPTION, img_line, m->pad_edge,
                          row_y + row_h + gap +
                             cobalt_font_line_height(r, COBALT_FONT_CAPTION),
                          hint);
      }
   }

   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy() ? "Posting..."
                                          : "A: choose    B: back to editing",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

static void
draw_alt_editing(cobalt_compose *compose, cobalt_render *r,
                 cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
   const int lines = 2;
   const int box_h = m->pad_tile * 2 + lines * (body_h + m->line_gap);

   SDL_Rect box = { m->pad_edge, top, m->width - 2 * m->pad_edge, box_h };
   cobalt_draw_tile(r, &box, 1.0f);
   cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                            compose->attach_alt[0] ? compose->attach_alt
                                                   : "Describe the image (optional)",
                            box.x + m->pad_tile, box.y + m->pad_tile,
                            box.w - 2 * m->pad_tile, lines,
                            compose->attach_alt[0] ? COBALT_COLOUR_TEXT
                                                   : COBALT_COLOUR_TEXT_DIM);

   SDL_Rect keys = { m->pad_edge, box.y + box_h + m->gap,
                     m->width - 2 * m->pad_edge, cobalt_keyboard_height(surface) };
   cobalt_keyboard_draw(&compose->alt_kb, r, surface, &keys);
}

static void
draw_picker(cobalt_compose *compose, cobalt_render *r, cobalt_surface_id surface)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int top = m->pad_edge + (surface == COBALT_SURFACE_DRC ? 62 : 130);
   const int row_h = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->line_gap;
   const int visible = 6;

   if (compose->picker_count == 0) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                               "No images found. Copy .jpg or .png files "
                               "(under 950 KB) into the folder below on the SD "
                               "card, then try again.",
                               m->pad_edge, top, m->width - 2 * m->pad_edge, 4,
                               COBALT_COLOUR_TEXT);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, compose->picker_dir, m->pad_edge,
                       top + 4 * row_h, COBALT_COLOUR_TEXT_DIM);
   } else {
      int first = compose->picker_sel - visible + 1;
      if (first < 0) {
         first = 0;
      }
      for (int i = 0; i < visible && first + i < compose->picker_count; i++) {
         SDL_Rect row = { m->pad_edge, top + i * (row_h + m->gap),
                          m->width - 2 * m->pad_edge, row_h };
         const bool focused = (first + i == compose->picker_sel);
         cobalt_draw_tile(r, &row, focused ? 1.0f : 0.0f);
         cobalt_draw_text(r, COBALT_FONT_BODY, compose->picker_names[first + i],
                          row.x + m->pad_tile, row.y + m->line_gap / 2,
                          focused ? COBALT_COLOUR_ACCENT : COBALT_COLOUR_TEXT);
      }
   }

   SDL_Color hint = { 0xB8, 0xCC, 0xE0, 0xFF };
   cobalt_draw_text(r, COBALT_FONT_CAPTION, "Up/Down: choose    A: attach    B: back",
                    m->pad_edge, m->height - m->pad_edge - 20, hint);
}

void
cobalt_compose_draw(cobalt_compose *compose, cobalt_render *r,
                    cobalt_surface_id surface)
{
   if (!compose || !r) {
      return;
   }

   draw_header(compose, r);

   if (compose->confirming && compose->alt_editing) {
      draw_alt_editing(compose, r, surface);
   } else if (compose->confirming && compose->picking) {
      draw_picker(compose, r, surface);
   } else if (compose->confirming) {
      draw_confirming(compose, r, surface);
   } else {
      draw_editing(compose, r, surface);
   }
}
