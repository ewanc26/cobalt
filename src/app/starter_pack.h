#pragma once

/*
 * The starter-pack screen: a pack header followed by its suggested accounts
 * and included feeds in one bounded list. Members page through the pack's
 * underlying list; feeds are opened in the ordinary feed view. B returns to
 * whichever screen opened the pack.
 *
 * Creation is intentionally absent. The issue scopes browse/use before create,
 * and Cobalt has no list editor to assemble app.bsky.graph.starterpack.
 */

#include "atproto/starter_packs.h"
#include "input/input.h"
#include "ui/listnav.h"
#include "ui/render.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   COBALT_STARTER_PACK_VIEW_STAY = 0,
   COBALT_STARTER_PACK_VIEW_BACK,
   COBALT_STARTER_PACK_VIEW_OPEN_PROFILE,
   COBALT_STARTER_PACK_VIEW_OPEN_FEED,
} cobalt_starter_pack_view_action;

typedef struct {
   cobalt_listnav nav;
   char uri[COBALT_POST_URI_MAX];
   char open_feed_uri[COBALT_POST_URI_MAX];
} cobalt_starter_pack_view;

void cobalt_starter_pack_view_init(cobalt_starter_pack_view *view);

/* Reset the cursor and remember which pack this screen is showing. The caller
 * starts the fetch; this function never blocks or touches the network. */
void cobalt_starter_pack_view_open(cobalt_starter_pack_view *view,
                                   const char *pack_uri);

cobalt_starter_pack_view_action
cobalt_starter_pack_view_update(cobalt_starter_pack_view *view,
                                const cobalt_input *in);

void cobalt_starter_pack_view_draw(cobalt_starter_pack_view *view,
                                   cobalt_render *r,
                                   cobalt_surface_id surface);

#ifdef __cplusplus
}
#endif
