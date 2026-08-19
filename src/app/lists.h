#pragma once

/*
 * The signed-in account's lists (app.bsky.graph.list): browse the account's
 * own lists, then drill into one to see its members.
 *
 * Two sub-modes on one screen, the same shape search.c uses for "pick one
 * thing, then look at what it opens onto": `browsing_members` false shows
 * the list-of-lists menu (name/description rows, feeds.c's row shape);
 * true shows the selected list's members, reusing the exact row shape
 * search.c/graph.c already draw for accounts. B from browsing members
 * returns to the list-of-lists rather than leaving the screen.
 *
 * Read-only — see atproto/curated_lists.h for why creating/editing a list is out of
 * scope for now.
 */

#include "atproto/actors.h"
#include "atproto/curated_lists.h"
#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_LISTS_VIEW_STAY = 0,
   COBALT_LISTS_VIEW_BACK,
} cobalt_lists_view_action;

typedef struct {
   bool browsing_members;   /* false: list-of-lists; true: one list's members */
   char open_uri[COBALT_POST_URI_MAX];   /* the list currently drilled into */
   char open_name[COBALT_POST_NAME_MAX];

   int selected;
   int scroll;
   int last_visible;

   SDL_Rect hit[COBALT_LISTS_MAX > COBALT_ACTORS_MAX ? COBALT_LISTS_MAX
                                                     : COBALT_ACTORS_MAX];
   int hit_index[COBALT_LISTS_MAX > COBALT_ACTORS_MAX ? COBALT_LISTS_MAX
                                                       : COBALT_ACTORS_MAX];
   int hit_count;
   bool hit_valid;
} cobalt_lists_view;

void cobalt_lists_view_init(cobalt_lists_view *view);

/* Rewinds to the list-of-lists and, if nothing is held yet, kicks off the
 * first fetch — the same "only fetch if there is nothing to show" rule the
 * timeline/notifications/muted-blocked entry points already follow. */
void cobalt_lists_view_open(cobalt_lists_view *view);

cobalt_lists_view_action cobalt_lists_view_update(cobalt_lists_view *view,
                                                   const cobalt_input *in);

void cobalt_lists_view_draw(cobalt_lists_view *view, cobalt_render *r,
                            cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
