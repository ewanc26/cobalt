#pragma once

/*
 * An actor's profile and their posts.
 *
 * The header is a card of its own above the post list rather than a separate
 * screen, so following someone and reading what they write are one place. It
 * scrolls with the list — pinning it would cost a third of the GamePad's
 * height on a screen whose point is the posts underneath.
 *
 * Selection index 0 is the header (where the follow button lives); everything
 * after it is a post. That keeps one cursor for the whole screen rather than
 * a focus that has to move between two regions.
 */

#include "atproto/feed.h"
#include "atproto/profile.h"
#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_PROFILE_VIEW_STAY = 0,
   COBALT_PROFILE_VIEW_BACK,
   COBALT_PROFILE_VIEW_OPEN_THREAD,
} cobalt_profile_action;

typedef struct {
   int selected;   /* 0 is the header card, 1.. are posts */
   int scroll;
   int last_visible;

   SDL_Rect hit[COBALT_FEED_MAX_POSTS + 1];
   int hit_index[COBALT_FEED_MAX_POSTS + 1];
   int hit_count;
   bool hit_valid;
} cobalt_profile_view;

void cobalt_profile_view_init(cobalt_profile_view *view);
void cobalt_profile_view_rewind(cobalt_profile_view *view);

cobalt_profile_action cobalt_profile_view_update(cobalt_profile_view *view,
                                                 const cobalt_input *in);

void cobalt_profile_view_draw(cobalt_profile_view *view, cobalt_render *r,
                              cobalt_surface_id surface);

/* The post the cursor is on, or NULL when it is on the header. */
const cobalt_post *cobalt_profile_view_selected_post(const cobalt_profile_view *view);

#ifdef __cplusplus
}
#endif
