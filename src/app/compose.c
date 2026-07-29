#include "app/compose.h"
#include "atproto/session.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define CONFIRM_POST    0
#define CONFIRM_EDIT    1
#define CONFIRM_DISCARD 2
#define CONFIRM_COUNT   3

static const char *const CONFIRM_LABELS[CONFIRM_COUNT] = {
   "Post", "Keep editing", "Discard"
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

   /* Confirmation row. */
   if (cobalt_input_pressed(in, COBALT_BTN_LEFT)) {
      compose->confirm_choice =
         (compose->confirm_choice + CONFIRM_COUNT - 1) % CONFIRM_COUNT;
   }
   if (cobalt_input_pressed(in, COBALT_BTN_RIGHT)) {
      compose->confirm_choice = (compose->confirm_choice + 1) % CONFIRM_COUNT;
   }

   /* B goes back to editing rather than discarding: losing a post someone
    * just typed on a console keyboard would be a genuinely bad outcome. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      compose->confirming = false;
      return COBALT_COMPOSE_STAY;
   }

   int chosen = -1;
   if (cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      chosen = compose->confirm_choice;
   } else if (s_confirm_hit_valid && in->touch_ended) {
      for (int i = 0; i < CONFIRM_COUNT; i++) {
         if (cobalt_input_tapped(in, &s_confirm_hit[i])) {
            compose->confirm_choice = i;
            chosen = i;
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

   const char *title = cobalt_compose_is_reply(compose) ? "Reply" : "New post";
   cobalt_draw_text(r, COBALT_FONT_TITLE, title, m->pad_edge, m->pad_edge,
                    COBALT_COLOUR_TILE_FOCUS);

   char subtitle[COBALT_POST_NAME_MAX + 40];
   const int remaining = cobalt_compose_remaining(compose);
   if (cobalt_compose_is_reply(compose)) {
      snprintf(subtitle, sizeof(subtitle), "to %s    %d left", compose->reply_to,
               remaining);
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

   const int row_h = m->font_body * 2;
   const int gap = m->gap;
   const int button_w = (box.w - gap * (CONFIRM_COUNT - 1)) / CONFIRM_COUNT;
   const int row_y = box.y + box_h + gap * 2;

   for (int i = 0; i < CONFIRM_COUNT; i++) {
      SDL_Rect button = { m->pad_edge + i * (button_w + gap), row_y, button_w,
                          row_h };
      const bool focused = (i == compose->confirm_choice);
      cobalt_draw_tile(r, &button, focused ? 1.0f : 0.0f);

      SDL_Color colour = (i == CONFIRM_DISCARD) ? COBALT_COLOUR_ERROR
                                                : COBALT_COLOUR_ACCENT;
      const int label_h = cobalt_font_line_height(r, COBALT_FONT_BODY);
      cobalt_draw_text_centred(r, COBALT_FONT_BODY, CONFIRM_LABELS[i], button.x,
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
   cobalt_draw_text(r, COBALT_FONT_CAPTION,
                    cobalt_session_busy() ? "Posting..."
                                          : "A: choose    B: back to editing",
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

   if (compose->confirming) {
      draw_confirming(compose, r, surface);
   } else {
      draw_editing(compose, r, surface);
   }
}
