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
