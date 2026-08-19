#pragma once

/*
 * Account search (app.bsky.actor.searchActors) — not post search, which
 * Cobalt does not implement.
 *
 * Two sub-modes on one screen, the same shape compose.c already uses for
 * "edit, then something else": typing owns the whole screen via the on-screen
 * keyboard, and OK swaps to browsing the results list, which reuses the exact
 * row shape and draw code cobalt_graph_view already has for muted/blocked
 * accounts (avatar, name, handle) — a search result and a muted-account row
 * are the same shape for the same reason those two are the same shape as each
 * other. B from browsing returns to typing rather than leaving the screen, so
 * revising a query does not mean re-opening search from the menu.
 */

#include "atproto/actors.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COBALT_SEARCH_QUERY_MAX 128

typedef enum {
   COBALT_SEARCH_VIEW_STAY = 0,
   COBALT_SEARCH_VIEW_BACK,
   COBALT_SEARCH_VIEW_OPEN_PROFILE,
} cobalt_search_view_action;

typedef struct {
   char query[COBALT_SEARCH_QUERY_MAX];
   cobalt_keyboard kb;
   bool browsing;   /* false: editing the query; true: viewing results */

   int selected;
   int scroll;
   int last_visible;

   SDL_Rect hit[COBALT_ACTORS_MAX];
   int hit_index[COBALT_ACTORS_MAX];
   int hit_count;
   bool hit_valid;
} cobalt_search_view;

void cobalt_search_view_init(cobalt_search_view *view);

/* Rewinds to an empty query in typing mode. Unlike the graph lists, search
 * never fetches on open — there is nothing to fetch until a query exists. */
void cobalt_search_view_open(cobalt_search_view *view);

cobalt_search_view_action cobalt_search_view_update(cobalt_search_view *view,
                                                     const cobalt_input *in);

void cobalt_search_view_draw(cobalt_search_view *view, cobalt_render *r,
                             cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
