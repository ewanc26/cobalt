#include "ui/postcard.h"

#include "ui/imagecache.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* How far one reply level shifts the content, as a fraction of tile padding.
 * Kept modest: on the GamePad's 854 pixels, a generous indent would leave the
 * deepest replies with a column of text too narrow to read. */
#define INDENT_STEP(m) ((m)->pad_tile)

/*
 * Avatar side, and the gap between it and the text column.
 *
 * Sized from the body font rather than fixed, because the two surfaces have
 * their own type scales (AGENTS.md §5) and a 48-pixel avatar that suits the TV
 * is most of the GamePad's line height.
 */
#define AVATAR_SIDE(m) ((m)->font_body * 2)
#define AVATAR_GAP(m)  ((m)->pad_tile / 2)

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

   /* The avatar column can be taller than the text beside it — a one-line post
    * with no engagement is shorter than the picture next to it — so the card
    * has to grow rather than let the circle spill onto the tile below. */
   const int avatar_min =
      m->pad_tile + AVATAR_SIDE(m) + m->pad_tile +
      (post->reposted_by[0] ? caption_h : 0);
   if (h < avatar_min) {
      h = avatar_min;
   }

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

/*
 * The first UTF-8 codepoint of `text`, copied whole.
 *
 * Byte-wise, because taking text[0] would slice a multi-byte sequence and draw
 * tofu for every account whose name does not begin with ASCII — which on
 * Bluesky is a great many of them.
 */
static bool
first_codepoint(const char *text, char *out, size_t out_size)
{
   if (!text || !text[0] || out_size < 5) {
      return false;
   }

   const unsigned char lead = (unsigned char) text[0];
   size_t len = 1;
   if (lead >= 0xF0)      len = 4;
   else if (lead >= 0xE0) len = 3;
   else if (lead >= 0xC0) len = 2;

   /* A truncated sequence at the end of the string is not worth drawing. */
   for (size_t i = 1; i < len; i++) {
      if ((text[i] & 0xC0) != 0x80) {
         return false;
      }
   }

   memcpy(out, text, len);
   out[len] = '\0';
   return true;
}

/*
 * A stable tint for the placeholder disc, derived from the handle.
 *
 * A timeline of identical grey circles is worse than no avatars at all: it
 * reads as one voice. Giving each account its own colour keeps the column
 * scannable while an image is still in flight, and keeps it useful for the
 * accounts that never set one.
 */
static SDL_Color
placeholder_tint(const char *handle)
{
   uint32_t h = 2166136261u;
   for (const char *p = handle; p && *p; p++) {
      h ^= (unsigned char) *p;
      h *= 16777619u;
   }

   /* Six hues around the wheel at a fixed, muted saturation, so every tint sits
    * within the palette rather than one account coming out fluorescent. */
   static const SDL_Color tints[] = {
      { 0x3E, 0x63, 0x9B, 0xFF }, { 0x4B, 0x87, 0x7A, 0xFF },
      { 0x8A, 0x6A, 0x3C, 0xFF }, { 0x8B, 0x4F, 0x63, 0xFF },
      { 0x63, 0x51, 0x8E, 0xFF }, { 0x3F, 0x74, 0x8C, 0xFF },
   };
   return tints[h % (sizeof(tints) / sizeof(tints[0]))];
}

void
cobalt_avatar_draw(cobalt_render *r, const char *url, const char *name,
                   const char *handle, int x, int y, int side)
{
   if (!r || side <= 0) {
      return;
   }

   const SDL_Rect box = { x, y, side, side };

   cobalt_imagecache *images = cobalt_render_images(r);
   SDL_Texture *texture = NULL;
   if (images && url && url[0]) {
      texture = cobalt_imagecache_get(images, r, url, NULL, NULL);
   }

   if (texture) {
      /* Already masked to a circle on the loader thread — see imagecache.h. */
      cobalt_draw_texture(r, texture, &box);
      return;
   }

   /* radius = side/2 makes the rounded rect a disc, reusing the antialiased
    * corner texture rather than adding a second circle primitive. */
   cobalt_fill_rounded_rect(r, &box, side / 2, placeholder_tint(handle));

   char initial[8];
   if (first_codepoint(name, initial, sizeof(initial))) {
      /* Pick the size that fills the disc: the card's is body-sized, the
       * profile header's is twice that, and one font for both would leave the
       * larger one with a letter lost in the middle of it. */
      const cobalt_metrics *m = cobalt_render_metrics(r);
      const cobalt_font_id font =
         (side >= m->font_heading * 2) ? COBALT_FONT_HEADING : COBALT_FONT_BODY;

      int w = 0, h = 0;
      cobalt_text_size(r, font, initial, &w, &h);
      cobalt_draw_text(r, font, initial, x + (side - w) / 2,
                       y + (side - h) / 2, COBALT_COLOUR_TEXT);
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
   if (right - left <= 0) {
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

   /*
    * The avatar sits below the repost banner, not beside it: the banner is
    * about who put the post in the feed, the avatar about who wrote it, and
    * running them together would say neither clearly.
    */
   cobalt_avatar_draw(r, post->avatar, post->author, post->handle, left, y,
                      AVATAR_SIDE(m));
   const int text_left = left + AVATAR_SIDE(m) + AVATAR_GAP(m);
   const int text_width = right - text_left;
   if (text_width <= 0) {
      return;
   }

   /* Author row. The age is right-aligned, so it is measured and placed from
    * the right edge rather than flowed after the handle, whose width varies
    * wildly with the display name. */
   const int name_w = cobalt_draw_text(r, COBALT_FONT_BODY, post->author,
                                       text_left, y, COBALT_COLOUR_TEXT);

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
   const int handle_x = text_left + name_w + m->pad_tile / 2;
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
      cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, post->text, text_left, y,
                               text_width, text_lines, COBALT_COLOUR_TEXT);
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
         cobalt_draw_text(r, COBALT_FONT_CAPTION, footer, text_left, y,
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
