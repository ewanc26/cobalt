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

/*
 * Fixed heights for the embed block, in body-line multiples like AVATAR_SIDE
 * above. Width-independent by design: cobalt_postcard_height has no rect to
 * measure against (callers always draw at the same width regardless of
 * indent — see thread.c, which passes indent to draw but not to height), so
 * every size in this file that has to agree between the two is a fixed
 * budget rather than something computed from a draw-time width.
 */
#define EMBED_SINGLE_IMAGE_H(bh) ((bh) * 6)
#define EMBED_IMAGE_GRID_H(bh)   ((bh) * 4)
#define EMBED_GAP(m)             ((m)->pad_tile / 2)
#define EMBED_CELL_GAP(m)        ((m)->pad_tile / 4 > 0 ? (m)->pad_tile / 4 : 2)

/*
 * Height of the image/link-card block below the post text, or 0 for a post
 * with neither. Shared between cobalt_postcard_height and cobalt_postcard_draw
 * so the two cannot disagree about how much room it takes.
 */
static int
embed_block_height(const cobalt_metrics *m, int caption_h, int body_h,
                   const cobalt_post *post)
{
   if (post->image_count == 1) {
      return EMBED_SINGLE_IMAGE_H(body_h);
   }
   if (post->image_count > 1) {
      return EMBED_IMAGE_GRID_H(body_h);
   }
   if (post->link.uri[0]) {
      /* Padding top and bottom, one title line, two description lines, one
       * domain line. */
      return m->pad_tile + body_h + 3 * caption_h;
   }
   return 0;
}

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

   const int embed_h = embed_block_height(m, caption_h, body_h, post);
   if (embed_h > 0) {
      h += embed_h + EMBED_GAP(m);
   }

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
cobalt_postcard_contain_fit(int box_w, int box_h, int src_w, int src_h,
                            int *out_w, int *out_h)
{
   if (!out_w || !out_h) {
      return;
   }
   if (box_w <= 0 || box_h <= 0) {
      *out_w = 0;
      *out_h = 0;
      return;
   }
   if (src_w <= 0 || src_h <= 0) {
      *out_w = box_w;
      *out_h = box_h;
      return;
   }

   /* Scale to the box height first; if that overflows the width, scale to
    * the width instead. Exactly one of the two candidate scales fits — the
    * other, by definition, is the one that overflowed. */
   const int64_t by_height_w = (int64_t) box_h * src_w / src_h;
   if (by_height_w <= box_w) {
      *out_w = (int) by_height_w;
      *out_h = box_h;
   } else {
      *out_w = box_w;
      *out_h = (int) ((int64_t) box_w * src_h / src_w);
   }
}

/*
 * One image or link-card thumbnail, aspect-fit within `cell` and centred.
 * Draws a flat frame first regardless of whether a texture is ready, so a
 * post with only an image reads as "something is here, still loading" rather
 * than as a gap — the same reasoning as the avatar placeholder disc, without
 * a letter since a thumbnail has no name to draw one from.
 */
static void
draw_thumb_cell(cobalt_render *r, const char *url, int aspect_w, int aspect_h,
                const SDL_Rect *cell)
{
   cobalt_fill_rounded_rect(r, cell, 6, COBALT_COLOUR_TILE_EDGE);

   cobalt_imagecache *thumbs = cobalt_render_thumbs(r);
   SDL_Texture *texture = NULL;
   int tex_w = 0, tex_h = 0;
   if (thumbs && url && url[0]) {
      texture = cobalt_imagecache_get(thumbs, r, url, &tex_w, &tex_h);
   }
   if (!texture) {
      return;
   }

   const int src_w = tex_w > 0 ? tex_w : aspect_w;
   const int src_h = tex_h > 0 ? tex_h : aspect_h;

   int dst_w = cell->w, dst_h = cell->h;
   cobalt_postcard_contain_fit(cell->w, cell->h, src_w, src_h, &dst_w, &dst_h);

   const SDL_Rect dst = { cell->x + (cell->w - dst_w) / 2,
                          cell->y + (cell->h - dst_h) / 2, dst_w, dst_h };
   cobalt_draw_texture(r, texture, &dst);
}

/* Up to COBALT_POST_IMAGES_MAX images, side by side in one row. */
static void
draw_image_row(cobalt_render *r, const cobalt_post *post, int x, int y,
               int width, int height)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const int n = post->image_count;
   const int gap = EMBED_CELL_GAP(m);
   const int cell_w = (width - gap * (n - 1)) / (n > 0 ? n : 1);
   if (cell_w <= 0) {
      return;
   }

   int cx = x;
   for (int i = 0; i < n; i++) {
      const SDL_Rect cell = { cx, y, cell_w, height };
      draw_thumb_cell(r, post->images[i].thumb, post->images[i].aspect_w,
                      post->images[i].aspect_h, &cell);
      cx += cell_w + gap;
   }
}

/*
 * A link card: an optional square thumbnail, then title, description and the
 * link's host stacked beside it — the same information Bluesky's own link
 * card shows, laid out for a fixed-height row rather than a variable one for
 * the same reason the rest of this file is width- but not content-driven.
 */
static void
draw_link_card(cobalt_render *r, const cobalt_post *post, int x, int y,
              int width, int height)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   const SDL_Rect box = { x, y, width, height };
   cobalt_draw_tile(r, &box, 0.0f);

   const int pad = EMBED_GAP(m);
   const int caption_h = cobalt_font_line_height(r, COBALT_FONT_CAPTION);

   int text_x = x + pad;
   if (post->link.thumb[0]) {
      const int side = height - 2 * pad;
      const SDL_Rect cell = { x + pad, y + pad, side, side };
      draw_thumb_cell(r, post->link.thumb, 1, 1, &cell);
      text_x = cell.x + cell.w + pad;
   }

   const int text_right = x + width - pad;
   const int text_width = text_right - text_x;
   if (text_width <= 0) {
      return;
   }

   int ty = y + pad;
   if (post->link.title[0]) {
      ty += cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, post->link.title,
                                     text_x, ty, text_width, 1,
                                     COBALT_COLOUR_TEXT);
   }
   if (post->link.description[0]) {
      ty += cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION,
                                     post->link.description, text_x, ty,
                                     text_width, 2, COBALT_COLOUR_TEXT_DIM);
   }

   char domain[64];
   cobalt_feed_link_domain(post->link.uri, domain, sizeof(domain));
   if (domain[0]) {
      cobalt_draw_text(r, COBALT_FONT_CAPTION, domain, text_x,
                       y + height - pad - caption_h, COBALT_COLOUR_ACCENT);
   }
}

/* Dispatches to whichever of the above the post actually carries. `height`
 * is the caller's embed_block_height() result, not recomputed here, so draw
 * and height can never disagree about how tall this block is. */
static void
draw_embed_media(cobalt_render *r, const cobalt_post *post, int x, int y,
                 int width, int height)
{
   if (post->image_count > 0) {
      draw_image_row(r, post, x, y, width, height);
   } else if (post->link.uri[0]) {
      draw_link_card(r, post, x, y, width, height);
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

   const int embed_h = embed_block_height(m, caption_h, body_h, post);
   if (embed_h > 0) {
      draw_embed_media(r, post, text_left, y, text_width, embed_h);
      y += embed_h + EMBED_GAP(m);
   }

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
