#pragma once

/*
 * One post, drawn as a Wii U menu tile.
 *
 * Shared by the timeline and the thread view. They are the same card with a
 * different indent, and keeping one implementation is what stops the two
 * screens drifting into looking like different applications — which is exactly
 * what AGENTS.md §5 is asking for when it says the app should read as native
 * rather than as a set of web views.
 *
 * Height is measured rather than assumed: a post with no engagement and no
 * repost banner is genuinely shorter than one with both, and reserving space
 * for the maximum would waste a third of a 480-pixel panel.
 */

#include "atproto/feed.h"
#include "ui/render.h"
#include "ui/theme.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The longest edge an avatar is decoded to, for whoever builds the image cache.
 * Larger than the side a card draws it at, so SDL is scaling down rather than
 * up and the masked circle's edge stays smooth.
 */
#define COBALT_AVATAR_TEXTURE_MAX 96

/*
 * The longest edge a post image or link-card thumbnail is decoded to.
 * Bigger than an avatar's, since these are drawn at up to a card's full
 * width, but deliberately not the ~1000 px a CDN thumbnail can arrive at: on
 * a console viewed from a couch or held at arm's length that resolution buys
 * nothing, and twenty-four of them at 1000x1000 would be 96 MB per surface
 * (see ui/imagecache.h's note on the avatar budget). At this size the same
 * arithmetic caps a surface's thumbnail cache under 10 MB.
 */
#define COBALT_THUMB_TEXTURE_MAX 320

/*
 * Draw an avatar: the cached image once it has arrived, a lettered disc in the
 * account's own colour until then, and permanently for accounts that have not
 * set one. `side` is the diameter.
 *
 * Shared with the profile header rather than reimplemented there, so the two
 * cannot drift into looking like different applications — the same reason this
 * file exists at all. `name` supplies the initial and `handle` the tint.
 */
void cobalt_avatar_draw(cobalt_render *r, const char *url, const char *name,
                        const char *handle, int x, int y, int side);

/*
 * Scale (src_w, src_h) down to fit within (box_w, box_h) without cropping,
 * preserving aspect ratio — the same "contain" behaviour as CSS
 * `object-fit: contain`, picking whichever axis is tighter. Falls back to
 * filling the box outright when the source size is unknown (zero or
 * negative), which is what a caller wants while a texture is still loading
 * and only a placeholder is being sized.
 *
 * Split out from the drawing code so the arithmetic — the part of a layout
 * bug that looks fine on a screenshot and is only wrong when you check the
 * numbers — can be tested directly.
 */
void cobalt_postcard_contain_fit(int box_w, int box_h, int src_w, int src_h,
                                 int *out_w, int *out_h);

/* How tall the card will be for `text_lines` lines of post text. */
int cobalt_postcard_height(cobalt_render *r, const cobalt_post *post,
                           int text_lines);

/*
 * Draw into `rect`. `indent` is the reply depth — 0 for a timeline card —
 * and shifts the content right without narrowing the tile, so the tree is
 * legible without costing width the text needs.
 */
void cobalt_postcard_draw(cobalt_render *r, const cobalt_post *post,
                          const SDL_Rect *rect, bool focused, int text_lines,
                          int indent);

#ifdef __cplusplus
}
#endif
