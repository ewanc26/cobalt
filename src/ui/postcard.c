#include "ui/postcard.h"

#include <stdio.h>
#include <string.h>

/* How far one reply level shifts the content, as a fraction of tile padding.
 * Kept modest: on the GamePad's 854 pixels, a generous indent would leave the
 * deepest replies with a column of text too narrow to read. */
#define INDENT_STEP(m) ((m)->pad_tile)

int
cobalt_postcard_height(cobalt_render *r, const cobalt_post *post, int text_lines)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   int h = m->pad_tile;
   if (post->reposted_by[0]) {
      h += caption_h;
   }
   h += body_h;                                     /* author row */
   h += text_lines * (body_h + m->line_gap);
   if (post->meta[0] || post->embed_note[0] || post->viewer_like[0] ||
       post->viewer_repost[0]) {
      h += caption_h;
   }
   h += m->pad_tile;

   /* A degenerate value would make the caller's scroll maths divide the screen
    * into an unbounded number of cards. The font failing to load is already
    * reported on the diagnostics screen. */
   return h > 0 ? h : m->font_body * 4;
}

/*
 * What the viewer has already done to this post, right-aligned on the footer.
 *
 * Words rather than symbols: the bundled font is a placeholder chosen for
 * coverage of ordinary text, and a heart or repost arrow would be a real risk
 * of drawing tofu on every liked post at once (AGENTS.md §5's font note).
 */
static void
viewer_marker(const cobalt_post *post, char *out, size_t out_size)
{
   const bool liked = post->viewer_like[0] != '\0';
   const bool reposted = post->viewer_repost[0] != '\0';

   if (liked && reposted) {
      snprintf(out, out_size, "Liked \xC2\xB7 Reposted");
   } else if (liked) {
      snprintf(out, out_size, "Liked");
   } else if (reposted) {
      snprintf(out, out_size, "Reposted");
   } else {
      out[0] = '\0';
   }
}

void
cobalt_postcard_draw(cobalt_render *r, const cobalt_post *post,
                     const SDL_Rect *rect, bool focused, int text_lines,
                     int indent)
{
   if (!r || !post || !rect) {
      return;
   }

   const cobalt_metrics *m = cobalt_render_metrics(r);
   cobalt_draw_tile(r, rect, focused ? 1.0f : 0.0f);

   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);
   const int body_h = cobalt_font_line_height(r, COBALT_FONT_BODY);

   const int shift = indent * INDENT_STEP(m);
   const int left = rect->x + m->pad_tile + shift;
   const int right = rect->x + rect->w - m->pad_tile;
   const int width = right - left;
   if (width <= 0) {
      return;
   }
   int y = rect->y + m->pad_tile;

   if (post->reposted_by[0]) {
      char banner[COBALT_POST_NAME_MAX + 16];
      snprintf(banner, sizeof(banner), "Reposted by %s", post->reposted_by);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, banner, left, y,
                       COBALT_COLOUR_TEXT_DIM);
      y += caption_h;
   }

   /* Author row. The age is right-aligned, so it is measured and placed from
    * the right edge rather than flowed after the handle, whose width varies
    * wildly with the display name. */
   const int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, post->author, left, y,
                                       COBALT_COLOUR_TEXT);

   int age_w = 0;
   if (post->age[0]) {
      cobalt_text_size(r, COBALT_FONT_CAPTION, post->age, &age_w, NULL);
      cobalt_draw_text(r, COBALT_FONT_CAPTION, post->age, right - age_w,
                       y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      age_w += m->pad_tile;
   }

   /* The handle fills whatever is left between the name and the age, and is
    * dropped rather than overlapped when there is none — on a narrow card a
    * long display name legitimately uses the whole row. */
   const int handle_x = left + name_w + m->pad_tile / 2;
   const int handle_room = (right - age_w) - handle_x;
   if (handle_room > 0) {
      int handle_w = 0;
      cobalt_text_size(r, COBALT_FONT_CAPTION, post->handle, &handle_w, NULL);
      if (handle_w <= handle_room) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, post->handle, handle_x,
                          y + (body_h - caption_h) / 2, COBALT_COLOUR_TEXT_DIM);
      }
   }
   y += body_h;

   /* An image-only post has no text at all, which is legitimate — the embed
    * note below is then the only thing describing it. */
   if (post->text[0]) {
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, post->text, left, y, width,
                               text_lines, COBALT_COLOUR_TEXT);
   }
   y += text_lines * (body_h + m->line_gap);

   char marker[48];
   viewer_marker(post, marker, sizeof(marker));

   if (post->meta[0] || post->embed_note[0] || marker[0]) {
      char footer[COBALT_POST_META_MAX + 40];
      if (post->meta[0] && post->embed_note[0]) {
         snprintf(footer, sizeof(footer), "%s   %s", post->meta, post->embed_note);
      } else {
         snprintf(footer, sizeof(footer), "%s",
                  post->meta[0] ? post->meta : post->embed_note);
      }
      if (footer[0]) {
         cobalt_draw_text(r, COBALT_FONT_CAPTION, footer, left, y,
                          COBALT_COLOUR_TEXT_DIM);
      }

      if (marker[0]) {
         int marker_w = 0;
         cobalt_text_size(r, COBALT_FONT_CAPTION, marker, &marker_w, NULL);
         cobalt_draw_text(r, COBALT_FONT_CAPTION, marker, right - marker_w, y,
                          COBALT_COLOUR_ACCENT);
      }
   }
}
