#pragma once

/*
 * Muted and blocked accounts: two screens sharing one implementation.
 *
 * Same reasoning as the post card being shared by the timeline and thread
 * view — a muted-accounts row and a blocked-accounts row are the same shape
 * (avatar, name, handle, one action), so this is one view parameterised by
 * `cobalt_graph_kind` rather than two near-identical files that would drift
 * apart the first time one of them changed.
 */

#include "atproto/actors.h"
#include "input/input.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_GRAPH_MUTED = 0,
   COBALT_GRAPH_BLOCKED,
} cobalt_graph_kind;

typedef enum {
   COBALT_GRAPH_VIEW_STAY = 0,
   COBALT_GRAPH_VIEW_BACK,
} cobalt_graph_view_action;

typedef struct {
   cobalt_graph_kind kind;
   int selected;
   int scroll;
   int last_visible;

   SDL_Rect hit[COBALT_ACTORS_MAX];
   int hit_index[COBALT_ACTORS_MAX];
   int hit_count;
   bool hit_valid;
} cobalt_graph_view;

void cobalt_graph_view_init(cobalt_graph_view *view);

/* Rewinds the cursor and, if the target list is empty, kicks off the first
 * fetch — the same "only fetch if there is nothing to show yet" rule the
 * timeline and notifications entry points already follow. */
void cobalt_graph_view_open(cobalt_graph_view *view, cobalt_graph_kind kind);

cobalt_graph_view_action cobalt_graph_view_update(cobalt_graph_view *view,
                                                  const cobalt_input *in);

void cobalt_graph_view_draw(cobalt_graph_view *view, cobalt_render *r,
                            cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
