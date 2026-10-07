#pragma once

/*
 * The timeline screen — AGENTS.md §12's step 3.
 *
 * Scrolling is by *selection* rather than by pixel: a highlighted post moves
 * with the D-pad and the view follows it. That is the Wii U Menu's own idiom
 * and it is the only one that works for someone holding a Pro Controller with
 * the GamePad face down, which §5 requires every screen to support. Touch
 * still selects a card directly.
 *
 * Cards are variable height — a post with no engagement and no repost banner
 * takes less room than one with both — so the layout measures rather than
 * assumes. The consequence is that how many posts fit is only known after a
 * draw, which is why the view state carries what the last frame managed to fit.
 */

#include "atproto/feed.h"
#include "input/input.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_TIMELINE_STAY = 0,
   COBALT_TIMELINE_BACK,
   COBALT_TIMELINE_OPEN_THREAD,
   COBALT_TIMELINE_OPEN_PROFILE,
   COBALT_TIMELINE_COMPOSE,
   COBALT_TIMELINE_MENU,
} cobalt_timeline_action;

typedef struct {
   /* Selection, scroll and touch targets. The GamePad is the tighter of the two
    * surfaces and is always present, so what it fitted is what scrolling
    * follows, keeping anything visible there visible on the TV as well. */
   cobalt_listnav nav;
} cobalt_timeline;

void cobalt_timeline_init(cobalt_timeline *view);

/* Reset the cursor to the top. Call when the feed is replaced. */
void cobalt_timeline_rewind(cobalt_timeline *view);

cobalt_timeline_action cobalt_timeline_update(cobalt_timeline *view,
                                              const cobalt_input *in);

void cobalt_timeline_draw(cobalt_timeline *view, cobalt_render *r,
                          cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
